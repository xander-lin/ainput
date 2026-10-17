//! PulseAudio capture, built on `libpulse-binding`.
//!
//! One thread per recording. `spawn` waits until the stream is actually ready,
//! so a missing microphone is reported immediately instead of surfacing later
//! as an empty result. `stop` only flips a flag: the thread finishes
//! preprocessing and writes the WAV on its own, then reports through the
//! completion callback. Nothing here blocks the fcitx5 event loop.
//!
//! Reading happens in our own poll loop rather than in a stream callback, so
//! the buffer is a plain `Vec` and stop latency stays around 2 ms.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use libpulse_binding as pulse;
use pulse::context::State as ContextState;
use pulse::mainloop::standard::{IterateResult, Mainloop};
use pulse::stream::State as StreamState;

use crate::diagnostic;
use crate::dsp;
use crate::error::{Category, Error, Result};
use crate::settings::Settings;
use crate::wav;

/// Bytes of 16 kHz mono S16 audio per millisecond.
///
/// The fragment size comes from settings (`BufferMs`, default 20). PulseAudio
/// echoes the request back verbatim (measured: 6/10/20/50/100 ms all granted
/// exactly), down to a ~5.4 ms hardware floor where it rounds up. The setting
/// therefore only bounds how much audio can sit in a partial fragment, and the
/// default is four times that floor.
const BYTES_PER_MS: u32 = 32;
/// How long to wait for the stream to reach READY before giving up.
const READY_TIMEOUT: Duration = Duration::from_secs(3);
/// Poll interval while recording; bounds stop latency.
const POLL_INTERVAL: Duration = Duration::from_millis(2);
/// No drain after a stop.
///
/// Measured: with peek/discard, PulseAudio delivers audio as it arrives, so at
/// the moment of stop the delivered sample count equals the recording
/// duration (0 ms in flight, five trials). Draining for N ms therefore does
/// not recover lost audio — it only appends N ms of *new* audio, padding every
/// recording and delaying recognition. A 20 ms drain does complete one final
/// fragment, but costs more latency than the fragment is worth for speech.
pub const TAIL_DRAIN: Duration = Duration::from_millis(0);

pub struct Captured {
    pub samples: Vec<i16>,
    pub wav_path: PathBuf,
    pub loudness: f64,
    pub elapsed: Duration,
}

pub struct Recording {
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl Recording {
    /// Signals the capture thread to finish. Returns immediately.
    pub fn stop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
    }

    /// True once preprocessing and the WAV write are done.
    pub fn is_finished(&mut self) -> bool {
        match self.thread.as_ref() {
            Some(handle) => handle.is_finished(),
            None => true,
        }
    }

    /// Waits for the capture thread. Used at shutdown only.
    pub fn join(&mut self) {
        if let Some(handle) = self.thread.take() {
            let _ = handle.join();
        }
    }
}

/// Per-user private directory for the intermediate WAV.
///
/// The recording is a plaintext copy of whatever the user said until the
/// provider returns, so it must not land in a world-readable shared /tmp with
/// a predictable name.
fn wav_path_for(recognition_id: u64) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("ainput-{}", unsafe {
        libc::geteuid()
    }));
    let path = dir.join(format!("cap_{}.wav", recognition_id));
    // The directory is created here rather than relying on wav::write, and
    // 0700 is set unconditionally so a pre-existing directory cannot widen it.
    if std::fs::create_dir_all(&dir).is_ok() {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700));
    }
    path
}

/// Starts recording without waiting for PulseAudio.
///
/// Waiting for READY here used to block the fcitx5 event loop for 40-50 ms on
/// every activation, during which no key was processed. Readiness and any
/// setup failure are now reported through `on_done` instead, which is the same
/// path a mid-recording failure takes.
pub fn spawn<F>(recognition_id: u64, settings: Settings, on_done: F) -> Result<Recording>
where
    F: FnOnce(u64, std::result::Result<Captured, Error>) + Send + 'static,
{
    let stop = Arc::new(AtomicBool::new(false));
    let stop_flag = stop.clone();

    let handle = thread::Builder::new()
        .name(format!("ainput-capture-{recognition_id}"))
        .spawn(move || run_capture(recognition_id, settings, stop_flag, on_done))
        .map_err(|e| Error::runtime(format!("cannot start capture thread: {e}")))?;

    Ok(Recording {
        stop,
        thread: Some(handle),
    })
}

