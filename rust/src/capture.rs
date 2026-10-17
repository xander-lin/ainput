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
use std::sync::mpsc;
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
const BYTES_PER_MS: u32 = 32;

/// Fragment size requested from PulseAudio.
///
/// The server delivers at most this many bytes per wakeup and the request is
/// echoed back verbatim (measured: 6/10/20/50/100 ms requests all granted
/// exactly), down to the ~5.4 ms hardware floor where it rounds up.
/// It is not a setting: there is no per-device optimum, and the value only
/// bounds how much audio can sit in a partial fragment at any instant.
///
/// 20 ms is chosen over the floor because it is 4x the measured floor (safe on
/// slower devices) while keeping the capture loop at ~50 wakeups/second, which
/// matters less than the 2 ms poll interval already in place.
pub const BUFFER_MS: u32 = 20;
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

    /// Waits for the capture thread. Used at shutdown only.
    pub fn join(&mut self) {
        if let Some(handle) = self.thread.take() {
            let _ = handle.join();
        }
    }
}

fn wav_path_for(recognition_id: u64) -> PathBuf {
    std::env::temp_dir().join(format!(
        "ainput_cap_{}_{}.wav",
        std::process::id(),
        recognition_id
    ))
}

/// Starts recording. Blocks until PulseAudio is ready or `READY_TIMEOUT`.
pub fn spawn<F>(recognition_id: u64, settings: Settings, on_done: F) -> Result<Recording>
where
    F: FnOnce(u64, std::result::Result<Captured, Error>) + Send + 'static,
{
    let stop = Arc::new(AtomicBool::new(false));
    let stop_flag = stop.clone();
    let (ready_tx, ready_rx) = mpsc::channel::<std::result::Result<(), Error>>();

    let handle = thread::Builder::new()
        .name(format!("ainput-capture-{recognition_id}"))
        .spawn(move || run_capture(recognition_id, settings, stop_flag, ready_tx, on_done))
        .map_err(|e| Error::runtime(format!("cannot start capture thread: {e}")))?;

    match ready_rx.recv_timeout(READY_TIMEOUT) {
        Ok(Ok(())) => Ok(Recording {
            stop,
            thread: Some(handle),
        }),
        Ok(Err(error)) => {
            let _ = handle.join();
            Err(error)
        }
        Err(_) => {
            stop.store(true, Ordering::Relaxed);
            let _ = handle.join();
            Err(Error::audio_data(
                "PulseAudio did not become ready within 3 s",
            ))
        }
    }
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
                samples.extend(std::iter::repeat(0i16).take(bytes / 2));
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
    ready: mpsc::Sender<std::result::Result<(), Error>>,
    on_done: F,
) where
    F: FnOnce(u64, std::result::Result<Captured, Error>),
{
    let started = Instant::now();
    let mut samples: Vec<i16> = Vec::with_capacity(16_000 * 8);

    let mut mainloop = match Mainloop::new() {
        Some(mainloop) => mainloop,
        None => {
            let _ = ready.send(Err(Error::audio_data("cannot create PulseAudio main loop")));
            return;
        }
    };

    let mut context = match pulse::context::Context::new(&mainloop, "ainput") {
        Some(context) => context,
        None => {
            let _ = ready.send(Err(Error::audio_data("cannot create PulseAudio context")));
            return;
        }
    };

    if let Err(err) = context.connect(None, pulse::context::FlagSet::NOFLAGS, None) {
        let _ = ready.send(Err(Error::audio_data(format!(
            "cannot connect to PulseAudio: {err}"
        ))));
        return;
    }

    loop {
        match context.get_state() {
            ContextState::Ready => break,
            ContextState::Failed | ContextState::Terminated => {
                let err = context.errno();
                let _ = ready.send(Err(Error::audio_data(format!(
                    "PulseAudio context failed: {err}"
                ))));
                return;
            }
            _ => {}
        }
        if started.elapsed() > READY_TIMEOUT {
            let _ = ready.send(Err(Error::audio_data(
                "PulseAudio context did not become ready",
            )));
            return;
        }
        if let Err(error) = iterate(&mut mainloop, false) {
            let _ = ready.send(Err(error));
            return;
        }
        thread::sleep(POLL_INTERVAL);
    }

    let spec = sample_spec();
    let mut stream = match pulse::stream::Stream::new(&mut context, "voice", &spec, None) {
        Some(stream) => stream,
        None => {
            let _ = ready.send(Err(Error::audio_data("cannot create PulseAudio stream")));
            return;
        }
    };

    let fragsize = BUFFER_MS * BYTES_PER_MS;
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
            ("buffer_ms", BUFFER_MS.to_string()),
            ("fragsize_bytes", fragsize.to_string()),
        ],
    );
    if let Err(err) = stream.connect_record(
        None,
        Some(&attr),
        pulse::stream::FlagSet::ADJUST_LATENCY,
    ) {
        let _ = ready.send(Err(Error::audio_data(format!(
            "cannot open microphone: {err}"
        ))));
        return;
    }

    loop {
        match stream.get_state() {
            StreamState::Ready => break,
            StreamState::Failed | StreamState::Terminated => {
                let err = context.errno();
                let _ = ready.send(Err(Error::audio_data(format!(
                    "PulseAudio stream failed: {err}"
                ))));
                return;
            }
            _ => {}
        }
        if started.elapsed() > READY_TIMEOUT {
            let _ = ready.send(Err(Error::audio_data(
                "PulseAudio stream did not become ready",
            )));
            return;
        }
        if let Err(error) = iterate(&mut mainloop, false) {
            let _ = ready.send(Err(error));
            return;
        }
        thread::sleep(POLL_INTERVAL);
    }

    let _ = ready.send(Ok(()));
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

    // Keep whatever the server already handed over, so the final syllable is
    // not clipped by an immediate disconnect.
    let drain_deadline = Instant::now() + TAIL_DRAIN;
    while Instant::now() < drain_deadline {
        let _ = iterate(&mut mainloop, false);
        let empty_before = matches!(stream.peek(), Ok(pulse::stream::PeekResult::Empty));
        drain(&mut stream, &mut samples);
        if empty_before {
            thread::sleep(Duration::from_millis(5));
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
