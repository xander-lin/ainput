//! Real-recognition A/B: cold connection vs kept-alive.
//!
//! `examples/http_reuse.rs` measures the transport alone (fast 401s); that
//! overstates the gain because the server-side inference dominates an actual
//! recognition. This measures what the user would feel.

use std::time::Instant;

fn median(mut v: Vec<f64>) -> f64 {
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    v[v.len() / 2]
}

fn main() {
    let dir = std::env::args().nth(1).unwrap_or_else(|| "/tmp/opencode/bench-set".into());
    let mut files: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.extension().map(|x| x == "wav").unwrap_or(false))
        .collect();
    files.sort();

    let cancel = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let credentials = ainput_core::settings::Credentials::default();
    let provider = ainput_core::smoke::create("qwen").unwrap();

    let run = |reset: bool| -> Vec<f64> {
        let mut out = Vec::new();
        for file in &files {
            let mut samples = ainput_core::wav::read(file).unwrap();
            ainput_core::dsp::preprocess(
                &mut samples,
                ainput_core::settings::Denoise::Speexdsp,
                -16.0,
                -15,
                2.4,
            )
            .ok();
            let path = std::env::temp_dir().join("ainput-reuse.wav");
            ainput_core::wav::write(&path, &samples).unwrap();
            if reset {
                ainput_core::http::reset_agent();
            }
            let started = Instant::now();
            let _ = provider.transcribe(&ainput_core::smoke::Input {
                wav_path: &path,
                cancel: &cancel,
                credentials: &credentials,
            });
            out.push(started.elapsed().as_secs_f64() * 1000.0);
            let _ = std::fs::remove_file(&path);
        }
        out
    };

    let cold = run(true);
    let warm = run(false);
    let (c, w) = (median(cold.clone()), median(warm.clone()));
    println!("cold connection (old behaviour): median {c:7.1} ms");
    println!("kept-alive connection          : median {w:7.1} ms");
    println!("gain                           : {:7.1} ms ({:.0}%)", c - w, (c - w) / c * 100.0);
}