fn sample_spec() -> pulse::sample::Spec {
    pulse::sample::Spec {
        format: pulse::sample::Format::S16le,
        channels: 1,
        rate: 16_000,
    }
}

fn iterate(mainloop: &mut Mainloop, block: bool) -> std::result::Result<(), Error> {
    match mainloop.iterate(block) {
        IterateResult::Success(_) => Ok(()),
        IterateResult::Quit(_) => Err(Error::audio_data("PulseAudio main loop quit")),
        IterateResult::Err(err) => Err(Error::audio_data(format!("PulseAudio error: {err}"))),
    }
}

fn drain(stream: &mut pulse::stream::Stream, samples: &mut Vec<i16>) {
    loop {
        match stream.peek() {
            Ok(pulse::stream::PeekResult::Data(bytes)) => {
                let usable = bytes.len() / 2;
                for index in 0..usable {
                    samples.push(i16::from_le_bytes([bytes[index * 2], bytes[index * 2 + 1]]));
                }
                if stream.discard().is_err() {
                    return;
                }
            }
            Ok(pulse::stream::PeekResult::Hole(bytes)) => {
                // A hole is silence, not missing audio.
                samples.extend(std::iter::repeat_n(0i16, bytes / 2));
                if stream.discard().is_err() {
                    return;
                }
            }
            Ok(pulse::stream::PeekResult::Empty) => return,
            Err(_) => return,
        }
    }
}

