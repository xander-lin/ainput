//! Audio preprocessing: normalize and denoise the captured waveform, then
//! check the remaining signal for speech and trim the quiet edges.

use std::os::unix::fs::DirBuilderExt;
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};

use crate::error::{Error, Result};
use crate::ffi::dsp;
use crate::settings::Denoise;

pub const FRAME_SIZE: usize = 320; // 20 ms at 16 kHz
const SAMPLES_PER_MS: usize = 16;
const MIN_FRAME_RMS: f64 = 64.0; // about -54 dBFS
/// Default `Gate::min_active_ms`.
pub const DEFAULT_MIN_ACTIVE_MS: i32 = 250;
/// Default `Gate::lufs_margin`.
pub const DEFAULT_LUFS_MARGIN: f64 = 5.0;

/// EBU R128 integrated loudness in LUFS. Negative infinity when nothing is
/// loud enough to measure: the absolute gate sits at -70 LUFS and one
/// measurement block spans 400 ms.
fn integrated_loudness(samples: &[i16]) -> Result<f64> {
    let state = unsafe { dsp::ebur128_init(1, 16_000, dsp::EBUR128_MODE_I) };
    if state.is_null() {
        return Err(Error::audio_data(
            "cannot initialize loudness normalization",
        ));
    }
    let mut loudness = 0.0f64;
    let (add_status, measure_status) = unsafe {
        let add = dsp::ebur128_add_frames_short(state, samples.as_ptr(), samples.len());
        let measure = dsp::ebur128_loudness_global(state, &mut loudness);
        let mut owned = state;
        dsp::ebur128_destroy(&mut owned);
        (add, measure)
    };
    if add_status != 0 || measure_status != 0 {
        return Err(Error::audio_data(format!(
            "loudness measurement failed (add={add_status}, measure={measure_status})"
        )));
    }
    Ok(loudness)
}

/// EBU R128 normalization to `target_lufs`, returning the measured loudness.
pub fn normalize(samples: &mut [i16], target_lufs: f64) -> Result<f64> {
    Ok(normalize_with_gain(samples, target_lufs)?.0)
}

fn normalize_with_gain(samples: &mut [i16], target_lufs: f64) -> Result<(f64, f64)> {
    if samples.is_empty() {
        return Ok((f64::NEG_INFINITY, 1.0));
    }
    let loudness = integrated_loudness(samples)?;

    let mut sum_sq = 0.0;
    let mut peak = 0.0f64;
    for &sample in samples.iter() {
        let value = sample as f64;
        sum_sq += value * value;
        peak = peak.max(value.abs());
    }
    if peak == 0.0 {
        return Ok((loudness, 1.0));
    }
    // Integrated R128 is gated below -70 LUFS and needs a 400 ms window.
    // Use RMS for those clips, retaining a bounded gain and peak headroom.
    let reference = if loudness.is_finite() {
        loudness
    } else {
        20.0 * ((sum_sq / samples.len() as f64).sqrt() / 32768.0).log10()
    };
    let gain = 10f64
        .powf((target_lufs - reference) / 20.0)
        .clamp(0.05, 1000.0)
        .min(32767.0 / peak);
    for sample in samples.iter_mut() {
        let scaled = (*sample as f64 * gain).clamp(-32768.0, 32767.0);
        *sample = scaled as i16;
    }
    Ok((loudness, gain))
}

fn check_cancel(cancel: &AtomicBool) -> Result<()> {
    if cancel.load(Ordering::Acquire) {
        return Err(Error::timeout("cancelled"));
    }
    Ok(())
}

fn frame_rms(frame: &[i16]) -> f64 {
    let (sum, sum_sq) = frame.iter().fold((0, 0), |(sum, sum_sq), &sample| {
        let value = i64::from(sample);
        (sum + value, sum_sq + value * value)
    });
    let count = frame.len() as i64;
    // Measure variation around the frame mean, leaving the waveform intact.
    ((count * sum_sq - sum * sum) as f64).sqrt() / count as f64
}

#[derive(Default)]
struct Activity {
    active_samples: usize,
    peak_rms: f64,
}

fn measure_activity(samples: &[i16], cancel: &AtomicBool) -> Result<Activity> {
    let mut activity = Activity::default();
    for frame in samples.chunks(FRAME_SIZE) {
        check_cancel(cancel)?;
        let rms = frame_rms(frame);
        activity.peak_rms = activity.peak_rms.max(rms);
        if rms >= MIN_FRAME_RMS {
            activity.active_samples += frame.len();
        }
    }
    Ok(activity)
}

