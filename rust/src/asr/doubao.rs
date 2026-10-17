//! ByteDance Volcengine Doubao file-recognition API.
//!
//! Two phases over one deadline: submit returns a task id, then the result is
//! polled. Status lives in the `X-Api-Status-Code` response header, not in the
//! HTTP status, so both must be checked.

use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use super::{Input, Provider};
use crate::config;
use crate::error::{Category, Error, Result};
use crate::settings::Credentials;
use crate::http::{self, Response, TransportError};
use crate::templates;

const SUBMIT_URL: &str = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit";
const QUERY_URL: &str = "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query";

const STATUS_SUCCESS: &str = "20000000";
const STATUS_IN_QUEUE: &str = "20000002";
const STATUS_PROCESSING: &str = "20000001";
const STATUS_SILENT: &str = "20000003";

pub struct Doubao;

struct Settings {
    api_key: String,
    resource_id: String,
    model_name: String,
    enable_itn: bool,
    enable_punc: bool,
    timeout_secs: i64,
}

impl Settings {
    fn load(credentials: &Credentials) -> Result<Self> {
        config::read_or_seed("doubao.json", templates::doubao);
        let value = match config::read_json("doubao.json") {
            Ok(Some(value)) => value,
            Ok(None) => {
                return Err(Error::config_missing(
                    "doubao.json does not exist; run once to regenerate the template",
                ))
            }
            Err(err) => return Err(Error::config_invalid(err)),
        };
        let issues = config::validate(&value, &config::doubao_fields(), "");
        config::report("doubao", "doubao.json", &issues);
        if let Some(issue) = issues.iter().find(|issue| issue.fatal) {
            return Err(Error::config_invalid(issue.message.clone()));
        }

        let file_key = value
            .get("api_key")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let api_key = Credentials::value(&credentials.doubao_api_key)
            .or_else(|| Credentials::value(file_key))
            .ok_or_else(|| {
                Error::config_missing(
                    "fill DoubaoApiKey in fcitx5-configtool, or api_key in                      ~/.config/ainput/doubao.json",
                )
            })?;

        let resource_id = value
            .get("resource_id")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .trim()
            .to_string();
        if resource_id.is_empty() {
            return Err(Error::config_missing(
                "fill resource_id in ~/.config/ainput/doubao.json",
            ));
        }

        Ok(Self {
            api_key,
            resource_id,
            model_name: Credentials::value(&credentials.doubao_model)
                .or_else(|| {
                    Credentials::value(
                        value
                            .get("model_name")
                            .and_then(Value::as_str)
                            .unwrap_or_default(),
                    )
                })
                .unwrap_or_else(|| "bigmodel".to_string()),
            enable_itn: value
                .get("enable_itn")
                .and_then(Value::as_bool)
                .unwrap_or(true),
            enable_punc: value
                .get("enable_punc")
                .and_then(Value::as_bool)
                .unwrap_or(true),
            timeout_secs: value
                .get("timeout_sec")
                .and_then(Value::as_i64)
                .filter(|v| *v > 0)
                .unwrap_or(90),
        })
    }
}

