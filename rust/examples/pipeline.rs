//! Per-stage timing for one dictation, without a network call.
//!
//! Run with:
//!   cargo run --release --example pipeline
//!
//! This exists because the addon has no usable external profiler story on this
//! machine (`perf` is not installed and `perf_event_paranoid=2` restricts
//! kernel sampling), and because "recognition feels slow" needs to be split
//! into capture, preprocess, encode and request time before anything is
//! optimised.
//!
//! The test signal is synthesised, not recorded: a tone burst with silence
//! around it, so the VAD and the silence trimmer both do real work. It is
//! deterministic, so two runs are comparable.

use std::time::{Duration, Instant};

fn signal(seconds: f64) -> Vec<i16> {
    // 0.3 s of quiet, then syllabic bursts, then quiet again.
    //
    // The bursts must have a high crest factor: a steady sine has crest ~1.4
    // and `has_voice` rejects it before running the VAD, which made an earlier
    // version of this benchmark measure a function that returned immediately.
    // Each "syllable" is a fast-decaying impulse hammered into a short window,
    // giving peak/rms well above the 2.4 default gate.
    let rate = 16_000.0;
    let total = (seconds * rate) as usize;
    let mut out = Vec::with_capacity(total);
    let mut index = 0usize;
    while index < total {
        let t = index as f64 / rate;
        let inside = t > 0.3 && t < seconds - 0.3;
        if inside {
            // 180 ms of speech-like activity, then 90 ms of quiet.
            for offset in 0..((0.18 * rate) as usize) {
                if index + offset >= total {
                    break;
                }
                let phase = offset as f64 / rate;
                let envelope = (-12.0 * phase).exp();
                let value = (2.0 * std::f64::consts::PI * 220.0 * phase).sin()
                    * envelope
                    * 24000.0;
                out.push(value as i16);
            }
            index += (0.18 * rate) as usize + (0.09 * rate) as usize;
            out.extend(std::iter::repeat_n(0i16, (0.09 * rate) as usize));
        } else {
            out.push(0);
            index += 1;
        }
    }
    out
}

fn median(mut values: Vec<Duration>) -> Duration {
    values.sort();
    values[values.len() / 2]
}

fn time_it<T>(runs: usize, mut body: impl FnMut() -> T) -> (Duration, T) {
    let mut samples = Vec::with_capacity(runs);
    let mut last = None;
    for _ in 0..runs {
        let started = Instant::now();
        last = Some(body());
        samples.push(started.elapsed());
    }
    (median(samples), last.unwrap())
}

fn main() {
    println!(
        "{:>6}  {:>10}  {:>10}  {:>10}  {:>10}  {:>10}  {:>10}  {:>10}",
        "secs", "normalize", "gate", "speexdsp", "trim", "FULL", "base64", "wav"
    );

    for seconds in [0.5, 1.0, 2.0, 5.0, 10.0] {
        let base = signal(seconds);
        let kb = base.len() * 2 / 1024;

        let (normalize, _) = time_it(20, || {
            let mut samples = base.clone();
            ainput_core::dsp::normalize(&mut samples, -16.0)
        });

        let (gate, gate_passed) =
            time_it(20, || ainput_core::dsp::has_voice(&base, 2.4));
        if !gate_passed {
            eprintln!("WARNING: gate rejected the benchmark signal at {seconds}s");
        }

        let (speex, _) = time_it(20, || {
            let mut samples = base.clone();
            ainput_core::dsp::denoise_speexdsp(&mut samples, -15);
            samples
        });

        let (trim, _) = time_it(20, || {
            let mut samples = base.clone();
            ainput_core::dsp::trim_silence(&mut samples);
            samples.len()
        });

        // base64 is measured through the same 3-byte loop doubao uses; the
        // function is private, so this mirrors its shape rather than calling it.
        let wav_bytes = {
            let dir = std::env::temp_dir().join("ainput-bench");
            std::fs::create_dir_all(&dir).unwrap();
            let path = dir.join("bench.wav");
            ainput_core::wav::write(&path, &base).unwrap();
            let bytes = std::fs::read(&path).unwrap();
            let _ = std::fs::remove_file(&path);
            bytes
        };
        let (wav, _) = time_it(20, || {
            let dir = std::env::temp_dir().join("ainput-bench");
            let path = dir.join("bench.wav");
            ainput_core::wav::write(&path, &base).unwrap();
            let _ = std::fs::remove_file(&path);
        });

        let (full, _) = time_it(20, || {
            let mut samples = base.clone();
            ainput_core::dsp::preprocess(&mut samples, ainput_core::settings::Denoise::Speexdsp, -16.0, -15, 2.4)
                .map(|g| g.voice)
                .unwrap_or(false)
        });

        let (b64, _) = time_it(20, || {
            const TABLE: &[u8; 64] =
                b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            let mut out = String::with_capacity(wav_bytes.len().div_ceil(3) * 4);
            for chunk in wav_bytes.chunks(3) {
                let b0 = chunk[0] as u32;
                let b1 = *chunk.get(1).unwrap_or(&0) as u32;
                let b2 = *chunk.get(2).unwrap_or(&0) as u32;
                let triple = (b0 << 16) | (b1 << 8) | b2;
                out.push(TABLE[(triple >> 18) as usize & 63] as char);
                out.push(TABLE[(triple >> 12) as usize & 63] as char);
                out.push(if chunk.len() > 1 {
                    TABLE[(triple >> 6) as usize & 63] as char
                } else {
                    '='
                });
                out.push(if chunk.len() > 2 {
                    TABLE[triple as usize & 63] as char
                } else {
                    '='
                });
            }
            out.len()
        });

        let ms = |d: Duration| d.as_secs_f64() * 1000.0;
        println!(
            "{:>6.1}  {:>9.3}ms  {:>9.3}ms  {:>9.3}ms  {:>9.3}ms  {:>9.3}ms  {:>9.3}ms  {:>9.3}ms   ({} KiB audio)",
            seconds,
            ms(normalize),
            ms(gate),
            ms(speex),
            ms(trim),
            ms(full),
            ms(b64),
            ms(wav),
            kb
        );
    }

    println!();
    println!("decode cost of the capture loop (per 20 ms fragment):");
    let (decode, _) = time_it(200, || {
        let bytes: Vec<u8> = (0..640).map(|i| i as u8).collect();
        let mut samples = Vec::with_capacity(320);
        for index in 0..bytes.len() / 2 {
            samples.push(i16::from_le_bytes([bytes[index * 2], bytes[index * 2 + 1]]));
        }
        samples
    });
    println!(
        "  {:.3} ms per fragment; 1 s of audio = 50 fragments = {:.2} ms",
        decode.as_secs_f64() * 1000.0,
        decode.as_secs_f64() * 1000.0 * 50.0
    );
}