/// Speech-gate parameters, applied to the normalized and denoised recording.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Gate {
    /// Minimum total time of active audio. A 20 ms frame is active when its
    /// variation reaches `MIN_FRAME_RMS`.
    pub min_active_ms: i32,
    /// Optional crest and VAD checks after denoising. `0` skips both.
    pub crest_threshold: f64,
    /// How far below the normalization target the denoised audio may fall
    /// and still count as speech, in LUFS. `0` disables the check.
    pub lufs_margin: f64,
}

impl Gate {
    /// Every parameter at its default except the crest threshold.
    pub fn with_crest(crest_threshold: f64) -> Self {
        Self {
            min_active_ms: DEFAULT_MIN_ACTIVE_MS,
            crest_threshold,
            lufs_margin: DEFAULT_LUFS_MARGIN,
        }
    }
}

/// Speech detection on normalized, denoised samples, with the gate
/// parameters at their default except `crest_threshold`.
pub fn has_voice(samples: &[i16], crest_threshold: f64) -> Result<bool> {
    let cancel = AtomicBool::new(false);
    let activity = measure_activity(samples, &cancel)?;
    detect_voice(samples, &activity, &Gate::with_crest(crest_threshold), &cancel)
}

fn detect_voice(samples: &[i16], activity: &Activity, gate: &Gate, cancel: &AtomicBool) -> Result<bool> {
    let min_active = gate.min_active_ms.max(0) as usize * SAMPLES_PER_MS;
    if activity.active_samples < min_active {
        return Ok(false);
    }
    if gate.crest_threshold <= 0.0 {
        return Ok(true);
    }

    let (peak, sum_sq) = samples
        .iter()
        .fold((0.0f64, 0.0), |(peak, sum_sq), &sample| {
            let value = f64::from(sample);
            (peak.max(value.abs()), sum_sq + value * value)
        });
    let rms = (sum_sq / samples.len() as f64).sqrt();
    if peak < gate.crest_threshold * rms {
        return Ok(false);
    }

    let mut vad = NativeSpeex::vad(90, 70)?;
    let mut voice_frames = 0;
    for frame in samples.chunks(FRAME_SIZE) {
        check_cancel(cancel)?;
        voice_frames += usize::from(vad.classify(frame));
    }
    let total = samples.len().div_ceil(FRAME_SIZE);
    Ok(voice_frames >= 5 && voice_frames as f64 >= 0.15 * total as f64)
}

/// Removes quiet edges, keeping 200 ms of headroom and one frame of tailroom.
pub fn trim_silence(samples: &mut Vec<i16>) -> Result<()> {
    trim_silence_cancellable(samples, &AtomicBool::new(false))
}

fn trim_silence_cancellable(samples: &mut Vec<i16>, cancel: &AtomicBool) -> Result<()> {
    const HEAD_SAMPLES: usize = 200 * SAMPLES_PER_MS;
    let mut vad = NativeSpeex::vad(80, 50)?;
    let mut first = None;
    let mut end = 0;
    for (index, frame) in samples.chunks(FRAME_SIZE).enumerate() {
        check_cancel(cancel)?;
        let start = index * FRAME_SIZE;
        // Keep active samples even when VAD misses them. A partial final
        // frame has fewer observations, so also retain it when it varies.
        if vad.classify(frame)
            || frame_rms(frame) >= MIN_FRAME_RMS
            || (frame.len() < FRAME_SIZE && frame.iter().any(|&s| s != frame[0]))
        {
            first.get_or_insert(start);
            end = start + frame.len();
        }
    }
    if let Some(start) = first {
        samples.truncate((end + FRAME_SIZE).min(samples.len()));
        samples.drain(..start.saturating_sub(HEAD_SAMPLES + FRAME_SIZE));
    }
    Ok(())
}

trait SpeexDenoiser {
    fn configure(&mut self, request: i32, value: i32) -> Result<()>;
    fn process(&mut self, frame: &mut [i16; FRAME_SIZE]) -> bool;
}

struct NativeSpeex(*mut dsp::SpeexPreprocessState);

