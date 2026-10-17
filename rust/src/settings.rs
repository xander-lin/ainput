//! Runtime settings.
//!
//! The authoritative source is fcitx5's `Configuration` (visible in
//! fcitx5-configtool). The shell pushes a snapshot on every change; each
//! recording captures its own copy, so a recording always uses the settings
//! that were current when it started — including the provider, which travels
//! with the job instead of being read globally at recognition time.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Denoise {
    None,
    Speexdsp,
    DeepFilter,
}

impl Denoise {
    pub fn from_i32(value: i32) -> Self {
        match value {
            1 => Denoise::Speexdsp,
            2 => Denoise::DeepFilter,
            _ => Denoise::None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Denoise::None => "none",
            Denoise::Speexdsp => "speexdsp",
            Denoise::DeepFilter => "deepfilter",
        }
    }

    /// Parses the string form used by the config file and the config UI.
    pub fn parse(value: &str) -> Option<Self> {
        match value.trim().to_ascii_lowercase().as_str() {
            "none" => Some(Denoise::None),
            "speexdsp" => Some(Denoise::Speexdsp),
            "deepfilter" => Some(Denoise::DeepFilter),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct Credentials {
    pub qwen_api_key: String,
    pub qwen_model: String,
    pub doubao_api_key: String,
    pub doubao_model: String,
}

impl Credentials {
    /// Returns `Some` only for a non-empty value that is not the seeded
    /// placeholder, so an untouched GUI field never blocks the JSON file.
    pub fn value(candidate: &str) -> Option<String> {
        let trimmed = candidate.trim();
        if trimmed.is_empty() || trimmed == crate::config::PLACEHOLDER_KEY {
            None
        } else {
            Some(trimmed.to_string())
        }
    }
}

/// The credentials currently in effect, readable without an engine handle.
///
/// Providers, and the display name shown in the switch notification, resolve
/// their effective key and model through this, so the notification reflects
/// what will actually run rather than what the file happens to say.
static ACTIVE_CREDENTIALS: std::sync::RwLock<Option<Credentials>> =
    std::sync::RwLock::new(None);

pub fn set_active_credentials(credentials: Credentials) {
    if let Ok(mut slot) = ACTIVE_CREDENTIALS.write() {
        *slot = Some(credentials);
    }
}

pub fn active_credentials() -> Credentials {
    ACTIVE_CREDENTIALS
        .read()
        .ok()
        .and_then(|slot| slot.clone())
        .unwrap_or_default()
}

#[derive(Debug, Clone)]
pub struct Settings {
    pub provider: String,
    pub activation_msec: i32,
    pub notification_timeout: i32,
    /// Watchdog: force-stop a recording that never saw its key release.
    /// Default 120 s; 0 disables it.
    pub max_recording_sec: i32,
    /// Microphone fragment size. Default 20; only worth raising on a device
    /// that clips the end of a phrase.
    pub buffer_ms: i32,
    pub denoise: Denoise,
    pub lufs_target: f64,
    pub speex_level: i32,
    pub crest_threshold: f64,
    pub credentials: Credentials,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            provider: "qwen".to_string(),
            activation_msec: 300,
            notification_timeout: 2000,
            max_recording_sec: 120,
            buffer_ms: 20,
            denoise: Denoise::None,
            lufs_target: -16.0,
            speex_level: -15,
            crest_threshold: 2.4,
            credentials: Credentials::default(),
        }
    }
}

impl Settings {
    /// Clamps out-of-range values instead of failing, so a stale config file
    /// can never make the addon unusable.
    pub fn sanitized(mut self) -> Self {
        self.activation_msec = self.activation_msec.clamp(50, 5000);
        self.notification_timeout = self.notification_timeout.clamp(200, 20000);
        // Negative would mean "disable"; anything else huge is still bounded
        // so a typo cannot pin the microphone open for a day.
        self.max_recording_sec = if self.max_recording_sec <= 0 {
            0
        } else {
            self.max_recording_sec.clamp(5, 3600)
        };
        self.buffer_ms = self.buffer_ms.clamp(6, 1000);
        if !(self.lufs_target > -100.0 && self.lufs_target < 0.0) {
            self.lufs_target = -16.0;
        }
        self.speex_level = self.speex_level.clamp(-100, 0);
        if self.crest_threshold < 0.0 {
            self.crest_threshold = 0.0;
        }
        self
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_and_placeholder_credential_are_unset() {
        assert_eq!(Credentials::value(""), None);
        assert_eq!(Credentials::value("   "), None);
        assert_eq!(Credentials::value(crate::config::PLACEHOLDER_KEY), None);
        assert_eq!(
            Credentials::value("  sk-real  "),
            Some("sk-real".to_string())
        );
    }

    #[test]
    fn gui_model_overrides_the_file_for_display() {
        // Touches the config directory and the active credentials, both of
        // which are process-wide; serialise with the other such tests.
        let _lock = crate::config::test_lock();
        // The switch notification must show what will actually run.
        let credentials = Credentials {
            qwen_model: "qwen-audio-3.0-asr-flash".to_string(),
            ..Credentials::default()
        };
        set_active_credentials(credentials.clone());
        let dir = std::env::temp_dir().join("ainput-display-test");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(
            dir.join("qwen.json"),
            r#"{"api_key": "sk-file", "model": "from-file"}"#,
        )
        .unwrap();
        let previous = crate::config::test_config_dir(dir);
        assert_eq!(
            crate::asr::display_name("qwen"),
            "Qwen · qwen-audio-3.0-asr-flash"
        );
        // Clearing the override falls back to the file.
        set_active_credentials(Credentials::default());
        assert_eq!(crate::asr::display_name("qwen"), "Qwen · from-file");
        set_active_credentials(Credentials::default());
        crate::config::restore_config_dir(previous);
    }

    #[test]
    fn capture_defaults_are_sane() {
        use crate::capture::TAIL_DRAIN;
        // Measured: nothing is in flight at stop, so waiting cannot recover
        // audio and would only pad the recording with new audio.
        assert_eq!(TAIL_DRAIN, std::time::Duration::ZERO);

        let settings = Settings::default();
        assert_eq!(settings.buffer_ms, 20);
        // 16 kHz mono S16 = 32 bytes per millisecond.
        assert_eq!(settings.buffer_ms as u32 * 32, 640);

        // The floor measured on real hardware was 5.375 ms; the clamp must
        // never let the value go below where PulseAudio starts rounding up.
        let tiny = Settings {
            buffer_ms: 1,
            ..Settings::default()
        };
        assert_eq!(tiny.sanitized().buffer_ms, 6);
    }

    #[test]
    fn watchdog_limit_is_configurable_and_bounded() {
        assert_eq!(Settings::default().max_recording_sec, 120);

        let clamp = |value: i32| {
            Settings {
                max_recording_sec: value,
                ..Settings::default()
            }
            .sanitized()
            .max_recording_sec
        };
        assert_eq!(clamp(30), 30);
        // 0 and negatives disable the watchdog.
        assert_eq!(clamp(0), 0);
        assert_eq!(clamp(-5), 0);
        // A typo must not leave the microphone open for a day.
        assert_eq!(clamp(999_999), 3600);
        assert_eq!(clamp(1), 5);
    }

    #[test]
    fn removing_debounce_kept_the_other_limits() {
        // The debounce option was retired; the rest of the clamping must stay.
        let clamped = Settings {
            activation_msec: 10_000,
            notification_timeout: 1,
            speex_level: 5,
            crest_threshold: -1.0,
            lufs_target: 0.0,
            ..Settings::default()
        }
        .sanitized();
        assert_eq!(clamped.activation_msec, 5000);
        assert_eq!(clamped.notification_timeout, 200);
        assert_eq!(clamped.speex_level, 0);
        assert_eq!(clamped.crest_threshold, 0.0);
        assert_eq!(clamped.lufs_target, -16.0);
    }
}
