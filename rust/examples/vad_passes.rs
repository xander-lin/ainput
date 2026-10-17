//! How much of the DSP time is repeated VAD work?
//!
//! `preprocess` runs three speexdsp VAD passes over the same audio:
//!   has_voice()  -> gate (prob_start 90/70)
//!   trim_silence() -> leading edge + trailing edge (prob_start 80/50)
//! The trimmer's two passes could be one, and the gate and the trimmer could
//! share a single pass if the thresholds were unified. This measures the
//! individual cost to see whether that is worth changing.

use std::time::Instant;

fn speech(seconds: f64) -> Vec<i16> {
    let rate = 16_000.0;
    let total = (seconds * rate) as usize;
    (0..total)
        .map(|index| {
            let t = index as f64 / rate;
            // Speech-like: syllabic bursts with silence around them, so the
            // VAD has an actual decision to make.
            let inside = t > 0.5 && t < seconds - 0.5;
            if inside && ((t * 4.0) as usize).is_multiple_of(2) {
                let phase = t * 2.0 * std::f64::consts::PI * 220.0;
                ((phase.sin()) * 24000.0) as i16
            } else {
                0
            }
        })
        .collect()
}

fn main() {
    // Prefer a real recording: a synthetic tone either passes the crest gate
    // or short-circuits before the VAD, so it cannot measure the VAD at all.
    let from_file = std::env::args().nth(1);
    let real = from_file.as_ref().map(|path| {
        ainput_core::wav::read(std::path::Path::new(path)).expect("read wav")
    });

    let lengths: Vec<(String, Vec<i16>, f64)> = match real {
        Some(samples) => {
            let seconds = samples.len() as f64 / 16_000.0;
            vec![(format!("{} ({:.1}s)", from_file.unwrap(), seconds), samples, seconds)]
        }
        None => [1.0, 3.0, 5.0, 10.0]
            .iter()
            .map(|s| (format!("{s:.0}s synthetic"), speech(*s), *s))
            .collect(),
    };

    for (label, base, seconds) in lengths {
        let runs = 30;

        let mut gate = Vec::new();
        for _ in 0..runs {
            let started = Instant::now();
            ainput_core::dsp::has_voice(&base, 2.4);
            gate.push(started.elapsed().as_secs_f64() * 1000.0);
        }

        let mut trim = Vec::new();
        for _ in 0..runs {
            let mut samples = base.clone();
            let started = Instant::now();
            ainput_core::dsp::trim_silence(&mut samples);
            trim.push(started.elapsed().as_secs_f64() * 1000.0);
        }

        let mut pre = Vec::new();
        for _ in 0..runs {
            let mut samples = base.clone();
            let started = Instant::now();
            ainput_core::dsp::preprocess(
                &mut samples,
                ainput_core::settings::Denoise::Speexdsp,
                -16.0,
                -15,
                2.4,
            )
            .ok();
            pre.push(started.elapsed().as_secs_f64() * 1000.0);
        }
        let _ = seconds;

        let med = |mut v: Vec<f64>| {
            v.sort_by(|a, b| a.partial_cmp(b).unwrap());
            v[v.len() / 2]
        };
        println!(
            "{:<22} gate(1x VAD) {:7.2}ms   trim(2x VAD) {:7.2}ms   full {:7.2}ms",
            label,
            med(gate),
            med(trim),
            med(pre)
        );
    }
}
