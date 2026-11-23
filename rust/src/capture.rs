//! PulseAudio capture, built on `libpulse-binding`.
//!
//! Capture and preprocessing run on the job thread, never the event loop.
//!
//! Reading happens in our own poll loop rather than in a stream callback, so
//! the buffer is a plain `Vec` and stop latency stays around 2 ms.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
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
    pub wav_path: PathBuf,
}

/// Per-user private directory for the intermediate WAV.
///
/// The recording is a plaintext copy of whatever the user said until the
/// provider returns, so it must not land in a world-readable shared /tmp with
/// a predictable name.
fn wav_path_for(recognition_id: u64) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("ainput-{}", unsafe { libc::geteuid() }));
    let path = dir.join(format!("cap_{}_{}.wav", std::process::id(), recognition_id));
    // The directory is created here rather than relying on wav::write, and
    // 0700 is set unconditionally so a pre-existing directory cannot widen it.
    if std::fs::create_dir_all(&dir).is_ok() {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700));
    }
    path
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

pub fn run(
    recognition_id: u64,
    settings: Settings,
    stop: Arc<AtomicBool>,
    cancel: &AtomicBool,
) -> Result<Captured> {
    let cancelled = || cancel.load(Ordering::Acquire);
    if cancelled() {
        return Err(Error::timeout("cancelled"));
    }
    let started = Instant::now();
    let mut samples: Vec<i16> = Vec::with_capacity(16_000 * 8);

    let mut mainloop =
        Mainloop::new().ok_or_else(|| Error::audio_data("cannot create PulseAudio main loop"))?;
    let mut context = pulse::context::Context::new(&mainloop, "ainput")
        .ok_or_else(|| Error::audio_data("cannot create PulseAudio context"))?;
    context
        .connect(None, pulse::context::FlagSet::NOFLAGS, None)
        .map_err(|err| Error::audio_data(format!("cannot connect to PulseAudio: {err}")))?;

    loop {
        if cancelled() {
            return Err(Error::timeout("cancelled"));
        }
        match context.get_state() {
            ContextState::Ready => break,
            ContextState::Failed | ContextState::Terminated => {
                return Err(Error::audio_data(format!(
                    "PulseAudio context failed: {}",
                    context.errno()
                )));
            }
            _ => {}
        }
        if started.elapsed() > READY_TIMEOUT {
            return Err(Error::audio_data("PulseAudio context did not become ready"));
        }
        iterate(&mut mainloop, false)?;
        thread::sleep(POLL_INTERVAL);
    }

    let spec = sample_spec();
    let mut stream = pulse::stream::Stream::new(&mut context, "voice", &spec, None)
        .ok_or_else(|| Error::audio_data("cannot create PulseAudio stream"))?;

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
    stream
        .connect_record(None, Some(&attr), pulse::stream::FlagSet::ADJUST_LATENCY)
        .map_err(|err| Error::audio_data(format!("cannot open microphone: {err}")))?;

    loop {
        if cancelled() {
            return Err(Error::timeout("cancelled"));
        }
        match stream.get_state() {
            StreamState::Ready => break,
            StreamState::Failed | StreamState::Terminated => {
                return Err(Error::audio_data(format!(
                    "PulseAudio stream failed: {}",
                    context.errno()
                )));
            }
            _ => {}
        }
        if started.elapsed() > READY_TIMEOUT {
            return Err(Error::audio_data("PulseAudio stream did not become ready"));
        }
        iterate(&mut mainloop, false)?;
        thread::sleep(POLL_INTERVAL);
    }

    diagnostic::event(
        "audio",
        "capture_started",
        &[("recognition_id", recognition_id.to_string())],
    );

    let mut stream_failed = false;
    while !stop.load(Ordering::Acquire) && !cancelled() {
        if iterate(&mut mainloop, false).is_err() {
            stream_failed = true;
            break;
        }
        drain(&mut stream, &mut samples);
        if matches!(
            stream.get_state(),
            StreamState::Failed | StreamState::Terminated
        ) {
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

    if cancelled() {
        return Err(Error::timeout("cancelled"));
    }
    if stream_failed {
        return Err(Error::audio_data(
            "microphone stream failed while recording",
        ));
    }
    finish_recording(recognition_id, settings, samples, cancel)
}

pub(crate) fn finish_recording(
    recognition_id: u64,
    settings: Settings,
    mut samples: Vec<i16>,
    cancel: &AtomicBool,
) -> Result<Captured> {
    if samples.is_empty() {
        return Err(Error::audio_data("no audio captured"));
    }

    let gated = dsp::preprocess_cancellable(
        &mut samples,
        settings.denoise,
        settings.lufs_target,
        settings.speex_level,
        settings.crest_threshold,
        cancel,
    )?;
    let output_peak = samples.iter().map(|&s| (s as i32).abs()).max().unwrap_or(0);
    eprintln!(
        "Ainput: id={recognition_id} audio input_peak={} output_peak={output_peak} loudness_lufs={:.1} gain={:.2} peak_frame_rms={:.1} active_ms={} crest_threshold={} voice={} denoise={}",
        gated.input_peak, gated.loudness, gated.gain, gated.peak_frame_rms(), gated.active_ms(),
        settings.crest_threshold, gated.voice, settings.denoise.as_str()
    );

    if gated.input_peak == 0 {
        return Err(Error::audio_data(
            "microphone returned only zero samples; check the input source and mute",
        ));
    }
    if !gated.voice {
        return Err(Error::new(Category::NoSpeech, "no speech detected"));
    }

    if cancel.load(Ordering::Acquire) {
        return Err(Error::timeout("cancelled"));
    }
    let wav_path = wav_path_for(recognition_id);
    if let Err(error) = wav::write(&wav_path, &samples) {
        let _ = std::fs::remove_file(&wav_path);
        return Err(error);
    }

    Ok(Captured { wav_path })
}
