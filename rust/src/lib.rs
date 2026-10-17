//! C ABI surface for the fcitx5 addon shell.
//!
//! The shell owns every fcitx5 type; this crate only sees numbers, C strings
//! and POD structs. See `shell/ainput_core.h` for the contract.

mod asr;

/// Minimal re-exports for the manual cloud smoke test.
pub mod smoke {
    pub use crate::asr::{create, Input, Provider};
    pub use crate::settings::Credentials;
}
mod capture;
mod config;
mod diagnostic;
mod dsp;
mod engine;
mod error;
mod ffi;
mod http;
mod settings;
mod templates;
mod uinput;
mod wav;

use std::ffi::{c_char, c_void, CStr};
use std::path::PathBuf;
use std::sync::Arc;

use engine::{Callbacks, Engine};
use settings::{Denoise, Settings};

/// Mirrors `AinputCallbacks` in the C header.
#[repr(C)]
pub struct RawCallbacks {
    pub user: *mut c_void,
    pub on_text: extern "C" fn(*mut c_void, u64, *const c_char),
    pub on_error: extern "C" fn(*mut c_void, u64, i32, *const c_char),
}

/// Mirrors `AinputSettings` in the C header.
#[repr(C)]
pub struct RawSettings {
    pub provider: *const c_char,
    pub activation_msec: i32,
    pub notification_timeout: i32,
    pub buffer_ms: i32,
    pub denoise: i32,
    pub lufs_target: f64,
    pub speex_level: i32,
    pub crest_threshold: f64,
    pub qwen_api_key: *const c_char,
    pub qwen_model: *const c_char,
    pub doubao_api_key: *const c_char,
    pub doubao_model: *const c_char,
}

/// Owned handle returned to C.
pub struct AinputEngine {
    engine: Arc<Engine>,
}

/// Copies `text` into `buf`, truncating on a char boundary. Returns the full
/// length in bytes so callers can size a buffer, or -1 when `buf` is too small.
unsafe fn copy_out(text: &str, buf: *mut c_char, len: usize) -> i32 {
    let bytes = text.as_bytes();
    if buf.is_null() || len == 0 {
        return bytes.len() as i32;
    }
    if bytes.len() + 1 > len {
        return -1;
    }
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), buf as *mut u8, bytes.len());
    *buf.add(bytes.len()) = 0;
    bytes.len() as i32
}

unsafe fn read_c_string(ptr: *const c_char, fallback: &str) -> String {
    if ptr.is_null() {
        return fallback.to_string();
    }
    let value = CStr::from_ptr(ptr).to_string_lossy().into_owned();
    if value.is_empty() {
        fallback.to_string()
    } else {
        value
    }
}

#[no_mangle]
pub extern "C" fn ainput_engine_new(callbacks: *const RawCallbacks) -> *mut AinputEngine {
    let callbacks = if callbacks.is_null() {
        return std::ptr::null_mut();
    } else {
        let raw = unsafe { &*callbacks };
        Callbacks {
            user: raw.user,
            on_text: raw.on_text,
            on_error: raw.on_error,
        }
    };

    // Seed every provider file once so a new user can open all of them.
    config::seed_all();
    uinput::open();

    diagnostic::event("engine", "created", &[]);
    Box::into_raw(Box::new(AinputEngine {
        engine: Arc::new(Engine::new(callbacks)),
    }))
}

#[no_mangle]
pub extern "C" fn ainput_engine_free(engine: *mut AinputEngine) {
    if engine.is_null() {
        return;
    }
    // SAFETY: the pointer came from ainput_engine_new and is freed once.
    drop(unsafe { Box::from_raw(engine) });
}

