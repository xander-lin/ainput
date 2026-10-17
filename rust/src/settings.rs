//! Runtime settings.
//!
//! The authoritative source is fcitx5's `Configuration` (visible in
//! fcitx5-configtool). The shell pushes a snapshot here after every change, so
//! a recording always uses the settings that were current when it started.
//! Provider credentials and model paths are NOT part of this struct: they live
//! in the per-provider JSON files.

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

#[derive(Debug, Clone)]
pub struct Settings {
    pub provider: String,
    pub activation_msec: i32,
    pub notification_timeout: i32,
    pub debounce_count: i32,
    pub denoise: Denoise,
    pub lufs_target: f64,
    pub speex_level: i32,
    pub crest_threshold: f64,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            provider: "qwen".to_string(),
            activation_msec: 300,
            notification_timeout: 2000,
            debounce_count: 2,
            denoise: Denoise::None,
            lufs_target: -16.0,
            speex_level: -15,
            crest_threshold: 2.4,
        }
    }
}

impl Settings {
    /// Clamps out-of-range values instead of failing, so a stale config file
    /// can never make the addon unusable.
    pub fn sanitized(mut self) -> Self {
        self.activation_msec = self.activation_msec.clamp(50, 5000);
        self.notification_timeout = self.notification_timeout.clamp(200, 20000);
        self.debounce_count = self.debounce_count.clamp(0, 20);
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
