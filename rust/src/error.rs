//! Error taxonomy shared with the C++ shell.
//!
//! Every failure that reaches the user must carry a category, so the shell can
//! render one actionable sentence instead of dumping a provider's raw text.

use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum Category {
    ConfigMissing = 0,
    ConfigInvalid = 1,
    AuthRejected = 2,
    ModelNotFound = 3,
    LocalSetup = 4,
    InvalidRequest = 5,
    Network = 6,
    ServiceUnavailable = 7,
    Timeout = 8,
    NoSpeech = 9,
    EmptyResult = 10,
    AudioData = 11,
    Runtime = 12,
    Unknown = 13,
}

impl Category {
    pub fn as_i32(self) -> i32 {
        self as i32
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Category::ConfigMissing => "config_missing",
            Category::ConfigInvalid => "config_invalid",
            Category::AuthRejected => "auth_rejected",
            Category::ModelNotFound => "model_not_found",
            Category::LocalSetup => "local_setup",
            Category::InvalidRequest => "invalid_request",
            Category::Network => "network",
            Category::ServiceUnavailable => "service_unavailable",
            Category::Timeout => "timeout",
            Category::NoSpeech => "no_speech",
            Category::EmptyResult => "empty_result",
            Category::AudioData => "audio_data",
            Category::Runtime => "runtime",
            Category::Unknown => "unknown",
        }
    }
}

#[derive(Debug, Clone)]
pub struct Error {
    pub category: Category,
    pub detail: String,
}

impl Error {
    pub fn new(category: Category, detail: impl Into<String>) -> Self {
        Self {
            category,
            detail: detail.into(),
        }
    }

    pub fn config_missing(detail: impl Into<String>) -> Self {
        Self::new(Category::ConfigMissing, detail)
    }

    pub fn config_invalid(detail: impl Into<String>) -> Self {
        Self::new(Category::ConfigInvalid, detail)
    }

    pub fn auth(detail: impl Into<String>) -> Self {
        Self::new(Category::AuthRejected, detail)
    }

    pub fn model_not_found(detail: impl Into<String>) -> Self {
        Self::new(Category::ModelNotFound, detail)
    }

    pub fn local_setup(detail: impl Into<String>) -> Self {
        Self::new(Category::LocalSetup, detail)
    }

    pub fn network(detail: impl Into<String>) -> Self {
        Self::new(Category::Network, detail)
    }

    pub fn service_unavailable(detail: impl Into<String>) -> Self {
        Self::new(Category::ServiceUnavailable, detail)
    }

    pub fn timeout(detail: impl Into<String>) -> Self {
        Self::new(Category::Timeout, detail)
    }

    pub fn empty_result(detail: impl Into<String>) -> Self {
        Self::new(Category::EmptyResult, detail)
    }

    pub fn audio_data(detail: impl Into<String>) -> Self {
        Self::new(Category::AudioData, detail)
    }

    pub fn runtime(detail: impl Into<String>) -> Self {
        Self::new(Category::Runtime, detail)
    }

    pub fn unknown(detail: impl Into<String>) -> Self {
        Self::new(Category::Unknown, detail)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.category.as_str(), self.detail)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;
