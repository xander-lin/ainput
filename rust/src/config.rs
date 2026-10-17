//! Provider config files: `~/.config/ainput/<id>.json`.
//!
//! These files hold only per-vendor data (credentials, model ids, endpoints,
//! paths, timeouts). Everything shared — provider selection, key timing, the
//! audio pipeline — lives in fcitx5's own configuration so it appears in
//! fcitx5-configtool.

use std::collections::BTreeSet;
use std::path::PathBuf;

use serde_json::Value;

use crate::templates;

pub fn config_dir() -> PathBuf {
    match std::env::var("HOME") {
        Ok(home) if !home.is_empty() => PathBuf::from(home).join(".config/ainput"),
        _ => PathBuf::from("/tmp/ainput_cfg"),
    }
}

pub fn config_path(name: &str) -> PathBuf {
    config_dir().join(name)
}

pub fn read_text(name: &str) -> Option<String> {
    std::fs::read_to_string(config_path(name)).ok()
}

/// Reads and parses a config file. `Err` carries a human message suitable for
/// the input panel; `Ok(None)` means the file does not exist.
pub fn read_json(name: &str) -> Result<Option<Value>, String> {
    let path = config_path(name);
    let text = match std::fs::read_to_string(&path) {
        Ok(text) => text,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(err) => return Err(format!("{}: {err}", path.display())),
    };
    let stripped = strip_comments(&text);
    serde_json::from_str(&stripped)
        .map(Some)
        .map_err(|err| format!("{}: {err}", path.display()))
}

/// Rewrites `//` line comments as spaces so byte offsets in error messages
/// still point at the original file, and the text stays valid JSON.
pub fn strip_comments(text: &str) -> String {
    let bytes = text.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut in_string = false;
    let mut escaped = false;
    let mut index = 0;
    while index < bytes.len() {
        let byte = bytes[index];
        if in_string {
            out.push(byte);
            if escaped {
                escaped = false;
            } else if byte == b'\\' {
                escaped = true;
            } else if byte == b'"' {
                in_string = false;
            }
            index += 1;
            continue;
        }
        if byte == b'"' {
            in_string = true;
            out.push(byte);
            index += 1;
            continue;
        }
        if byte == b'/' && index + 1 < bytes.len() && bytes[index + 1] == b'/' {
            while index < bytes.len() && bytes[index] != b'\n' {
                out.push(b' ');
                index += 1;
            }
            continue;
        }
        out.push(byte);
        index += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Seeds `name` from `template` when the file is missing (mode 0600).
///
/// Existing files are never touched. Returns true when a file was created.
pub fn seed_if_missing(name: &str, template: fn() -> String) -> bool {
    let path = config_path(name);
    if path.exists() {
        return false;
    }
    if let Some(parent) = path.parent() {
        if std::fs::create_dir_all(parent).is_err() {
            return false;
        }
    }
    let body = template();
    match std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
    {
        Ok(mut file) => {
            use std::io::Write;
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600));
            if file.write_all(body.as_bytes()).is_ok() {
                eprintln!(
                    "Ainput: {} was missing; recreated the commented template",
                    path.display()
                );
                true
            } else {
                false
            }
        }
        Err(_) => false,
    }
}

/// Reads a config file, seeding its template first when it is missing.
pub fn read_or_seed(name: &str, template: fn() -> String) -> Option<String> {
    seed_if_missing(name, template);
    read_text(name)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldType {
    String,
    Int,
    Bool,
    StringArray,
    Object,
}

#[derive(Debug, Clone)]
pub struct Field {
    pub name: &'static str,
    pub ty: FieldType,
    pub required: bool,
}

#[derive(Debug, Clone)]
pub struct Issue {
    pub fatal: bool,
    pub message: String,
}

impl Issue {
    pub fn warning(message: impl Into<String>) -> Self {
        Self {
            fatal: false,
            message: message.into(),
        }
    }
}

impl std::fmt::Display for Issue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}

fn type_matches(value: &Value, ty: FieldType) -> bool {
    match ty {
        FieldType::String => value.is_string(),
        FieldType::Int => value.is_i64() || value.is_u64(),
        FieldType::Bool => value.is_boolean(),
        FieldType::StringArray => value
            .as_array()
            .map(|items| items.iter().all(Value::is_string))
            .unwrap_or(false),
        FieldType::Object => value.is_object(),
    }
}

fn type_label(ty: FieldType) -> &'static str {
    match ty {
        FieldType::String => "a string",
        FieldType::Int => "an integer",
        FieldType::Bool => "true/false",
        FieldType::StringArray => "an array of strings",
        FieldType::Object => "an object",
    }
}

fn edit_distance(a: &str, b: &str) -> usize {
    let a: Vec<char> = a.chars().collect();
    let b: Vec<char> = b.chars().collect();
    let mut prev: Vec<usize> = (0..=b.len()).collect();
    let mut current = vec![0usize; b.len() + 1];
    for i in 1..=a.len() {
        current[0] = i;
        for j in 1..=b.len() {
            let cost = if a[i - 1] == b[j - 1] { 0 } else { 1 };
            current[j] = (prev[j] + 1).min(current[j - 1] + 1).min(prev[j - 1] + cost);
        }
        std::mem::swap(&mut prev, &mut current);
    }
    prev[b.len()]
}

