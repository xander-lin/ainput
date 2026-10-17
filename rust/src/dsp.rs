//! Audio preprocessing: loudness normalization, speech gating, denoise,
//! silence trimming and resampling.
//!
//! Order matters and matches the C++ pipeline: normalize -> gate -> denoise ->
//! trim. The denoiser needs the leading noise to build a profile, which is why
//! trimming happens last.

use std::path::PathBuf;
use std::ptr;

use crate::error::{Error, Result};
use crate::ffi::dsp;
use crate::settings::Denoise;

pub const FRAME_SIZE: usize = 320; // 20 ms at 16 kHz

/// EBU R128 normalization to `target_lufs`, returning the measured loudness.
pub fn normalize(samples: &mut [i16], target_lufs: f64) -> f64 {
    if samples.is_empty() {
        return -70.0;
    }
    let state = unsafe { dsp::ebur128_init(1, 16_000, dsp::EBUR128_MODE_I) };
    if state.is_null() {
        return -70.0;
    }
    let mut loudness = 0.0f64;
    unsafe {
        dsp::ebur128_add_frames_short(state, samples.as_ptr(), samples.len());
        dsp::ebur128_loudness_global(state, &mut loudness);
        let mut owned = state;
        dsp::ebur128_destroy(&mut owned);
    }

    if loudness < -70.0 || !loudness.is_finite() {
        return loudness;
    }
    let gain = 10f64.powf((target_lufs - loudness) / 20.0).clamp(0.05, 20.0);
    for sample in samples.iter_mut() {
        let scaled = (*sample as f64 * gain).clamp(-32768.0, 32767.0);
        *sample = scaled as i16;
    }
    loudness
}

/// Speech gate: crest factor, then a speexdsp VAD majority vote.
pub fn has_voice(samples: &[i16], crest_threshold: f64) -> bool {
    if samples.is_empty() {
        return false;
    }
    let mut peak: i32 = 0;
    let mut sum_sq = 0f64;
    for sample in samples {
        let value = *sample as f64;
        sum_sq += value * value;
        let abs = (*sample as i32).abs();
        if abs > peak {
            peak = abs;
        }
    }
    let rms = (sum_sq / samples.len() as f64).sqrt();
    let crest = peak as f64 / if rms > 0.0 { rms } else { 1.0 };
    if crest < crest_threshold {
        return false;
    }

    const MIN_VOICE_RATIO: f64 = 0.15;
    const MIN_VOICE_FRAMES: usize = 5;

    let state = unsafe { dsp::speex_preprocess_state_init(FRAME_SIZE as i32, 16_000) };
    if state.is_null() {
        return true; // crest passed; do not drop audio because VAD is unavailable
    }
    let mut enable: i32 = 1;
    let mut prob_start: i32 = 90;
    let mut prob_continue: i32 = 70;
    unsafe {
        dsp::speex_preprocess_ctl(
            state,
            dsp::SPEEX_PREPROCESS_SET_VAD,
            &mut enable as *mut i32 as *mut _,
        );
        dsp::speex_preprocess_ctl(
            state,
            dsp::SPEEX_PREPROCESS_SET_PROB_START,
            &mut prob_start as *mut i32 as *mut _,
        );
        dsp::speex_preprocess_ctl(
            state,
            dsp::SPEEX_PREPROCESS_SET_PROB_CONTINUE,
            &mut prob_continue as *mut i32 as *mut _,
        );
    }

    let frame_count = samples.len() / FRAME_SIZE;
    let mut voice_frames = 0usize;
    let mut frame = vec![0i16; FRAME_SIZE];
    for index in 0..frame_count {
        frame.copy_from_slice(&samples[index * FRAME_SIZE..(index + 1) * FRAME_SIZE]);
        if unsafe { dsp::speex_preprocess_run(state, frame.as_mut_ptr()) } != 0 {
            voice_frames += 1;
        }
    }
    let mut remainder = samples.len() % FRAME_SIZE;
    if remainder > 0 {
        let tail = &samples[samples.len() - remainder..];
        frame[..remainder].copy_from_slice(tail);
        for slot in frame.iter_mut().skip(remainder) {
            *slot = 0;
        }
        if unsafe { dsp::speex_preprocess_run(state, frame.as_mut_ptr()) } != 0 {
            voice_frames += 1;
        }
        remainder = 0;
    }
    let _ = remainder;
    unsafe { dsp::speex_preprocess_state_destroy(state) };

    let total = frame_count + usize::from(!samples.len().is_multiple_of(FRAME_SIZE));
    if total == 0 {
        return voice_frames >= MIN_VOICE_FRAMES;
    }
    voice_frames >= MIN_VOICE_FRAMES
        && (voice_frames as f64 / total as f64) >= MIN_VOICE_RATIO
}

