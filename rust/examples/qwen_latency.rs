//! End-to-end latency of a real qwen recognition, without the GUI.
//!
//! Uses the credentials already configured for the addon, so it measures the
//! exact call path the addon uses. Run:
//!   cargo run --release --example qwen_latency [wav]
//!
//! Prints one line per attempt with the three stages the addon logs.

use std::time::Instant;

/// Applies the same preprocessing the capture thread applies, so what is sent
/// matches what the addon sends. Skipping this measures a different audio file
/// (raw microphone input, no loudness normalisation, no denoise, no trim) and
/// the provider rejects it as containing no words.
fn preprocess(path: &str) -> Vec<i16> {
    let mut samples = ainput_core::wav::read(std::path::Path::new(path)).expect("read wav");
    let before = samples.len();
    ainput_core::dsp::preprocess(
        &mut samples,
        ainput_core::settings::Denoise::Speexdsp,
        -16.0,
        -15,
        2.4,
    )
    .expect("preprocess");
    eprintln!(
        "preprocess: {:.1}s -> {:.1}s",
        before as f64 / 16_000.0,
        samples.len() as f64 / 16_000.0
    );
    samples
}

fn main() {
    let source = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "/tmp/opencode/rec/raw/seg09.wav".to_string());

    let samples = preprocess(&source);
    let path = std::env::temp_dir().join("ainput-latency-input.wav");
    ainput_core::wav::write(&path, &samples).unwrap();

    let cancel = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let credentials = ainput_core::settings::Credentials::default();
    let provider = ainput_core::smoke::create("qwen").expect("qwen");

    let mut timings = Vec::new();
    for attempt in 1..=6 {
        let started = Instant::now();
        let input = ainput_core::smoke::Input {
            wav_path: &path,
            cancel: &cancel,
            credentials: &credentials,
        };
        let result = provider.transcribe(&input);
        let took = started.elapsed().as_secs_f64() * 1000.0;
        timings.push(took);
        match result {
            Ok(text) => println!(
                "attempt {attempt}: total {took:7.1} ms  text_len={}  {:?}",
                text.len(),
                text.chars().take(40).collect::<String>()
            ),
            Err(err) => println!(
                "attempt {attempt}: total {took:7.1} ms  FAILED {:?}",
                err.detail.chars().take(60).collect::<String>()
            ),
        }
    }
    timings.sort_by(|a, b| a.partial_cmp(b).unwrap());
    println!(
        "\nmedian {:.1} ms   min {:.1}   max {:.1}",
        timings[timings.len() / 2],
        timings[0],
        timings[timings.len() - 1]
    );
    let _ = std::fs::remove_file(&path);
}
