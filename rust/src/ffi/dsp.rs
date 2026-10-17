//! DSP library FFI: libebur128, speexdsp, libsoxr.

#![allow(non_camel_case_types)]

use libc::{c_char, c_double, c_int, c_short, c_ulong, c_void, size_t};

// ---------------------------------------------------------------- libebur128

pub type ebur128_state = c_void;

/// `EBUR128_MODE_I` = `(1 << 2) | EBUR128_MODE_M(1 << 0)`.
pub const EBUR128_MODE_I: c_int = 5;

extern "C" {
    pub fn ebur128_init(
        channels: u32,
        samplerate: c_ulong,
        mode: c_int,
    ) -> *mut ebur128_state;
    pub fn ebur128_add_frames_short(
        st: *mut ebur128_state,
        src: *const c_short,
        frames: size_t,
    ) -> c_int;
    pub fn ebur128_loudness_global(st: *mut ebur128_state, out: *mut c_double) -> c_int;
    pub fn ebur128_destroy(st: *mut *mut ebur128_state);
}

// ----------------------------------------------------------------- speexdsp

pub type SpeexPreprocessState = c_void;

pub const SPEEX_PREPROCESS_SET_DENOISE: c_int = 0;
pub const SPEEX_PREPROCESS_SET_VAD: c_int = 4;
pub const SPEEX_PREPROCESS_SET_PROB_START: c_int = 14;
pub const SPEEX_PREPROCESS_SET_PROB_CONTINUE: c_int = 16;
pub const SPEEX_PREPROCESS_SET_NOISE_SUPPRESS: c_int = 18;

extern "C" {
    pub fn speex_preprocess_state_init(
        frame_size: c_int,
        sampling_rate: c_int,
    ) -> *mut SpeexPreprocessState;
    pub fn speex_preprocess_ctl(
        st: *mut SpeexPreprocessState,
        request: c_int,
        ptr: *mut c_void,
    ) -> c_int;
    pub fn speex_preprocess_run(st: *mut SpeexPreprocessState, x: *mut c_short) -> c_int;
    pub fn speex_preprocess_state_destroy(st: *mut SpeexPreprocessState);
}

// -------------------------------------------------------------------- soxr

pub type soxr = c_void;
/// `soxr_error_t` is `char const *`; null means success.
pub type soxr_error_t = *const c_char;

extern "C" {
    pub fn soxr_create(
        input_rate: c_double,
        output_rate: c_double,
        num_channels: u32,
        error: *mut soxr_error_t,
        io_spec: *const c_void,
        quality_spec: *const c_void,
        runtime_spec: *const c_void,
    ) -> *mut soxr;
    pub fn soxr_process(
        resampler: *mut soxr,
        input: *const c_void,
        input_len: size_t,
        input_done: *mut size_t,
        output: *mut c_void,
        output_len: size_t,
        output_done: *mut size_t,
    ) -> soxr_error_t;
    pub fn soxr_delete(resampler: *mut soxr);
}

pub fn soxr_error_message(error: soxr_error_t) -> Option<String> {
    if error.is_null() {
        return None;
    }
    Some(unsafe { std::ffi::CStr::from_ptr(error) }.to_string_lossy().into_owned())
}
