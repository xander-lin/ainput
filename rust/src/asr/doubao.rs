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
use crate::http::{Response, TransportError};
use crate::settings::Credentials;
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
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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
        self.transcribe_with(input, super::Context::default())
    }

    fn transcribe_with(&self, input: &Input<'_>, context: super::Context<'_>) -> Result<String> {
        let settings = Settings::load(input.credentials)?;
        recognize(input, context, settings, [SUBMIT_URL, QUERY_URL])
    }
}

fn recognize(
    input: &Input<'_>,
    context: super::Context<'_>,
    settings: Settings,
    [submit_url, query_url]: [&str; 2],
) -> Result<String> {
    if input.cancel.load(Ordering::Relaxed) {
        return Err(Error::timeout("cancelled"));
    }

    let bytes = std::fs::read(input.wav_path)
        .map_err(|e| Error::audio_data(format!("cannot read WAV: {e}")))?;
    if bytes.len() <= 44 {
        return Err(Error::audio_data("recorded WAV is empty"));
    }

    let deadline = context.deadline(Duration::from_secs(settings.timeout_secs.max(1) as u64));
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

    let submit = context
        .client
        .post_json(
            context.id,
            "doubao_submit",
            submit_url,
            &[
                ("X-Api-Key", settings.api_key.as_str()),
                ("X-Api-Resource-Id", settings.resource_id.as_str()),
                ("X-Api-Request-Id", task_id.as_str()),
                ("X-Api-Sequence", "-1"),
            ],
            &submit_body,
            remaining(deadline)?.min(Duration::from_secs(30)),
            input.cancel,
        )
        .map_err(|err| classify_transport("submit", err))?;

    let code = submit.header("X-Api-Status-Code").unwrap_or_default();
    if code != STATUS_SUCCESS {
        return Err(explain(&submit, &code, input.cancel));
    }

    // ---- poll ----
    //
    // Query immediately, then back off. Sleeping first cost a fixed 300 ms
    // on every recognition even when the transcript was ready by the time
    // the submit response came back — which for short utterances is the
    // common case.
    let mut interval = Duration::ZERO;
    let mut retried_query = false;
    if context.id != 0 {
        crate::diagnostic::trace(context.id, "doubao_query");
    }
    loop {
        if input.cancel.load(Ordering::Relaxed) {
            return Err(Error::timeout("cancelled"));
        }
        let until = Instant::now() + interval.min(remaining(deadline)?);
        while let Some(wait) = until.checked_duration_since(Instant::now()) {
            if input.cancel.load(Ordering::Acquire) {
                return Err(Error::timeout("cancelled"));
            }
            std::thread::sleep(wait.min(Duration::from_millis(20)));
        }
        // Start short, then settle: the first few hundred milliseconds are
        // where a fast answer shows up, and after that polling faster than
        // the server can finish only burns requests.
        interval = if interval.is_zero() {
            Duration::from_millis(120)
        } else {
            (interval * 2).min(Duration::from_millis(800))
        };

        let query = context.client.post_json(
            0,
            "doubao_query",
            query_url,
            &[
                ("X-Api-Key", settings.api_key.as_str()),
                ("X-Api-Resource-Id", settings.resource_id.as_str()),
                ("X-Api-Request-Id", task_id.as_str()),
            ],
            "{}",
            remaining(deadline)?,
            input.cancel,
        );
        let query = match query {
            Ok(query) => {
                retried_query = false;
                query
            }
            Err(TransportError::NetworkChanged) => {
                // Submit was confirmed: resume polling the same task ID.
                if context.id != 0 {
                    crate::diagnostic::trace(context.id, "doubao_query_reconnect");
                }
                continue;
            }
            Err(TransportError::Network(_)) if !retried_query => {
                // The route may already have changed before this query
                // began. Retry once on a fresh handle once it settles.
                retried_query = true;
                continue;
            }
            Err(err) => return Err(classify_transport("query", err)),
        };

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

fn remaining(deadline: Instant) -> Result<Duration> {
    deadline
        .checked_duration_since(Instant::now())
        .filter(|left| !left.is_zero())
        .ok_or_else(|| Error::timeout("Doubao recognition timed out"))
}

fn classify_transport(stage: &str, err: TransportError) -> Error {
    match err {
        TransportError::Cancelled => Error::timeout("cancelled"),
        TransportError::NetworkChanged => {
            Error::network(format!("network changed during Doubao {stage}"))
        }
        TransportError::Timeout(message) => {
            Error::timeout(format!("Doubao {stage} timed out: {message}"))
        }
        TransportError::Network(message) => {
            Error::network(format!("Doubao {stage} failed: {message}"))
        }
    }
}

fn explain(
    response: &Response,
    code: &str,
    cancel: &std::sync::Arc<std::sync::atomic::AtomicBool>,
) -> Error {
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
    fn confirmed_submit_keeps_task_id_when_query_moves_to_new_network() {
        use crate::http::tests::read_request;
        use crate::network::MockNetwork;
        use std::io::Write;
        use std::net::TcpListener;
        use std::sync::atomic::AtomicBool;
        use std::sync::{mpsc, Arc};

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let base = format!("http://{}", listener.local_addr().unwrap());
        let network = Arc::new(MockNetwork::default());
        let client = crate::http::Transport::new(network.clone());
        let (done, completed) = mpsc::channel();
        let server = std::thread::spawn(move || {
            fn task_id(request: &str) -> &str {
                request
                    .lines()
                    .find_map(|line| {
                        let (name, value) = line.split_once(':')?;
                        name.eq_ignore_ascii_case("X-Api-Request-Id")
                            .then(|| value.trim())
                    })
                    .unwrap()
            }
            let (mut old, _) = listener.accept().unwrap();
            let submit = read_request(&mut old);
            assert!(submit.starts_with("POST /submit "));
            old.write_all(
                b"HTTP/1.1 200 OK\r\nX-Api-Status-Code: 20000000\r\nContent-Length: 0\r\n\r\n",
            )
            .unwrap();
            let query = read_request(&mut old);
            assert!(query.starts_with("POST /query "));
            assert_eq!(task_id(&query), task_id(&submit));
            network.0.fetch_add(1, Ordering::SeqCst);

            let (mut fresh, _) = listener.accept().unwrap();
            let retry = read_request(&mut fresh);
            assert!(
                retry.starts_with("POST /query "),
                "audio must not be uploaded again"
            );
            assert_eq!(task_id(&retry), task_id(&submit));
            let body = r#"{"result":{"text":"recovered"}}"#;
            write!(fresh, "HTTP/1.1 200 OK\r\nX-Api-Status-Code: 20000000\r\nContent-Length: {}\r\n\r\n{body}", body.len()).unwrap();
            let _ = completed.recv_timeout(Duration::from_secs(5));
        });
        let wav_path =
            std::env::temp_dir().join(format!("ainput-doubao-recovery-{}.wav", std::process::id()));
        crate::wav::write(&wav_path, &[1; 1600]).unwrap();
        let result = recognize(
            &Input {
                wav_path: &wav_path,
                cancel: &Arc::new(AtomicBool::new(false)),
                credentials: &Credentials::default(),
            },
            super::super::Context {
                id: 0,
                client: &client,
                deadline: None,
            },
            Settings {
                api_key: "test".into(),
                resource_id: "test".into(),
                model_name: "bigmodel".into(),
                enable_itn: true,
                enable_punc: true,
                timeout_secs: 5,
            },
            [&format!("{base}/submit"), &format!("{base}/query")],
        );
        let _ = std::fs::remove_file(wav_path);
        assert_eq!(result.unwrap(), "recovered");
        done.send(()).unwrap();
        server.join().unwrap();
    }

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