impl NativeSpeex {
    fn new() -> Result<Self> {
        let state = unsafe { dsp::speex_preprocess_state_init(FRAME_SIZE as i32, 16_000) };
        if state.is_null() {
            Err(Error::audio_data("cannot initialize SpeexDSP preprocessor"))
        } else {
            Ok(Self(state))
        }
    }

    fn vad(start: i32, continue_: i32) -> Result<Self> {
        let mut state = Self::new()?;
        state.configure(dsp::SPEEX_PREPROCESS_SET_VAD, 1)?;
        state.configure(dsp::SPEEX_PREPROCESS_SET_PROB_START, start)?;
        state.configure(dsp::SPEEX_PREPROCESS_SET_PROB_CONTINUE, continue_)?;
        Ok(state)
    }

    fn classify(&mut self, samples: &[i16]) -> bool {
        let mut frame = [0i16; FRAME_SIZE];
        frame[..samples.len()].copy_from_slice(samples);
        self.process(&mut frame)
    }
}

impl SpeexDenoiser for NativeSpeex {
    fn configure(&mut self, request: i32, mut value: i32) -> Result<()> {
        let status =
            unsafe { dsp::speex_preprocess_ctl(self.0, request, &mut value as *mut i32 as *mut _) };
        if status != 0 {
            return Err(Error::audio_data(format!(
                "cannot configure SpeexDSP preprocessor (option {request}, error {status})"
            )));
        }
        Ok(())
    }

    fn process(&mut self, frame: &mut [i16; FRAME_SIZE]) -> bool {
        // SpeexDSP returns the VAD decision, even though it also updates the audio.
        (unsafe { dsp::speex_preprocess_run(self.0, frame.as_mut_ptr()) }) != 0
    }
}

impl Drop for NativeSpeex {
    fn drop(&mut self) {
        unsafe { dsp::speex_preprocess_state_destroy(self.0) };
    }
}

/// Speexdsp spectral subtraction with the configured suppression level.
pub fn denoise_speexdsp(samples: &mut [i16], level_db: i32) -> Result<()> {
    denoise_speexdsp_with(samples, level_db, &AtomicBool::new(false), NativeSpeex::new)
}

fn denoise_speexdsp_with<S: SpeexDenoiser>(
    samples: &mut [i16],
    level_db: i32,
    cancel: &AtomicBool,
    create: impl FnOnce() -> Result<S>,
) -> Result<()> {
    check_cancel(cancel)?;
    let mut state = create()?;
    state.configure(dsp::SPEEX_PREPROCESS_SET_DENOISE, 1)?;
    state.configure(dsp::SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, level_db)?;

    // SpeexDSP's overlap-add output trails the input by one frame. Write
    // each output to the preceding chunk, then flush the final chunk.
    let mut previous: Option<&mut [i16]> = None;
    let mut frame = [0i16; FRAME_SIZE];
    for input in samples.chunks_mut(FRAME_SIZE) {
        check_cancel(cancel)?;
        frame.fill(0);
        frame[..input.len()].copy_from_slice(input);
        state.process(&mut frame);
        if let Some(output) = previous.replace(input) {
            output.copy_from_slice(&frame[..output.len()]);
        }
    }
    if let Some(output) = previous {
        check_cancel(cancel)?;
        frame.fill(0);
        state.process(&mut frame);
        output.copy_from_slice(&frame[..output.len()]);
    }
    Ok(())
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
    run_deepfilter(samples, deep_filter_binary(), &AtomicBool::new(false))
}