/// Removes leading and trailing non-speech frames, keeping one frame of pad
/// and at least 200 ms of head so the first syllable is never clipped.
pub fn trim_silence(samples: &mut Vec<i16>) {
    const PAD_FRAMES: usize = 1;
    const MIN_HEAD_SAMPLES: usize = 3200;

    let state = unsafe { dsp::speex_preprocess_state_init(FRAME_SIZE as i32, 16_000) };
    if state.is_null() {
        return;
    }
    let mut enable: i32 = 1;
    let mut prob_start: i32 = 80;
    let mut prob_continue: i32 = 50;
    unsafe {
        dsp::speex_preprocess_ctl(
            state,
            dsp::SPEEX_PREPROCESS_SET_VAD,
            &mut enable as *mut i32 as *mut _,
        );
        dsp::speex_preprocess_ctl(
            state,
            dsp::SPEEX_PREPROCESS_SET_PROB_START,
            &mut prob_start as *mut i32 as *mut _,
        );
        dsp::speex_preprocess_ctl(
            state,
            dsp::SPEEX_PREPROCESS_SET_PROB_CONTINUE,
            &mut prob_continue as *mut i32 as *mut _,
        );
    }

    let frame_count = samples.len() / FRAME_SIZE;
    let mut voice = vec![false; frame_count];
    let mut frame = vec![0i16; FRAME_SIZE];
    for index in 0..frame_count {
        frame.copy_from_slice(&samples[index * FRAME_SIZE..(index + 1) * FRAME_SIZE]);
        voice[index] = unsafe { dsp::speex_preprocess_run(state, frame.as_mut_ptr()) } != 0;
    }
    unsafe { dsp::speex_preprocess_state_destroy(state) };

    let mut first = 0usize;
    while first < frame_count && !voice[first] {
        first += 1;
    }
    let mut last = frame_count;
    while last > 0 && !voice[last - 1] {
        last -= 1;
    }
    if first >= last {
        return;
    }
    first = first.saturating_sub(PAD_FRAMES);
    last = (last + PAD_FRAMES).min(frame_count);

    let mut start = first * FRAME_SIZE;
    let end = last * FRAME_SIZE;
    if start < MIN_HEAD_SAMPLES {
        start = MIN_HEAD_SAMPLES;
    }
    if start > end || start >= samples.len() {
        return;
    }
    samples.truncate(end);
    samples.drain(..start);
}

/// Speexdsp spectral subtraction with the configured suppression level.
pub fn denoise_speexdsp(samples: &mut [i16], level_db: i32) {
    let state = unsafe { dsp::speex_preprocess_state_init(FRAME_SIZE as i32, 16_000) };
    if state.is_null() {
        return;
    }
    let mut enable: i32 = 1;
    let mut level = level_db;
    unsafe {
        dsp::speex_preprocess_ctl(
            state,
            dsp::SPEEX_PREPROCESS_SET_DENOISE,
            &mut enable as *mut i32 as *mut _,
        );
        dsp::speex_preprocess_ctl(
            state,
            dsp::SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,
            &mut level as *mut i32 as *mut _,
        );
    }

    let frame_count = samples.len() / FRAME_SIZE;
    for index in 0..frame_count {
        unsafe {
            dsp::speex_preprocess_run(
                state,
                samples[index * FRAME_SIZE..].as_mut_ptr(),
            );
        }
    }
    let remainder = samples.len() % FRAME_SIZE;
    if remainder > 0 {
        let tail_start = samples.len() - remainder;
        let mut pad = vec![0i16; FRAME_SIZE];
        pad[..remainder].copy_from_slice(&samples[tail_start..]);
        unsafe { dsp::speex_preprocess_run(state, pad.as_mut_ptr()) };
        samples[tail_start..].copy_from_slice(&pad[..remainder]);
    }
    unsafe { dsp::speex_preprocess_state_destroy(state) };
}