fn run_capture<F>(
    recognition_id: u64,
    settings: Settings,
    stop: Arc<AtomicBool>,
    on_done: F,
) where
    F: FnOnce(u64, std::result::Result<Captured, Error>),
{
    let started = Instant::now();
    let mut samples: Vec<i16> = Vec::with_capacity(16_000 * 8);

    let mut mainloop = match Mainloop::new() {
        Some(mainloop) => mainloop,
        None => {
            on_done(
                recognition_id,
                Err(Error::audio_data("cannot create PulseAudio main loop")),
            );
            return;
        }
    };

    let mut context = match pulse::context::Context::new(&mainloop, "ainput") {
        Some(context) => context,
        None => {
            on_done(
                recognition_id,
                Err(Error::audio_data("cannot create PulseAudio context")),
            );
            return;
        }
    };

    if let Err(err) = context.connect(None, pulse::context::FlagSet::NOFLAGS, None) {
        on_done(
            recognition_id,
            Err(Error::audio_data(format!(
                "cannot connect to PulseAudio: {err}"
            ))),
        );
        return;
    }

    loop {
        match context.get_state() {
            ContextState::Ready => break,
            ContextState::Failed | ContextState::Terminated => {
                let err = context.errno();
                on_done(
                    recognition_id,
                    Err(Error::audio_data(format!(
                        "PulseAudio context failed: {err}"
                    ))),
                );
                return;
            }
            _ => {}
        }
        if started.elapsed() > READY_TIMEOUT {
            on_done(
                recognition_id,
                Err(Error::audio_data("PulseAudio context did not become ready")),
            );
            return;
        }
        if let Err(error) = iterate(&mut mainloop, false) {
            on_done(recognition_id, Err(error));
            return;
        }
        thread::sleep(POLL_INTERVAL);
    }

    let spec = sample_spec();
    let mut stream = match pulse::stream::Stream::new(&mut context, "voice", &spec, None) {
        Some(stream) => stream,
        None => {
            on_done(
                recognition_id,
                Err(Error::audio_data("cannot create PulseAudio stream")),
            );
            return;
        }
    };

    let fragsize = settings.buffer_ms.max(1) as u32 * BYTES_PER_MS;
    let attr = pulse::def::BufferAttr {
        maxlength: u32::MAX,
        tlength: u32::MAX,
        prebuf: u32::MAX,
        minreq: u32::MAX,
        fragsize,
    };
    diagnostic::event(
        "audio",
        "capture_buffer",
        &[
            ("recognition_id", recognition_id.to_string()),
            ("buffer_ms", settings.buffer_ms.to_string()),
            ("fragsize_bytes", fragsize.to_string()),
        ],
    );
    if let Err(err) = stream.connect_record(
        None,
        Some(&attr),
        pulse::stream::FlagSet::ADJUST_LATENCY,
    ) {
        on_done(
            recognition_id,
            Err(Error::audio_data(format!("cannot open microphone: {err}"))),
        );
        return;
    }

    loop {
        match stream.get_state() {
            StreamState::Ready => break,
            StreamState::Failed | StreamState::Terminated => {
                let err = context.errno();
                on_done(
                    recognition_id,
                    Err(Error::audio_data(format!(
                        "PulseAudio stream failed: {err}"
                    ))),
                );
                return;
            }
            _ => {}
        }
        if started.elapsed() > READY_TIMEOUT {
            on_done(
                recognition_id,
                Err(Error::audio_data("PulseAudio stream did not become ready")),
            );
            return;
        }
        if let Err(error) = iterate(&mut mainloop, false) {
            on_done(recognition_id, Err(error));
            return;
        }
        thread::sleep(POLL_INTERVAL);
    }

    diagnostic::event(
        "audio",
        "capture_started",
        &[("recognition_id", recognition_id.to_string())],
    );

    let mut stream_failed = false;
    while !stop.load(Ordering::Relaxed) {
        if iterate(&mut mainloop, false).is_err() {
            stream_failed = true;
            break;
        }
        drain(&mut stream, &mut samples);
        if matches!(stream.get_state(), StreamState::Failed | StreamState::Terminated) {
            stream_failed = true;
            break;
        }
        thread::sleep(POLL_INTERVAL);
    }

    if TAIL_DRAIN > Duration::ZERO {
        // Nothing is in flight at stop on the systems we measured, so this
        // branch is off. It exists only so a future device that does buffer
        // can be tested without rewriting the loop.
        let drain_deadline = Instant::now() + TAIL_DRAIN;
        while Instant::now() < drain_deadline {
            let _ = iterate(&mut mainloop, false);
            let empty_before = matches!(stream.peek(), Ok(pulse::stream::PeekResult::Empty));
            drain(&mut stream, &mut samples);
            if empty_before {
                thread::sleep(Duration::from_millis(5));
            }
        }
    }

    let elapsed = started.elapsed();
    drop(stream);
    drop(context);
    drop(mainloop);

    diagnostic::event(
        "audio",
        "capture_stopped",
        &[
            ("recognition_id", recognition_id.to_string()),
            ("samples", samples.len().to_string()),
            ("elapsed_ms", elapsed.as_millis().to_string()),
        ],
    );

    if stream_failed {
        on_done(
            recognition_id,
            Err(Error::audio_data("microphone stream failed while recording")),
        );
        return;
    }
    if samples.is_empty() {
        on_done(recognition_id, Err(Error::audio_data("no audio captured")));
        return;
    }

    let gated = match dsp::preprocess(
        &mut samples,
        settings.denoise,
        settings.lufs_target,
        settings.speex_level,
        settings.crest_threshold,
    ) {
        Ok(gated) => gated,
        Err(error) => {
            // A configured denoiser failed; report it instead of quietly
            // sending undenoised audio.
            on_done(recognition_id, Err(error));
            return;
        }
    };

    if !gated.voice {
        on_done(
            recognition_id,
            Err(Error::new(Category::NoSpeech, "no speech detected")),
        );
        return;
    }

    let wav_path = wav_path_for(recognition_id);
    if let Err(error) = wav::write(&wav_path, &samples) {
        on_done(recognition_id, Err(error));
        return;
    }

    on_done(
        recognition_id,
        Ok(Captured {
            samples,
            wav_path,
            loudness: gated.loudness,
            elapsed,
        }),
    );
}
