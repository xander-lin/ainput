//! Alibaba DashScope (Model Studio) Qwen ASR.
//!
//! Synchronous multimodal call: the WAV is inlined as a base64 data URL, so no
//! public URL or upload step is needed. Two wire formats exist and the model
//! name decides which one is used — see `legacy_format`.

use std::sync::atomic::Ordering;

use serde_json::{json, Value};

use super::{Input, Provider};
use crate::config;
use crate::error::{Error, Result};
use crate::http::{self, TransportError};
use crate::settings::Credentials;
use crate::templates;

const DEFAULT_MODEL: &str = "qwen-audio-3.1-asr-flash";
const DEFAULT_ENDPOINT: &str =
    "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";

pub struct Qwen;

struct Settings {
    api_key: String,
    model: String,
    endpoint: String,
    language_hints: Option<Value>,
    vocabulary: Option<Value>,
    vocabulary_id: String,
    keep_dialect: bool,
    speaker_diarization: bool,
    timeout_secs: i64,
}

impl Settings {
    fn load(credentials: &Credentials) -> Result<Self> {
        config::read_or_seed("qwen.json", templates::qwen);
        let value = match config::read_json("qwen.json") {
            Ok(Some(value)) => value,
            Ok(None) => {
                return Err(Error::config_missing(
                    "qwen.json does not exist; run once to regenerate the template",
                ))
            }
            Err(err) => return Err(Error::config_invalid(err)),
        };
        let issues = config::validate(&value, &config::qwen_fields(), "");
        config::report("qwen", "qwen.json", &issues);
        if let Some(issue) = issues.iter().find(|issue| issue.fatal) {
            return Err(Error::config_invalid(issue.message.clone()));
        }

        // A value typed into fcitx5-configtool wins; otherwise the file, then
        // the built-in default. Empty means "not set here", never "clear it".
        let file_key = value
            .get("api_key")
            .and_then(Value::as_str)
            .unwrap_or_default();
        let api_key = Credentials::value(&credentials.qwen_api_key)
            .or_else(|| Credentials::value(file_key))
            .ok_or_else(|| {
                Error::config_missing(
                    "fill QwenApiKey in fcitx5-configtool, or api_key in ~/.config/ainput/qwen.json",
                )
            })?;

        let file_model = value
            .get("model")
            .and_then(Value::as_str)
            .unwrap_or_default();

        Ok(Self {
            api_key,
            model: Credentials::value(&credentials.qwen_model)
                .or_else(|| Credentials::value(file_model))
                .unwrap_or_else(|| DEFAULT_MODEL.to_string()),
            endpoint: value
                .get("endpoint")
                .and_then(Value::as_str)
                .filter(|s| !s.is_empty())
                .unwrap_or(DEFAULT_ENDPOINT)
                .to_string(),
            language_hints: value.get("language_hints").cloned(),
            vocabulary: value.get("vocabulary").cloned(),
            vocabulary_id: value
                .get("vocabulary_id")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string(),
            keep_dialect: value
                .get("keep_dialect")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            speaker_diarization: value
                .get("speaker_diarization")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            timeout_secs: value
                .get("timeout_sec")
                .and_then(Value::as_i64)
                .filter(|v| *v > 0)
                .unwrap_or(60),
        })
    }
}

/// Legacy models (`qwen3-asr-flash`, `qwen2-*`) use the older request shape.
fn legacy_format(model: &str) -> bool {
    let lower = model.to_ascii_lowercase();
    lower.starts_with("qwen3-asr-flash")
        || lower.starts_with("qwen2-audio")
        || lower.starts_with("qwen2-asr")
}