#[no_mangle]
pub extern "C" fn ainput_engine_set_settings(
    engine: *mut AinputEngine,
    raw: *const RawSettings,
) {
    if engine.is_null() || raw.is_null() {
        return;
    }
    // SAFETY: the shell owns every string for the duration of this call.
    let (provider, credentials) = unsafe {
        let raw = &*raw;
        (
            read_c_string(raw.provider, "qwen"),
            settings::Credentials {
                qwen_api_key: read_c_string(raw.qwen_api_key, ""),
                qwen_model: read_c_string(raw.qwen_model, ""),
                doubao_api_key: read_c_string(raw.doubao_api_key, ""),
                doubao_model: read_c_string(raw.doubao_model, ""),
            },
        )
    };
    // SAFETY: `raw` was checked non-null above.
    let raw = unsafe { &*raw };
    let settings = Settings {
        provider: provider.clone(),
        activation_msec: raw.activation_msec,
        notification_timeout: raw.notification_timeout,
        buffer_ms: raw.buffer_ms,
        denoise: Denoise::from_i32(raw.denoise),
        lufs_target: raw.lufs_target,
        speex_level: raw.speex_level,
        crest_threshold: raw.crest_threshold,
        credentials,
    };
    engine::set_current_provider(&provider);
    let handle = unsafe { &*engine };
    handle.engine.set_settings(settings);
}

#[no_mangle]
pub extern "C" fn ainput_engine_start(engine: *mut AinputEngine) -> u64 {
    if engine.is_null() {
        return 0;
    }
    unsafe { &*engine }.engine.start()
}

#[no_mangle]
pub extern "C" fn ainput_engine_stop(engine: *mut AinputEngine, recognition_id: u64) {
    if engine.is_null() {
        return;
    }
    unsafe { &*engine }.engine.stop(recognition_id);
}

#[no_mangle]
pub extern "C" fn ainput_engine_shutdown(engine: *mut AinputEngine) {
    if engine.is_null() {
        return;
    }
    unsafe { &*engine }.engine.shutdown();
}

#[no_mangle]
pub extern "C" fn ainput_provider_count() -> usize {
    asr::DESCRIPTORS.len()
}

#[no_mangle]
pub extern "C" fn ainput_provider_id(
    index: usize,
    buf: *mut c_char,
    len: usize,
) -> i32 {
    match asr::DESCRIPTORS.get(index) {
        Some(descriptor) => unsafe { copy_out(descriptor.id, buf, len) },
        None => -1,
    }
}

#[no_mangle]
pub extern "C" fn ainput_provider_display(
    index: usize,
    buf: *mut c_char,
    len: usize,
) -> i32 {
    match asr::DESCRIPTORS.get(index) {
        Some(descriptor) => {
            let name = asr::display_name(descriptor.id);
            unsafe { copy_out(&name, buf, len) }
        }
        None => -1,
    }
}