fn run_deepfilter(samples: &mut Vec<i16>, binary: PathBuf, cancel: &AtomicBool) -> Result<()> {
    const FILTER_RATE: u32 = 48_000;
    if !binary.is_file() {
        return Err(Error::local_setup(format!(
            "deepfilter is enabled but {} does not exist",
            binary.display()
        )));
    }

    static NEXT_WORK: AtomicU64 = AtomicU64::new(0);
    let work_dir = std::env::temp_dir().join(format!(
        "ainput-df-{}-{}-{}",
        std::process::id(),
        NEXT_WORK.fetch_add(1, Ordering::Relaxed),
        crate::diagnostic::hash_value(&format!("{:?}", std::time::SystemTime::now()))
    ));
    std::fs::DirBuilder::new()
        .mode(0o700)
        .create(&work_dir)
        .map_err(|error| Error::audio_data(format!("cannot stage DeepFilter audio: {error}")))?;
    struct WorkDir(PathBuf);
    impl Drop for WorkDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    let work_dir = WorkDir(work_dir);
    let input_48k = work_dir.0.join("input_48k.wav");
    // The upstream helper writes the input basename to its output directory.
    let output_dir = work_dir.0.join("out");
    std::fs::create_dir(&output_dir)
        .map_err(|error| Error::audio_data(format!("cannot stage DeepFilter output: {error}")))?;

    let resampled = resample(samples, crate::wav::SAMPLE_RATE, FILTER_RATE)?;
    crate::wav::write_at_rate(&input_48k, &resampled, FILTER_RATE)?;

    let mut command = std::process::Command::new(&binary);
    command.arg("-D").arg("-o").arg(&output_dir).arg(&input_48k);
    let status = crate::process::run(
        &mut command,
        &binary.display().to_string(),
        Some(Instant::now() + Duration::from_secs(120)),
        cancel,
    )?;
    if !status.success() {
        return Err(Error::local_setup(format!(
            "{} exited with {status}",
            binary.display()
        )));
    }

    let output_48k = output_dir.join("input_48k.wav");
    let output_48k = if output_48k.is_file() {
        output_48k
    } else {
        output_dir.join("input_48k_df.wav")
    };
    let filtered = crate::wav::read_at_rate(&output_48k, FILTER_RATE)?;
    if filtered.is_empty() {
        return Err(Error::audio_data("deepfilter produced an empty WAV"));
    }
    *samples = resample(&filtered, FILTER_RATE, crate::wav::SAMPLE_RATE)?;
    Ok(())
}

/// Full preprocessing pipeline with every gate parameter at its default
/// except `crest_threshold`. Returns an error when a configured denoiser
/// could not run, so the caller can surface it.
pub fn preprocess(
    samples: &mut Vec<i16>,
    denoise: Denoise,
    lufs_target: f64,
    speex_level: i32,
    crest_threshold: f64,
) -> Result<Gated> {
    preprocess_cancellable(
        samples,
        denoise,
        lufs_target,
        speex_level,
        Gate::with_crest(crest_threshold),
        &AtomicBool::new(false),
    )
}

/// A denoiser that may borrow from its caller for the duration of one run.
type Denoiser<'a> = Box<dyn FnOnce(&mut Vec<i16>) -> Result<()> + 'a>;

pub(crate) fn preprocess_cancellable(
    samples: &mut Vec<i16>,
    denoise: Denoise,
    lufs_target: f64,
    speex_level: i32,
    gate: Gate,
    cancel: &AtomicBool,
) -> Result<Gated> {
    let denoiser = (denoise != Denoise::None).then(|| {
        Box::new(move |samples: &mut Vec<i16>| match denoise {
            Denoise::None => Ok(()),
            Denoise::Speexdsp => {
                denoise_speexdsp_with(samples, speex_level, cancel, NativeSpeex::new)
            }
            Denoise::DeepFilter => run_deepfilter(samples, deep_filter_binary(), cancel),
        }) as Denoiser
    });
    preprocess_with_denoiser(samples, lufs_target, &gate, cancel, denoiser)
}

/// `denoise` is `None` when no denoiser is enabled.
fn preprocess_with_denoiser(
    samples: &mut Vec<i16>,
    lufs_target: f64,
    gate: &Gate,
    cancel: &AtomicBool,
    denoise: Option<Denoiser<'_>>,
) -> Result<Gated> {
    check_cancel(cancel)?;
    let input_peak = samples
        .iter()
        .map(|&s| i32::from(s).abs())
        .max()
        .unwrap_or(0);
    if input_peak == 0 {
        return Ok(Gated {
            input_peak,
            loudness: f64::NEG_INFINITY,
            output_loudness: f64::NEG_INFINITY,
            gain: 1.0,
            activity: Activity::default(),
            voice: false,
        });
    }
    let input_is_constant = samples.iter().all(|&sample| sample == samples[0]);
    let (loudness, gain) = normalize_with_gain(samples, lufs_target)?;
    check_cancel(cancel)?;
    let denoised = denoise.is_some();
    if let Some(denoise) = denoise {
        denoise(samples)?;
    }
    check_cancel(cancel)?;
    let activity = measure_activity(samples, cancel)?;

    // Loudness gate. Normalization puts the recording at `lufs_target`, and
    // the denoiser then removes whatever it takes for noise. Speech survives
    // that nearly unchanged, so audio that is mostly speech stays within
    // `lufs_margin` of the target, while noise alone falls far below it.
    //
    // Both premises must hold for the comparison to mean anything: the
    // recording really was normalized to the target (R128 measured it, so it
    // is at least 400 ms and louder than -70 LUFS), and a denoiser ran. With
    // no denoiser the normalized audio sits at the target by construction and
    // the gate could not tell speech from noise. Otherwise it is skipped.
    let measurable = denoised && loudness.is_finite();
    let output_loudness = if measurable {
        integrated_loudness(samples)?
    } else {
        f64::NEG_INFINITY
    };
    let loud_enough = !(measurable && gate.lufs_margin > 0.0)
        || output_loudness > lufs_target - gate.lufs_margin;
    // A flat capture can only develop variation through processing artifacts.
    let voice = loud_enough
        && !input_is_constant
        && detect_voice(samples, &activity, gate, cancel)?;
    if voice {
        trim_silence_cancellable(samples, cancel)?;
    }
    Ok(Gated {
        input_peak,
        loudness,
        output_loudness,
        gain,
        activity,
        voice,
    })
}