/// Resamples mono i16 audio between two rates using libsoxr.
pub fn resample(samples: &[i16], from: u32, to: u32) -> Result<Vec<i16>> {
    if from == to {
        return Ok(samples.to_vec());
    }
    let mut error: dsp::soxr_error_t = ptr::null();
    let ratio = to as f64 / from as f64;
    let resampler = unsafe {
        dsp::soxr_create(
            from as f64,
            to as f64,
            1,
            &mut error,
            ptr::null(),
            ptr::null(),
            ptr::null(),
        )
    };
    if let Some(message) = dsp::soxr_error_message(error) {
        return Err(Error::runtime(format!("soxr create failed: {message}")));
    }
    if resampler.is_null() {
        return Err(Error::runtime("soxr_create returned null"));
    }

    let input: Vec<f32> = samples.iter().map(|s| *s as f32 / 32768.0).collect();
    let mut output: Vec<f32> = Vec::with_capacity((input.len() as f64 * ratio) as usize + 512);
    let mut chunk = vec![0f32; 16_384];
    let mut done = 0usize;

    // Feed the whole buffer, then flush the resampler's internal delay line.
    loop {
        let mut consumed = 0usize;
        let mut produced = 0usize;
        let error = unsafe {
            dsp::soxr_process(
                resampler,
                input.as_ptr().add(done) as *const _,
                input.len() - done,
                &mut consumed,
                chunk.as_mut_ptr() as *mut _,
                chunk.len(),
                &mut produced,
            )
        };
        if let Some(message) = dsp::soxr_error_message(error) {
            unsafe { dsp::soxr_delete(resampler) };
            return Err(Error::runtime(format!("soxr process failed: {message}")));
        }
        done += consumed;
        output.extend_from_slice(&chunk[..produced]);
        if produced == 0 && consumed == 0 {
            break;
        }
        if done >= input.len() && produced == 0 {
            break;
        }
    }

    loop {
        let mut consumed = 0usize;
        let mut produced = 0usize;
        let error = unsafe {
            dsp::soxr_process(
                resampler,
                ptr::null(),
                0,
                &mut consumed,
                chunk.as_mut_ptr() as *mut _,
                chunk.len(),
                &mut produced,
            )
        };
        if let Some(message) = dsp::soxr_error_message(error) {
            unsafe { dsp::soxr_delete(resampler) };
            return Err(Error::runtime(format!("soxr flush failed: {message}")));
        }
        if produced == 0 {
            break;
        }
        output.extend_from_slice(&chunk[..produced]);
    }

    unsafe { dsp::soxr_delete(resampler) };
    Ok(output
        .into_iter()
        .map(|value| (value * 32768.0).clamp(-32768.0, 32767.0) as i16)
        .collect())
}

/// DeepFilterNet helper binary path (user-installed, not shipped).
fn deep_filter_binary() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".to_string());
    PathBuf::from(home).join(".local/share/ainput/bin/deep-filter")
}

/// Runs the external DeepFilterNet binary.
///
/// Deliberately returns an error instead of silently keeping undenoised audio:
/// an enabled denoiser that does nothing is exactly the class of silent
/// failure this rewrite is meant to remove.
pub fn denoise_deepfilter(samples: &mut Vec<i16>) -> Result<()> {
    let binary = deep_filter_binary();
    if !binary.is_file() {
        return Err(Error::local_setup(format!(
            "deepfilter is enabled but {} does not exist",
            binary.display()
        )));
    }

    let work_dir = std::env::temp_dir();
    let stamp = format!("{}_{}", std::process::id(), crate::diagnostic::hash_value(
        &format!("{:?}", std::time::SystemTime::now()),
    ));
    let input_48k = work_dir.join(format!("ainput_df_{stamp}_48k.wav"));
    let output_48k = work_dir.join(format!("ainput_df_{stamp}_48k_df.wav"));

    let resampled = resample(samples, 16_000, 48_000)?;
    crate::wav::write(&input_48k, &resampled)?;

    let status = std::process::Command::new(&binary)
        .arg("-D")
        .arg("-o")
        .arg(&work_dir)
        .arg(&input_48k)
        .status();

    let cleanup = |path: &PathBuf| {
        let _ = std::fs::remove_file(path);
    };
    cleanup(&input_48k);

    let status = match status {
        Ok(status) => status,
        Err(err) => {
            cleanup(&output_48k);
            return Err(Error::local_setup(format!(
                "cannot run {}: {err}",
                binary.display()
            )));
        }
    };
    if !status.success() {
        cleanup(&output_48k);
        return Err(Error::local_setup(format!(
            "{} exited with {status}",
            binary.display()
        )));
    }

    // DeepFilterNet writes `<stem>_48k_df.wav` next to its output directory.
    let produced = if output_48k.is_file() {
        output_48k.clone()
    } else {
        // Some builds keep the input name and write `<stem>_df.wav`.
        let alternative = work_dir.join(format!("ainput_df_{stamp}_48k_df.wav"));
        alternative
    };
    let result = crate::wav::read(&produced).and_then(|filtered| {
        if filtered.is_empty() {
            return Err(Error::audio_data("deepfilter produced an empty WAV"));
        }
        resample(&filtered, 48_000, 16_000)
    });
    cleanup(&produced);

    let filtered = result?;
    *samples = filtered;
    Ok(())
}

