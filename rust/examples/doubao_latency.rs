//! Latency of a real doubao recognition (submit + poll), stages split.
//! Run: cargo run --release --features diagnostics --example doubao_latency <wav>
use std::time::Instant;

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
    .ok();
    let path = std::env::temp_dir().join("ainput-db.wav");
    ainput_core::wav::write(&path, &samples).unwrap();
    eprintln!("input {:.1}s", samples.len() as f64 / 16_000.0);

    let cancel = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let credentials = ainput_core::settings::Credentials::default();
    let provider = ainput_core::smoke::create("doubao").expect("doubao");

    let mut times = Vec::new();
    for attempt in 1..=5 {
        let started = Instant::now();
        let result = provider.transcribe(&ainput_core::smoke::Input {
            wav_path: &path,
            cancel: &cancel,
            credentials: &credentials,
        });
        let took = started.elapsed().as_secs_f64() * 1000.0;
        times.push(took);
        match result {
            Ok(text) => println!(
                "attempt {attempt}: {took:7.1} ms  len={}  {:?}",
                text.len(),
                text.chars().take(30).collect::<String>()
            ),
            Err(e) => println!(
                "attempt {attempt}: {took:7.1} ms  FAILED {:?}",
                e.detail.chars().take(60).collect::<String>()
            ),
        }
    }
    times.sort_by(|a, b| a.partial_cmp(b).unwrap());
    println!("\nmedian {:.1} ms  min {:.1}  max {:.1}", times[times.len()/2], times[0], times[times.len()-1]);
    let _ = std::fs::remove_file(&path);
}