fn build_body(settings: &Settings, data_uri: &str) -> Value {
    if legacy_format(&settings.model) {
        return json!({
            "model": settings.model,
            "input": {
                "messages": [{
                    "content": [{ "audio": data_uri }],
                    "role": "user"
                }]
            },
            "parameters": {
                "asr_options": { "enable_itn": false }
            }
        });
    }

    let mut parameters = json!({
        "format": "wav",
        "sample_rate": "16000"
    });
    if let Some(hints) = &settings.language_hints {
        if hints.as_array().map(|a| !a.is_empty()).unwrap_or(false) {
            parameters["language_hints"] = hints.clone();
        }
    }
    if settings.keep_dialect {
        parameters["keep_dialect"] = json!(true);
    }
    if settings.speaker_diarization {
        parameters["speaker_diarization_enabled"] = json!(true);
    }
    if let Some(vocabulary) = &settings.vocabulary {
        if vocabulary.as_object().map(|o| !o.is_empty()).unwrap_or(false) {
            parameters["vocabulary"] = vocabulary.clone();
        }
    }
    if !settings.vocabulary_id.is_empty() {
        parameters["vocabulary_id"] = json!(settings.vocabulary_id);
    }

    json!({
        "model": settings.model,
        "input": {
            "messages": [{
                "role": "user",
                "content": [{
                    "type": "input_audio",
                    "input_audio": { "data": data_uri }
                }]
            }]
        },
        "parameters": parameters
    })
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

fn extract_text(body: &Value) -> Option<String> {
    let output = body.get("output")?;
    if let Some(text) = output.get("text").and_then(Value::as_str) {
        if !text.is_empty() {
            return Some(text.to_string());
        }
    }
    if let Some(text) = output
        .get("sentence")
        .and_then(|s| s.get("text"))
        .and_then(Value::as_str)
    {
        if !text.is_empty() {
            return Some(text.to_string());
        }
    }
    // Legacy shape: output.choices[0].message.content[0].text
    output
        .get("choices")
        .and_then(Value::as_array)
        .and_then(|choices| choices.first())
        .and_then(|choice| choice.get("message"))
        .and_then(|message| message.get("content"))
        .and_then(Value::as_array)
        .and_then(|content| content.first())
        .and_then(|item| item.get("text"))
        .and_then(Value::as_str)
        .filter(|text| !text.is_empty())
        .map(str::to_string)
}

fn classify(status: u64, code: &str, message: &str) -> crate::error::Category {
    use crate::error::Category;
    let code = code.to_ascii_lowercase();
    let message_lower = message.to_ascii_lowercase();
    let looks_model = code.contains("model")
        || message_lower.contains("model not found")
        || message_lower.contains("model not exist")
        || message_lower.contains("does not exist")
        || message_lower.contains("unsupported model");
    if status == 401 || status == 403 {
        return Category::AuthRejected;
    }
    if looks_model {
        return Category::ModelNotFound;
    }
    if code.contains("invalidapikey") || message_lower.contains("invalid api key") {
        return Category::AuthRejected;
    }
    if status == 429 || status >= 500 {
        return Category::ServiceUnavailable;
    }
    if status == 408 {
        return Category::Timeout;
    }
    if status >= 400 {
        return Category::InvalidRequest;
    }
    Category::Unknown
}

impl Provider for Qwen {
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
        let data_uri = format!("data:audio/wav;base64,{}", base64_encode(&bytes));
        let body = build_body(&settings, &data_uri);
        let body = serde_json::to_string(&body)
            .map_err(|e| Error::runtime(format!("cannot build request: {e}")))?;

        let authorization = format!("Bearer {}", settings.api_key);
        let response = http::post_json(
            &settings.endpoint,
            &[("Authorization", authorization.as_str()), ("Accept", "application/json")],
            &body,
            std::time::Duration::from_secs(settings.timeout_secs.max(1) as u64),
            input.cancel,
        )
        .map_err(|err| match err {
            TransportError::Cancelled => Error::timeout("cancelled"),
            TransportError::Timeout(message) => {
                Error::timeout(format!("Qwen request timed out: {message}"))
            }
            TransportError::Network(message) => {
                Error::network(format!("Qwen network error: {message}"))
            }
        })?;

        let parsed: Value = serde_json::from_str(&response.body).unwrap_or(Value::Null);

        if response.status != 200 {
            let code = parsed
                .get("code")
                .and_then(Value::as_str)
                .unwrap_or_default()
                .to_string();
            let message = parsed
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or("no message")
                .to_string();
            let category = classify(response.status, &code, &message);
            return Err(Error::new(
                category,
                format!("Qwen HTTP {} {}: {}", response.status, code, message),
            ));
        }

        match extract_text(&parsed) {
            Some(text) if !text.trim().is_empty() => Ok(text),
            _ => Err(Error::empty_result("Qwen returned no text")),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_matches_known_values() {
        assert_eq!(base64_encode(b""), "");
        assert_eq!(base64_encode(b"f"), "Zg==");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
        assert_eq!(base64_encode(b"foo"), "Zm9v");
        assert_eq!(base64_encode(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn legacy_models_are_detected() {
        assert!(legacy_format("qwen3-asr-flash"));
        assert!(legacy_format("qwen2-audio-turbo"));
        assert!(!legacy_format("qwen-audio-3.1-asr-flash"));
        assert!(!legacy_format("fun-asr-flash-2026-06-15"));
    }

    #[test]
    fn extracts_new_generation_text() {
        let body = serde_json::json!({"output": {"text": "你好世界"}});
        assert_eq!(extract_text(&body).as_deref(), Some("你好世界"));
    }

    #[test]
    fn extracts_sentence_fallback() {
        let body = serde_json::json!({"output": {"sentence": {"text": "第一句"}}});
        assert_eq!(extract_text(&body).as_deref(), Some("第一句"));
    }

    #[test]
    fn extracts_legacy_text() {
        let body = serde_json::json!({
            "output": {"choices": [{"message": {"content": [{"text": "旧格式"}]}}]}
        });
        assert_eq!(extract_text(&body).as_deref(), Some("旧格式"));
    }

    #[test]
    fn http_401_is_auth() {
        use crate::error::Category;
        assert_eq!(classify(401, "", ""), Category::AuthRejected);
        assert_eq!(classify(403, "", ""), Category::AuthRejected);
    }

    #[test]
    fn model_errors_are_model_not_found() {
        use crate::error::Category;
        assert_eq!(
            classify(400, "InvalidParameter", "model not found"),
            Category::ModelNotFound
        );
    }

    #[test]
    fn rate_limit_is_service_unavailable() {
        use crate::error::Category;
        assert_eq!(classify(429, "", ""), Category::ServiceUnavailable);
    }

    #[test]
    fn request_body_shape() {
        let settings = Settings {
            api_key: "k".into(),
            model: "qwen-audio-3.1-asr-flash".into(),
            endpoint: DEFAULT_ENDPOINT.into(),
            language_hints: Some(serde_json::json!(["zh"])),
            vocabulary: None,
            vocabulary_id: String::new(),
            keep_dialect: false,
            speaker_diarization: false,
            timeout_secs: 60,
        };
        let body = build_body(&settings, "data:audio/wav;base64,AAA");
        let content = &body["input"]["messages"][0]["content"][0];
        assert_eq!(content["type"], "input_audio");
        assert_eq!(body["parameters"]["format"], "wav");
        assert_eq!(body["parameters"]["sample_rate"], "16000");
        assert_eq!(body["parameters"]["language_hints"][0], "zh");
        assert!(body["parameters"].get("vocabulary").is_none());
    }
}
