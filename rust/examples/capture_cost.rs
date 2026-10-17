//! CPU cost of the capture poll loop.
//!
//! The loop wakes every 2 ms and calls peek/discard plus a PulseAudio mainloop
//! iteration. At 500 iterations per second this is not obviously free, and it
//! runs while the user is speaking (potentially for a long recording), so it
//! belongs in the profile even though it is not on the recognition path.

use std::time::{Duration, Instant};

fn main() {
    let mut samples: Vec<i16> = Vec::with_capacity(16_000 * 30);
    // Simulate the per-iteration work: decode a 20 ms fragment (the common
    // case with BufferMs=20) into the sample buffer.
    let fragment = [0u8; 640];
    let iterations = 500 * 10; // 10 s of recording
    let started = Instant::now();
    for _ in 0..iterations {
        for index in 0..fragment.len() / 2 {
            samples.push(i16::from_le_bytes([fragment[index * 2], fragment[index * 2 + 1]]));
        }
        // The loop also sleeps 2 ms; measured separately below.
        std::hint::black_box(&samples);
    }
    let work = started.elapsed();
    println!(
        "decode only: {iterations} fragments in {:?} -> {:.3} ms per second of audio",
        work,
        work.as_secs_f64() * 1000.0 / 10.0
    );

    let started = Instant::now();
    for _ in 0..iterations {
        std::thread::sleep(Duration::from_micros(2000));
    }
    println!(
        "sleep(2ms) x {iterations}: {:?} (wall clock is the recording length, not CPU)",
        started.elapsed()
    );
}