pub struct Gated {
    pub input_peak: i32,
    /// Integrated loudness of the capture, before normalization.
    pub loudness: f64,
    /// Integrated loudness after denoising. Measured only when the recording
    /// was normalized to its target and a denoiser ran (see the loudness
    /// gate); negative infinity otherwise.
    pub output_loudness: f64,
    pub gain: f64,
    activity: Activity,
    pub voice: bool,
}

impl Gated {
    pub fn active_ms(&self) -> usize {
        self.activity.active_samples / SAMPLES_PER_MS
    }

    pub fn peak_frame_rms(&self) -> f64 {
        self.activity.peak_rms
    }
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
        assert!(!has_voice(&silent, 2.4).unwrap());
    }

    #[test]
    fn steady_tone_is_gated_by_crest() {
        // A pure sine has crest ~1.41; speech has much sharper peaks. The
        // gate exists precisely to reject flat noise like this.
        let samples = tone(12_000.0, 16_000);
        assert!(!has_voice(&samples, 2.4).unwrap());
    }

    #[test]
    fn sustained_burst_passes_crest_and_vad_gate() {
        // A voiced burst amid silence gives enough active frames and a high crest factor.
        let mut samples = vec![0i16; 16_000];
        let burst = tone(20_000.0, 4160);
        samples[3200..3200 + burst.len()].copy_from_slice(&burst);
        assert!(has_voice(&samples, 2.4).unwrap());
    }

    #[test]
    fn zero_crest_threshold_still_requires_sustained_audio_variation() {
        // A partial final frame contributes only the samples it contains.
        assert!(has_voice(&tone(3000.0, 4000), 0.0).unwrap());
        assert!(!has_voice(&tone(3000.0, 3999), 0.0).unwrap());
        let mut sparse = tone(3000.0, 3840);
        sparse.resize(16_000, 0);
        assert!(!has_voice(&sparse, 0.0).unwrap());
        assert!(!has_voice(&tone(3.0, 16_000), 0.0).unwrap());
        assert!(!has_voice(&[0; 4800], 0.0).unwrap());
        assert!(!has_voice(&[3000; 4800], 0.0).unwrap());
        let mut clicks = vec![0; 16_000];
        for index in [160, 3360, 6560] {
            clicks[index] = 20_000;
        }
        let gated = preprocess(&mut clicks, Denoise::Speexdsp, -16.0, -15, 0.0).unwrap();
        assert!(!gated.voice);
        assert!(gated.active_ms() < 250);
    }

    #[test]
    fn normalize_reaches_target() {
        let mut samples = tone(2000.0, 16_000);
        let measured = normalize(&mut samples, -16.0).unwrap();
        assert!(measured < -20.0);
        assert!(samples.iter().any(|s| s.abs() > 2000));
    }

    #[test]
    fn very_quiet_and_short_audio_is_amplified_before_the_gate() {
        for (len, expected_voice) in [(1600, false), (4800, true), (16_000, true)] {
            let mut samples = tone(3.0, len);
            let gated = preprocess(&mut samples, Denoise::Speexdsp, -16.0, -15, 0.0).unwrap();
            assert_eq!(gated.voice, expected_voice, "samples={len}");
            assert!(gated.gain > 20.0);
            assert!(samples.iter().any(|&s| i32::from(s).abs() > 100));
        }
    }

    #[test]
    fn only_digital_silence_skips_denoising_and_activity_uses_its_output() {
        let cancel = AtomicBool::new(false);
        let gate = Gate::with_crest(0.0);
        let mut samples = vec![0; 1600];
        let gated = preprocess_with_denoiser(
            &mut samples,
            -16.0,
            &gate,
            &cancel,
            Some(Box::new(|_: &mut Vec<i16>| {
                panic!("digital silence must skip the denoiser");
            })),
        )
        .unwrap();
        assert_eq!(gated.input_peak, 0);
        assert!(!gated.voice);

        for input in [vec![1; 4800], tone(3.0, 4800)] {
            let mut samples = input.clone();
            let gated = preprocess_with_denoiser(
                &mut samples,
                -16.0,
                &gate,
                &cancel,
                Some(Box::new(|samples: &mut Vec<i16>| {
                    assert!(samples.iter().any(|&s| s > 100));
                    if input.iter().all(|&s| s == 1) {
                        assert!(samples.iter().all(|&s| s == 1000));
                    }
                    samples.fill(0);
                    Ok(())
                })),
            )
            .unwrap();
            assert!(!gated.voice);
            assert_eq!(gated.active_ms(), 0);
        }
    }

    #[test]
    fn dc_only_recording_is_not_speech_after_real_denoising() {
        for denoise in [Denoise::None, Denoise::Speexdsp] {
            for len in [1600, 16_000] {
                let mut samples = vec![1; len];
                let gated = preprocess(&mut samples, denoise, -16.0, -15, 0.0).unwrap();
                assert_eq!(gated.gain, 1000.0);
                assert!(!gated.voice, "{denoise:?}: {} ms", gated.active_ms());
            }
        }
        for level in [100, 1000, i16::MAX] {
            let mut samples = vec![level; 16_000];
            let gated = preprocess(&mut samples, Denoise::Speexdsp, -16.0, -15, 0.0).unwrap();
            assert!(!gated.voice, "DC level {level}: {} ms", gated.active_ms());
        }
    }

    #[test]
    fn normalization_preserves_silence_and_avoids_clipping() {
        let mut silence = vec![0; 1600];
        let (_, gain) = normalize_with_gain(&mut silence, -16.0).unwrap();
        assert_eq!(gain, 1.0);
        assert!(silence.iter().all(|&s| s == 0));

        let mut transient = vec![1; 1600];
        transient[0] = 30_000;
        let (_, gain) = normalize_with_gain(&mut transient, -16.0).unwrap();
        assert!(gain <= 32767.0 / 30_000.0);
    }

    /// Stands in for a denoiser that removes a known share of the signal.
    fn attenuate(factor: f64) -> Denoiser<'static> {
        Box::new(move |samples| {
            for sample in samples.iter_mut() {
                *sample = (*sample as f64 * factor) as i16;
            }
            Ok(())
        })
    }

    #[test]
    fn loudness_gate_compares_the_denoised_audio_with_the_target() {
        let cancel = AtomicBool::new(false);
        // One second, so R128 can measure the recording before and after.
        let run = |factor: f64, margin: f64| {
            let gate = Gate {
                lufs_margin: margin,
                ..Gate::with_crest(0.0)
            };
            let mut samples = tone(3000.0, 16_000);
            preprocess_with_denoiser(&mut samples, -16.0, &gate, &cancel, Some(attenuate(factor)))
                .unwrap()
        };

        // Speech that survives denoising: about 1 LU below the target.
        let kept = run(0.9, 5.0);
        assert!(kept.voice);
        assert!((kept.output_loudness - (-16.0 + 20.0 * 0.9f64.log10())).abs() < 0.5);

        // 6 LU below the target is past the default margin...
        let dropped = run(0.5, 5.0);
        assert!(!dropped.voice);
        // ...but inside a wider one, and not checked at all when it is off.
        assert!(run(0.5, 7.0).voice);
        // It is still measured then, so the journal can show the distance.
        let far = run(0.1, 0.0);
        assert!(far.voice);
        assert!((far.output_loudness - (-36.0)).abs() < 0.5);
    }

    #[test]
    fn loudness_gate_needs_a_denoiser_and_a_measurable_recording() {
        let cancel = AtomicBool::new(false);
        let gate = Gate::with_crest(0.0);

        // Without a denoiser the normalized audio sits at the target by
        // construction: the gate could not tell speech from noise, so it is
        // skipped and nothing is measured.
        let mut samples = tone(3000.0, 16_000);
        let undenoised =
            preprocess_with_denoiser(&mut samples, -16.0, &gate, &cancel, None).unwrap();
        assert!(undenoised.voice);
        assert_eq!(undenoised.output_loudness, f64::NEG_INFINITY);

        // Shorter than one R128 block: the gain came from the RMS fallback,
        // so "normalized to the target" is not established. The gate is
        // skipped even though a denoiser ran.
        let mut samples = tone(3000.0, 300 * SAMPLES_PER_MS);
        let short = preprocess_with_denoiser(
            &mut samples,
            -16.0,
            &gate,
            &cancel,
            Some(attenuate(0.1)),
        )
        .unwrap();
        assert!(short.voice);
        assert_eq!(short.output_loudness, f64::NEG_INFINITY);
    }

    #[test]
    fn active_time_threshold_is_configurable() {
        let cancel = AtomicBool::new(false);
        // 300 ms of tone and no denoiser: only the time threshold decides.
        let passes = |min_active_ms: i32| {
            let gate = Gate {
                min_active_ms,
                ..Gate::with_crest(0.0)
            };
            let mut samples = tone(3000.0, 300 * SAMPLES_PER_MS);
            preprocess_with_denoiser(&mut samples, -16.0, &gate, &cancel, None)
                .unwrap()
                .voice
        };
        assert!(passes(DEFAULT_MIN_ACTIVE_MS));
        assert!(passes(300));
        assert!(!passes(301));
        assert!(!passes(1000));
    }

    struct FakeSpeex {
        fail_request: Option<i32>,
        calls: std::rc::Rc<std::cell::Cell<usize>>,
    }

    impl SpeexDenoiser for FakeSpeex {
        fn configure(&mut self, request: i32, _: i32) -> Result<()> {
            if self.fail_request == Some(request) {
                return Err(Error::audio_data("injected configuration failure"));
            }
            Ok(())
        }

        fn process(&mut self, _: &mut [i16; FRAME_SIZE]) -> bool {
            self.calls.set(self.calls.get() + 1);
            false
        }
    }

    #[test]
    fn speex_initialization_and_configuration_failures_are_reported() {
        let cancel = AtomicBool::new(false);
        let mut samples = vec![123; FRAME_SIZE];
        let error = denoise_speexdsp_with::<FakeSpeex>(&mut samples, -15, &cancel, || {
            Err(Error::audio_data("injected initialization failure"))
        })
        .unwrap_err();
        assert!(error.detail.contains("initialization"));

        for request in [
            dsp::SPEEX_PREPROCESS_SET_DENOISE,
            dsp::SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,
        ] {
            let calls = std::rc::Rc::new(std::cell::Cell::new(0));
            let error = denoise_speexdsp_with(&mut samples, -15, &cancel, || {
                Ok(FakeSpeex {
                    fail_request: Some(request),
                    calls: calls.clone(),
                })
            })
            .unwrap_err();
            assert!(error.detail.contains("configuration"));
            assert_eq!(calls.get(), 0);
        }
        assert!(samples.iter().all(|&s| s == 123));
    }

    #[test]
    fn speex_cancellation_stops_processing() {
        let cancel = AtomicBool::new(true);
        let calls = std::rc::Rc::new(std::cell::Cell::new(0));
        let mut samples = vec![123; FRAME_SIZE * 4];
        let error = denoise_speexdsp_with(&mut samples, -15, &cancel, || {
            Ok(FakeSpeex {
                fail_request: None,
                calls: calls.clone(),
            })
        })
        .unwrap_err();
        assert_eq!(error.category, crate::error::Category::Timeout);
        assert_eq!(calls.get(), 0);
    }

    #[test]
    fn concurrent_speex_jobs_match_independent_results() {
        let inputs: Vec<_> = [200.0, 2000.0, 12000.0]
            .into_iter()
            .map(|amplitude| tone(amplitude, 16_123))
            .collect();
        let expected: Vec<_> = inputs
            .iter()
            .map(|input| {
                let mut samples = input.clone();
                denoise_speexdsp(&mut samples, -15).unwrap();
                assert_ne!(&samples, input);
                samples
            })
            .collect();
        let barrier = std::sync::Arc::new(std::sync::Barrier::new(inputs.len()));
        let jobs: Vec<_> = inputs
            .into_iter()
            .map(|mut samples| {
                let barrier = barrier.clone();
                std::thread::spawn(move || {
                    barrier.wait();
                    denoise_speexdsp(&mut samples, -15).unwrap();
                    samples
                })
            })
            .collect();
        for (job, expected) in jobs.into_iter().zip(expected) {
            assert_eq!(job.join().unwrap(), expected);
        }
    }

    #[test]
    fn speex_flushes_and_aligns_full_and_partial_frames() {
        for tail_len in [96, FRAME_SIZE] {
            let prefix = 20 * FRAME_SIZE;
            let mut samples = vec![0; prefix];
            samples.extend(tone(3000.0, tail_len));
            denoise_speexdsp(&mut samples, -15).unwrap();
            assert_eq!(samples.len(), prefix + tail_len);
            assert!(samples[..prefix - FRAME_SIZE].iter().all(|&s| s == 0));
            assert!(samples[prefix..].iter().any(|&s| i32::from(s).abs() > 1000));
        }

        let mut very_short = tone(1000.0, 96);
        denoise_speexdsp(&mut very_short, -15).unwrap();
        assert!(very_short.iter().any(|&s| i32::from(s).abs() > 100));
    }

    #[test]
    fn concurrent_deepfilter_jobs_use_independent_files() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!(
            "ainput-deepfilter-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&dir).unwrap();
        struct Cleanup(PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = std::fs::remove_dir_all(&self.0);
            }
        }
        let _cleanup = Cleanup(dir.clone());
        let helper = dir.join("deep-filter");
        std::fs::write(
            &helper,
            r##"#!/bin/sh
set -eu
root=$(dirname "$0")
touch "$root/ready-$$"
i=0
while [ "$(find "$root" -name 'ready-*' | wc -l)" -lt 3 ]; do
    i=$((i+1))
    [ "$i" -lt 150 ] || exit 1
    sleep 0.02
done
# Canonical 48 kHz mono PCM with 12,000 samples. Write an independent
# header using the upstream output basename and verify the staged input.
out="$3/input_48k.wav"
printf 'RIFF\344\135\000\000WAVEfmt \020\000\000\000\001\000\001\000\200\273\000\000\000\167\001\000\002\000\020\000data\300\135\000\000' > "$out"
head -c 44 "$4" | cmp - "$out"
dd if="$4" bs=44 skip=1 status=none >> "$out"
"##,
        )
        .unwrap();
        std::fs::set_permissions(&helper, std::fs::Permissions::from_mode(0o700)).unwrap();
        let inputs: Vec<_> = [200.0, 2000.0, 12000.0]
            .into_iter()
            .map(|amplitude| tone(amplitude, 4000))
            .collect();
        let expected: Vec<_> = inputs
            .iter()
            .map(|input| {
                resample(&resample(input, 16_000, 48_000).unwrap(), 48_000, 16_000).unwrap()
            })
            .collect();
        let jobs: Vec<_> = inputs
            .into_iter()
            .map(|mut samples| {
                let helper = helper.clone();
                std::thread::spawn(move || {
                    run_deepfilter(&mut samples, helper, &AtomicBool::new(false)).unwrap();
                    samples
                })
            })
            .collect();
        for (job, expected) in jobs.into_iter().zip(expected) {
            assert_eq!(job.join().unwrap(), expected);
        }
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
        trim_silence(&mut samples).unwrap();
        assert!(samples.len() < before);
    }

    #[test]
    fn trim_preserves_speech_at_start() {
        let mut samples = tone(12_000.0, 16_000);
        samples.extend(vec![0; 8000]);
        let beginning = samples[..3200].to_vec();
        trim_silence(&mut samples).unwrap();
        assert_eq!(&samples[..beginning.len()], beginning);
    }

    #[test]
    fn trim_preserves_partial_final_frame() {
        let mut samples = tone(12_000.0, 3199);
        let original = samples.clone();
        trim_silence(&mut samples).unwrap();
        assert_eq!(samples, original);

        let tail = tone(20.0, 159);
        let mut samples = tone(12_000.0, 3200);
        samples.resize(16_000, 0);
        samples.extend_from_slice(&tail);
        trim_silence(&mut samples).unwrap();
        assert!(samples.ends_with(&tail));
    }
}