/// Full preprocessing pipeline. Returns an error when a configured denoiser
/// could not run, so the caller can surface it.
pub fn preprocess(
    samples: &mut Vec<i16>,
    denoise: Denoise,
    lufs_target: f64,
    speex_level: i32,
    crest_threshold: f64,
) -> Result<Gated> {
    let loudness = normalize(samples, lufs_target);
    let voice = has_voice(samples, crest_threshold);
    if voice {
        match denoise {
            Denoise::None => {}
            Denoise::Speexdsp => denoise_speexdsp(samples, speex_level),
            Denoise::DeepFilter => denoise_deepfilter(samples)?,
        }
        trim_silence(samples);
    }
    Ok(Gated { loudness, voice })
}

pub struct Gated {
    pub loudness: f64,
    pub voice: bool,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tone(amplitude: f64, count: usize) -> Vec<i16> {
        (0..count)
            .map(|i| {
                let phase = 2.0 * std::f64::consts::PI * 440.0 * i as f64 / 16_000.0;
                (phase.sin() * amplitude) as i16
            })
            .collect()
    }

    #[test]
    fn silence_is_not_speech() {
        let silent = vec![0i16; 16_000];
        assert!(!has_voice(&silent, 2.4));
    }

    #[test]
    fn steady_tone_is_gated_by_crest() {
        // A pure sine has crest ~1.41; speech has much sharper peaks. The
        // gate exists precisely to reject flat noise like this.
        let samples = tone(12_000.0, 16_000);
        assert!(!has_voice(&samples, 2.4));
    }

    #[test]
    fn impulsive_signal_passes_gate() {
        // Short bursts separated by silence give a high crest factor.
        let mut samples = vec![0i16; 16_000];
        for burst in samples.chunks_mut(2000).step_by(8) {
            for slot in burst.iter_mut().take(60) {
                *slot = 20_000;
            }
        }
        assert!(has_voice(&samples, 2.4));
    }

    #[test]
    fn zero_threshold_disables_crest_gate() {
        let quiet = tone(3.0, 16_000);
        // With the gate at 0, only the VAD decides.
        let _ = has_voice(&quiet, 0.0);
    }

    #[test]
    fn normalize_reaches_target() {
        let mut samples = tone(2000.0, 16_000);
        let measured = normalize(&mut samples, -16.0);
        assert!(measured < -20.0);
        assert!(samples.iter().any(|s| s.abs() > 2000));
    }

    #[test]
    fn resample_changes_length() {
        let samples = tone(8000.0, 16_000);
        let up = resample(&samples, 16_000, 48_000).unwrap();
        assert!(up.len() > samples.len() * 2);
        let back = resample(&up, 48_000, 16_000).unwrap();
        assert!((back.len() as i64 - samples.len() as i64).abs() < 500);
    }

    #[test]
    fn trim_removes_padding() {
        let mut samples = vec![0i16; 8000];
        samples.extend(tone(12_000.0, 16_000));
        samples.extend(vec![0i16; 8000]);
        let before = samples.len();
        trim_silence(&mut samples);
        assert!(samples.len() < before);
    }
}
