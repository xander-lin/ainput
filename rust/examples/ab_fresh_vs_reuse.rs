//! A/B: the addon's real qwen path, with and without connection reuse.
//!
//! `qwen_latency` measures the shared `http::post_json`, which reuses the
//! connection. To measure the old behaviour the connection must be dropped
//! between requests, which `Agent::new()` does (a fresh handle has an empty
//! connection cache).
//!
//! Run: cargo run --release --example ab_fresh_vs_reuse <wav>

use std::time::Instant;

fn median(mut v: Vec<f64>) -> f64 {
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    v[v.len() / 2]
}

fn main() {
    let source = std::env::args().nth(1).expect("usage: <wav>");
    let mut samples = ainput_core::wav::read(std::path::Path::new(&source)).expect("read");
    ainput_core::dsp::preprocess(
        &mut samples,
        ainput_core::settings::Denoise::Speexdsp,
        -16.0,
        -15,
        2.4,
    )
    .expect("preprocess");
    let path = std::env::temp_dir().join("ainput-ab.wav");
    ainput_core::wav::write(&path, &samples).unwrap();
    eprintln!("input: {:.1}s", samples.len() as f64 / 16_000.0);

    // The body/payload cost is identical in both arms; only the connection
    // lifetime differs.
    let cancel = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let credentials = ainput_core::settings::Credentials::default();
    let provider = ainput_core::smoke::create("qwen").expect("qwen");

    const RUNS: usize = 6;
    let mut fresh = Vec::new();
    let mut reused = Vec::new();

    for _ in 0..RUNS {
        ainput_core::http::reset_agent();
        let started = Instant::now();
        let _ = provider.transcribe(&ainput_core::smoke::Input {
            wav_path: &path,
            cancel: &cancel,
            credentials: &credentials,
        });
        fresh.push(started.elapsed().as_secs_f64() * 1000.0);
    }

    for _ in 0..RUNS {
        let started = Instant::now();
        let _ = provider.transcribe(&ainput_core::smoke::Input {
            wav_path: &path,
            cancel: &cancel,
            credentials: &credentials,
        });
        reused.push(started.elapsed().as_secs_f64() * 1000.0);
    }

    let _ = std::fs::remove_file(&path);
    let (f, r) = (median(fresh.clone()), median(reused.clone()));
    println!("cold connection each time : median {f:7.1} ms  min {:7.1}  max {:7.1}", fresh.iter().cloned().fold(f64::MAX, f64::min), fresh.iter().cloned().fold(f64::MIN, f64::max));
    println!("kept-alive connection     : median {r:7.1} ms  min {:7.1}  max {:7.1}", reused.iter().cloned().fold(f64::MAX, f64::min), reused.iter().cloned().fold(f64::MIN, f64::max));
    println!("saved                     : {:7.1} ms ({:.0}%)", f - r, (f - r) / f * 100.0);
}