fn base64_encode(data: &[u8]) -> String {
    const TABLE: &[u8; 64] =
        b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = *chunk.get(1).unwrap_or(&0) as u32;
        let b2 = *chunk.get(2).unwrap_or(&0) as u32;
        let triple = (b0 << 16) | (b1 << 8) | b2;
        out.push(TABLE[(triple >> 18) as usize & 63] as char);
        out.push(TABLE[(triple >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 {
            TABLE[(triple >> 6) as usize & 63] as char
        } else {
            '='
        });
        out.push(if chunk.len() > 2 {
            TABLE[triple as usize & 63] as char
        } else {
            '='
        });
    }
    out
}

fn request_id() -> String {
    // 16 random bytes rendered as hex; falls back to a time-based value when
    // /dev/urandom is unavailable.
    let mut bytes = [0u8; 16];
    if let Ok(mut file) = std::fs::File::open("/dev/urandom") {
        use std::io::Read;
        if file.read_exact(&mut bytes).is_err() {
            bytes = fallback_bytes();
        }
    } else {
        bytes = fallback_bytes();
    }
    let mut out = String::with_capacity(32);
    for byte in bytes {
        out.push_str(&format!("{byte:02x}"));
    }
    out
}

fn fallback_bytes() -> [u8; 16] {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let mut seed = now.as_nanos() as u64 ^ ((std::process::id() as u64) << 32);
    let mut bytes = [0u8; 16];
    for slot in bytes.iter_mut() {
        // xorshift64
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        *slot = (seed & 0xff) as u8;
    }
    bytes
}

fn classify_status(code: &str) -> Option<Category> {
    match code {
        STATUS_SILENT => Some(Category::NoSpeech),
        "45000001" | "45000002" | "45000151" => Some(Category::InvalidRequest),
        "45000131" => Some(Category::ServiceUnavailable),
        "45000132" => Some(Category::InvalidRequest),
        "55000031" => Some(Category::ServiceUnavailable),
        _ => None,
    }
}

fn extract_text(body: &Value) -> Option<String> {
    body.get("result")
        .and_then(|r| r.get("text"))
        .and_then(Value::as_str)
        .filter(|text| !text.trim().is_empty())
        .map(str::to_string)
}

fn message_of(body: &Value, headers: &Response, fallback: &str) -> String {
    body.get("message")
        .and_then(Value::as_str)
        .map(str::to_string)
        .or_else(|| headers.header("X-Api-Message"))
        .unwrap_or_else(|| fallback.to_string())
}

impl Provider for Doubao {
    fn transcribe(&self, input: &Input<'_>) -> Result<String> {
        let settings = Settings::load(input.credentials)?;
        if input.cancel.load(Ordering::Relaxed) {
            return Err(Error::timeout("cancelled"));
        }

        let bytes = std::fs::read(input.wav_path)
            .map_err(|e| Error::audio_data(format!("cannot read WAV: {e}")))?;
        if bytes.len() <= 44 {
            return Err(Error::audio_data("recorded WAV is empty"));
        }

        let deadline = Instant::now() + Duration::from_secs(settings.timeout_secs.max(1) as u64);
        let task_id = request_id();

        // ---- submit ----
        let submit_body = json!({
            "audio": {
                "format": "wav",
                "data": base64_encode(&bytes)
            },
            "request": {
                "model_name": settings.model_name,
                "enable_itn": settings.enable_itn,
                "enable_punc": settings.enable_punc
            }
        })
        .to_string();

        let submit = http::post_json(
            SUBMIT_URL,
            &[
                ("X-Api-Key", settings.api_key.as_str()),
                ("X-Api-Resource-Id", settings.resource_id.as_str()),
                ("X-Api-Request-Id", task_id.as_str()),
                ("X-Api-Sequence", "-1"),
            ],
            &submit_body,
            Duration::from_secs(remaining_secs(deadline).clamp(1, 30) as u64),
            input.cancel,
        )
        .map_err(|err| classify_transport("submit", err))?;

        let code = submit.header("X-Api-Status-Code").unwrap_or_default();
        if code != STATUS_SUCCESS {
            return Err(explain(&submit, &code, input.cancel));
        }

        // ---- poll ----
        let mut interval = Duration::from_millis(300);
        loop {
            if input.cancel.load(Ordering::Relaxed) {
                return Err(Error::timeout("cancelled"));
            }
            if Instant::now() >= deadline {
                return Err(Error::timeout("Doubao recognition timed out"));
            }
            std::thread::sleep(interval.min(deadline.saturating_duration_since(Instant::now())));
            interval = Duration::from_millis(800);

            let query = http::post_json(
                QUERY_URL,
                &[
                    ("X-Api-Key", settings.api_key.as_str()),
                    ("X-Api-Resource-Id", settings.resource_id.as_str()),
                    ("X-Api-Request-Id", task_id.as_str()),
                ],
                "{}",
                Duration::from_secs(remaining_secs(deadline).max(1) as u64),
                input.cancel,
            )
            .map_err(|err| classify_transport("query", err))?;

            let code = query.header("X-Api-Status-Code").unwrap_or_default();
            if code == STATUS_SUCCESS {
                let body: Value = serde_json::from_str(&query.body).unwrap_or(Value::Null);
                return match extract_text(&body) {
                    Some(text) => Ok(text),
                    None => Err(Error::empty_result("Doubao returned no text")),
                };
            }
            if code == STATUS_IN_QUEUE || code == STATUS_PROCESSING {
                continue;
            }
            return Err(explain(&query, &code, input.cancel));
        }
    }
}

fn remaining_secs(deadline: Instant) -> i64 {
    let remaining = deadline.saturating_duration_since(Instant::now());
    (remaining.as_secs() as i64).max(1)
}

fn classify_transport(stage: &str, err: TransportError) -> Error {
    match err {
        TransportError::Cancelled => Error::timeout("cancelled"),
        TransportError::Timeout(message) => {
            Error::timeout(format!("Doubao {stage} timed out: {message}"))
        }
        TransportError::Network(message) => {
            Error::network(format!("Doubao {stage} failed: {message}"))
        }
    }
}

fn explain(response: &Response, code: &str, cancel: &std::sync::Arc<std::sync::atomic::AtomicBool>) -> Error {
    if cancel.load(Ordering::Relaxed) {
        return Error::timeout("cancelled");
    }
    let body: Value = serde_json::from_str(&response.body).unwrap_or(Value::Null);
    let detail = message_of(&body, response, "no message");
    if code.is_empty() {
        if response.status == 401 || response.status == 403 {
            return Error::auth(format!("Doubao rejected the credentials: {detail}"));
        }
        if response.status == 429 || response.status >= 500 {
            return Error::service_unavailable(format!(
                "Doubao HTTP {}: {detail}",
                response.status
            ));
        }
        return Error::new(
            Category::Unknown,
            format!("Doubao HTTP {}: {detail}", response.status),
        );
    }
    if let Some(category) = classify_status(code) {
        return Error::new(category, format!("Doubao status {code}: {detail}"));
    }
    Error::unknown(format!("Doubao status {code}: {detail}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn silent_audio_maps_to_no_speech() {
        assert_eq!(classify_status("20000003"), Some(Category::NoSpeech));
    }

    #[test]
    fn busy_maps_to_service_unavailable() {
        assert_eq!(
            classify_status("55000031"),
            Some(Category::ServiceUnavailable)
        );
    }

    #[test]
    fn unknown_status_is_none() {
        assert_eq!(classify_status("20000000"), None);
    }

    #[test]
    fn extracts_result_text() {
        let body = serde_json::json!({"result": {"text": "识别结果"}});
        assert_eq!(extract_text(&body).as_deref(), Some("识别结果"));
    }

    #[test]
    fn request_ids_are_unique() {
        let a = request_id();
        let b = request_id();
        assert_eq!(a.len(), 32);
        assert_ne!(a, b);
    }
}
