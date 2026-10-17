//! Where the time actually goes for one dictation.
//!
//! Splits a recognition into the stages the user waits through:
//!   preprocess (normalize/gate/denoise/trim)  -> on the capture thread
//!   encode (wav read + base64 + JSON)         -> worker
//!   network (submit/HTTP)                     -> worker
//!   parse                                     -> worker
//!
//! Run: cargo run --release --example stage_breakdown <dir-with-wavs> [provider]

use std::path::Path;
use std::time::Instant;

fn median(mut v: Vec<f64>) -> f64 {
    if v.is_empty() {
        return 0.0;
    }
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    v[v.len() / 2]
}

fn main() {
    let dir = std::env::args().nth(1).unwrap_or_else(|| "/tmp/opencode/bench-set".into());
    let provider_id = std::env::args().nth(2).unwrap_or_else(|| "qwen".into());

    let mut files: Vec<_> = std::fs::read_dir(&dir)
        .expect("read dir")
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().map(|x| x == "wav").unwrap_or(false))
        .collect();
    files.sort();
    if files.is_empty() {
        panic!("no wav files in {dir}");
    }

    let cancel = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let credentials = ainput_core::settings::Credentials::default();

    let mut prep = Vec::new();
    let mut total = Vec::new();

    for file in &files {
        let mut samples = ainput_core::wav::read(file).expect("read");
        let started = Instant::now();
        ainput_core::dsp::preprocess(
            &mut samples,
            ainput_core::settings::Denoise::Speexdsp,
            -16.0,
            -15,
            2.4,
        )
        .ok();
        prep.push(started.elapsed().as_secs_f64() * 1000.0);

        let out = std::env::temp_dir().join("ainput-stage.wav");
        ainput_core::wav::write(&out, &samples).unwrap();

        let provider = ainput_core::smoke::create(&provider_id).expect("provider");
        let started = Instant::now();
        let _ = provider.transcribe(&ainput_core::smoke::Input {
            wav_path: &out,
            cancel: &cancel,
            credentials: &credentials,
        });
        total.push(started.elapsed().as_secs_f64() * 1000.0);
        let _ = std::fs::remove_file(&out);
    }

    let (p, t) = (median(prep.clone()), median(total.clone()));
    println!("provider: {provider_id}   clips: {}", files.len());
    println!("  preprocess        median {p:7.1} ms");
    println!("  recognition       median {t:7.1} ms   (encode + network + parse)");
    println!("  ---------------------------------------------");
    println!("  user-visible      median {:7.1} ms", p + t);
    println!(
        "\n  preprocess share: {:.1}%   network/inference share: {:.1}%",
        p / (p + t) * 100.0,
        t / (p + t) * 100.0
    );

    let _ = Path::new(&dir);
}