/// Validates a parsed object against `fields`, producing actionable warnings.
pub fn validate(value: &Value, fields: &[Field], prefix: &str) -> Vec<Issue> {
    let mut issues = Vec::new();
    let object = match value.as_object() {
        Some(object) => object,
        None => {
            issues.push(Issue {
                fatal: true,
                message: format!("{prefix}must be a JSON object"),
            });
            return issues;
        }
    };

    let known: BTreeSet<&str> = fields.iter().map(|f| f.name).collect();
    for key in object.keys() {
        if known.contains(key.as_str()) {
            continue;
        }
        let lower = key.to_ascii_lowercase();
        let suggestion = fields
            .iter()
            .map(|f| {
                let distance = edit_distance(&lower, f.name);
                // A user writing "modelname" for "model" is a prefix match,
                // not a near-miss by edit distance.
                let related = f.name.starts_with(&lower)
                    || lower.starts_with(f.name)
                    || distance <= 3;
                (distance, related, f.name)
            })
            .filter(|(_, related, _)| *related)
            .min_by_key(|(distance, _, _)| *distance)
            .map(|(_, _, name)| format!(" (did you mean \"{name}\"?)"))
            .unwrap_or_default();
        issues.push(Issue::warning(format!(
            "{prefix}unknown field \"{key}\"{suggestion}"
        )));
    }

    for field in fields {
        match object.get(field.name) {
            None => {
                if field.required {
                    issues.push(Issue::warning(format!(
                        "{prefix}missing required field \"{}\"",
                        field.name
                    )));
                }
            }
            Some(value) => {
                if !type_matches(value, field.ty) {
                    issues.push(Issue::warning(format!(
                        "{prefix}field \"{}\" expects {}",
                        field.name,
                        type_label(field.ty)
                    )));
                }
            }
        }
    }
    issues
}

fn s(name: &'static str, required: bool) -> Field {
    Field {
        name,
        ty: FieldType::String,
        required,
    }
}

fn i(name: &'static str) -> Field {
    Field {
        name,
        ty: FieldType::Int,
        required: false,
    }
}

fn b(name: &'static str) -> Field {
    Field {
        name,
        ty: FieldType::Bool,
        required: false,
    }
}

pub fn qwen_fields() -> Vec<Field> {
    vec![
        s("api_key", true),
        s("model", false),
        s("endpoint", false),
        Field {
            name: "language_hints",
            ty: FieldType::StringArray,
            required: false,
        },
        Field {
            name: "vocabulary",
            ty: FieldType::Object,
            required: false,
        },
        s("vocabulary_id", false),
        b("keep_dialect"),
        b("speaker_diarization"),
        i("timeout_sec"),
    ]
}

pub fn doubao_fields() -> Vec<Field> {
    vec![
        s("api_key", true),
        s("resource_id", true),
        s("model_name", false),
        b("enable_itn"),
        b("enable_punc"),
        i("timeout_sec"),
    ]
}

pub fn local_fields() -> Vec<Field> {
    vec![s("model_dir", false), s("bin_path", false), i("timeout_sec")]
}

/// Reports issues to stderr and the diagnostic log.
pub fn report(owner: &str, file: &str, issues: &[Issue]) {
    for issue in issues {
        eprintln!(
            "Ainput {owner}: {file}: {}{}",
            if issue.fatal { "error: " } else { "warning: " },
            issue.message
        );
        crate::diagnostic::event(
            "config",
            "issue",
            &[
                ("file", file.to_string()),
                ("owner", owner.to_string()),
                ("message", issue.message.clone()),
            ],
        );
    }
}

/// First issue, used for the switch notification.
pub fn first_issue(issues: &[Issue]) -> Option<String> {
    issues.first().map(|issue| issue.message.clone())
}

/// Expands a leading `~` to `$HOME`.
pub fn expand_path(path: &str) -> PathBuf {
    if let Some(rest) = path.strip_prefix('~') {
        let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".to_string());
        return PathBuf::from(format!("{home}{rest}"));
    }
    PathBuf::from(path)
}

/// Seeds all provider templates. Called once at startup so a new user can open
/// every file and see the complete set of options.
pub fn seed_all() {
    seed_if_missing("qwen.json", templates::qwen);
    seed_if_missing("doubao.json", templates::doubao);
    seed_if_missing("zipformer.json", templates::zipformer);
    seed_if_missing("fire_red.json", templates::fire_red);
}

pub const PLACEHOLDER_KEY: &str = "PASTE_YOUR_KEY_HERE";

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn comments_are_replaced_by_spaces() {
        let src = "{\n  // note\n  \"a\": 1\n}\n";
        let out = strip_comments(src);
        assert_eq!(out.len(), src.len());
        assert!(!out.contains("//"));
        assert!(serde_json::from_str::<Value>(&out).is_ok());
    }

    #[test]
    fn comments_inside_strings_are_kept() {
        let src = r#"{"url": "https://example.com/x"}"#;
        assert_eq!(strip_comments(src), src);
    }

    #[test]
    fn unknown_field_gets_suggestion() {
        let value = serde_json::json!({"api_key": "sk", "modelname": "x"});
        let issues = validate(&value, &qwen_fields(), "");
        assert!(issues.iter().any(|i| i.message.contains("did you mean \"model\"")));
    }

    #[test]
    fn type_mismatch_is_reported() {
        let value = serde_json::json!({"api_key": 42});
        let issues = validate(&value, &qwen_fields(), "");
        assert!(issues.iter().any(|i| i.message.contains("expects a string")));
    }

    #[test]
    fn missing_required_is_reported() {
        let value = serde_json::json!({"model": "m"});
        let issues = validate(&value, &qwen_fields(), "");
        assert!(issues.iter().any(|i| i.message.contains("missing required field \"api_key\"")));
    }

    #[test]
    fn valid_config_has_no_issues() {
        let value = serde_json::json!({"api_key": "sk", "timeout_sec": 60});
        assert!(validate(&value, &qwen_fields(), "").is_empty());
    }

    #[test]
    fn tilde_expands_to_home() {
        std::env::set_var("HOME", "/tmp/ainput-home-test");
        assert_eq!(expand_path("~/x"), PathBuf::from("/tmp/ainput-home-test/x"));
    }
}
