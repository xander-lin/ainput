//! Cost of the connection setup versus the request itself.
//!
//! The cloud providers used to build a fresh `Easy` per request. libcurl keeps
//! its connection cache on the easy handle, so that reconnects and re-runs the
//! TLS handshake every time. This measures the difference on a real endpoint
//! with a realistic body (a 5 s recording, base64-encoded).
//!
//! Run: cargo run --release --example http_reuse

use std::time::{Duration, Instant};

const URL: &str =
    "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";

/// Shaped like a real payload: a 5 s 16 kHz mono recording, base64 in a data
/// URI. The body size drives upload time and therefore the variance, so using
/// "{}" would measure something the addon never sends.
fn body(seconds: usize) -> String {
    let pcm_len = 16_000 * 2 * seconds;
    let mut bytes = Vec::with_capacity(pcm_len + 44);
    bytes.extend_from_slice(b"RIFF");
    bytes.extend_from_slice(&((36 + pcm_len) as u32).to_le_bytes());
    bytes.extend_from_slice(b"WAVEfmt ");
    bytes.extend_from_slice(&16u32.to_le_bytes());
    bytes.extend_from_slice(&1u16.to_le_bytes());
    bytes.extend_from_slice(&1u16.to_le_bytes());
    bytes.extend_from_slice(&16_000u32.to_le_bytes());
    bytes.extend_from_slice(&32_000u32.to_le_bytes());
    bytes.extend_from_slice(&2u16.to_le_bytes());
    bytes.extend_from_slice(&16u16.to_le_bytes());
    bytes.extend_from_slice(b"data");
    bytes.extend_from_slice(&(pcm_len as u32).to_le_bytes());
    for index in 0..pcm_len {
        // Deterministic noise: compresses like audio, so the body size here is
        // representative rather than an idealised stream of zeros.
        bytes.push((index.wrapping_mul(2654435761) >> 13) as u8);
    }
    const TABLE: &[u8; 64] =
        b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut b64 = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let triple = ((chunk[0] as u32) << 16)
            | ((chunk.get(1).copied().unwrap_or(0) as u32) << 8)
            | chunk.get(2).copied().unwrap_or(0) as u32;
        b64.push(TABLE[(triple >> 18) as usize & 63] as char);
        b64.push(TABLE[(triple >> 12) as usize & 63] as char);
        b64.push(if chunk.len() > 1 {
            TABLE[(triple >> 6) as usize & 63] as char
        } else {
            '='
        });
        b64.push(if chunk.len() > 2 {
            TABLE[triple as usize & 63] as char
        } else {
            '='
        });
    }
    format!(
        r#"{{"model":"qwen-audio-3.1-asr-flash","input":{{"messages":[{{"content":[{{"audio":"data:audio/wav;base64,{b64}"}}],"role":"user"}}]}}}}"#
    )
}

fn median(mut values: Vec<f64>) -> f64 {
    values.sort_by(|a, b| a.partial_cmp(b).unwrap());
    values[values.len() / 2]
}

fn main() {
    let secs: usize = std::env::args().nth(1).and_then(|a| a.parse().ok()).unwrap_or(5);
    let body = body(secs);
    let cancel = std::sync::atomic::AtomicBool::new(false);
    println!("body: {} KiB ({secs} s recording)", body.len() / 1024);
    println!("no API key is sent, so the server answers 401 — that is fine: the");
    println!("measurement is connection setup plus upload plus response.\n");

    const RUNS: usize = 8;

    let mut fresh = Vec::new();
    for index in 0..RUNS {
        let mut agent = ainput_core::http::Agent::new();
        let started = Instant::now();
        let _ = agent.post_json(
            URL,
            &[("Content-Type", "application/json")],
            &body,
            Duration::from_secs(30),
            &cancel,
        );
        let took = started.elapsed().as_secs_f64() * 1000.0;
        fresh.push(took);
        if index == 0 {
            println!("  (first sample discarded from nothing; kept for honesty)");
        }
    }

    let mut reused = Vec::new();
    let mut agent = ainput_core::http::Agent::new();
    for _ in 0..RUNS {
        let started = Instant::now();
        let _ = agent.post_json(
            URL,
            &[("Content-Type", "application/json")],
            &body,
            Duration::from_secs(30),
            &cancel,
        );
        reused.push(started.elapsed().as_secs_f64() * 1000.0);
    }

    let fresh_median = median(fresh.clone());
    let reused_median = median(reused.clone());
    println!("new Easy per request : median {:6.1} ms", fresh_median);
    println!("one reused Easy      : median {:6.1} ms", reused_median);
    println!(
        "difference           : {:6.1} ms ({:.0}% of the fresh-connection time)",
        fresh_median - reused_median,
        (fresh_median - reused_median) / fresh_median * 100.0
    );
}