#[no_mangle]
pub extern "C" fn ainput_uinput_open() -> i32 {
    if uinput::open() {
        1
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn ainput_uinput_restore_capslock() {
    uinput::restore_capslock();
}

#[no_mangle]
pub extern "C" fn ainput_uinput_close() {
    uinput::close();
}

#[no_mangle]
pub extern "C" fn ainput_uinput_error(buf: *mut c_char, len: usize) -> i32 {
    match uinput::error() {
        Some(message) => unsafe { copy_out(&message, buf, len) },
        None => {
            if buf.is_null() || len == 0 {
                return 0;
            }
            unsafe { *buf = 0 };
            0
        }
    }
}

#[no_mangle]
pub extern "C" fn ainput_config_template(
    which: i32,
    buf: *mut c_char,
    len: usize,
) -> i32 {
    match templates::by_id(which) {
        Some(text) => unsafe { copy_out(&text, buf, len) },
        None => -1,
    }
}

/// Reads the pre-0.2 `config.json` layout.
///
/// The old file mixed provider selection, key timing and audio tuning in one
/// place. That data now lives in fcitx5's configuration; this call lets the
/// shell carry existing values over instead of silently resetting them.
#[no_mangle]
pub extern "C" fn ainput_migrate_legacy_config(
    out: *mut RawSettings,
    report: *mut c_char,
    report_len: usize,
) -> i32 {
    if out.is_null() {
        return -1;
    }

    let path: PathBuf = config::config_dir().join("config.json");
    let text = match std::fs::read_to_string(&path) {
        Ok(text) => text,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => return 0,
        Err(err) => {
            let message = format!("cannot read {}: {err}", path.display());
            unsafe { copy_out(&message, report, report_len) };
            return -1;
        }
    };

    let value: serde_json::Value = match serde_json::from_str(&config::strip_comments(&text)) {
        Ok(value) => value,
        Err(err) => {
            let message = format!("{}: {err}", path.display());
            unsafe { copy_out(&message, report, report_len) };
            return -1;
        }
    };

    let mut settings = Settings::default();
    if let Some(provider) = value.get("provider").and_then(|v| v.as_str()) {
        if !provider.is_empty() {
            settings.provider = provider.to_string();
        }
    }
    if let Some(ui) = value.get("ui") {
        if let Some(v) = ui.get("activation_msec").and_then(|v| v.as_i64()) {
            settings.activation_msec = v as i32;
        }
        if let Some(v) = ui.get("notification_timeout").and_then(|v| v.as_i64()) {
            settings.notification_timeout = v as i32;
        }
    }
    if let Some(audio) = value.get("audio") {
        if let Some(v) = audio.get("denoise").and_then(|v| v.as_str()) {
            if let Some(denoise) = Denoise::parse(v) {
                settings.denoise = denoise;
            }
        }
        if let Some(v) = audio.get("lufs_target").and_then(|v| v.as_f64()) {
            settings.lufs_target = v;
        }
        if let Some(v) = audio.get("speex_level").and_then(|v| v.as_i64()) {
            settings.speex_level = v as i32;
        }
        if let Some(v) = audio.get("crest_threshold").and_then(|v| v.as_f64()) {
            settings.crest_threshold = v;
        }
    }

    let provider = std::ffi::CString::new(settings.provider.clone()).unwrap_or_default();
    let mut provider = std::mem::ManuallyDrop::new(provider);
    unsafe {
        (*out).provider = provider.as_ptr();
        (*out).activation_msec = settings.activation_msec;
        (*out).notification_timeout = settings.notification_timeout;
        (*out).denoise = match settings.denoise {
            Denoise::None => 0,
            Denoise::Speexdsp => 1,
            Denoise::DeepFilter => 2,
        };
        (*out).lufs_target = settings.lufs_target;
        (*out).speex_level = settings.speex_level;
        (*out).crest_threshold = settings.crest_threshold;
    }
    // The provider string is intentionally kept alive for the process
    // lifetime: the shell copies it immediately, and leaking one small
    // allocation per launch is cheaper than transferring ownership.
    std::mem::forget(std::mem::take(&mut *provider));

    let note = format!(
        "migrated {} into fcitx5 configuration; the file is no longer read",
        path.display()
    );
    unsafe { copy_out(&note, report, report_len) };
    1
}

#[no_mangle]
pub extern "C" fn ainput_string_free(text: *mut c_char) {
    if text.is_null() {
        return;
    }
    // SAFETY: only strings produced by CString::into_raw are passed here.
    drop(unsafe { std::ffi::CString::from_raw(text) });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn copy_out_reports_size_and_truncates_safely() {
        let mut buf = [0i8; 8];
        let written = unsafe { copy_out("hello", buf.as_mut_ptr(), buf.len()) };
        assert_eq!(written, 5);
        let text = unsafe { CStr::from_ptr(buf.as_ptr()) }.to_str().unwrap();
        assert_eq!(text, "hello");
    }

    #[test]
    fn copy_out_rejects_short_buffer() {
        let mut buf = [0i8; 2];
        let written = unsafe { copy_out("hello", buf.as_mut_ptr(), buf.len()) };
        assert_eq!(written, -1);
    }

    #[test]
    fn provider_surface_matches_registry() {
        assert_eq!(ainput_provider_count(), 5);
        let mut buf = [0i8; 64];
        assert!(ainput_provider_id(0, buf.as_mut_ptr(), buf.len()) > 0);
        let first = unsafe { CStr::from_ptr(buf.as_ptr()) }.to_str().unwrap();
        assert_eq!(first, "qwen");
        assert_eq!(ainput_provider_id(99, buf.as_mut_ptr(), buf.len()), -1);
    }

    #[test]
    fn template_surface_returns_text() {
        let mut buf = vec![0i8; 4096];
        let written = ainput_config_template(1, buf.as_mut_ptr(), buf.len());
        assert!(written > 0);
    }
}
