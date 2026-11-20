//! ASR backends.
//!
//! One trait, one factory function, five implementations. Providers never
//! touch fcitx5 types: they receive a WAV path and a cancellation flag, and
//! return text or a categorized error.

pub mod doubao;
pub mod mock;
pub mod qwen;
pub mod sherpa;

use std::path::Path;
use std::sync::atomic::AtomicBool;
use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::config;
use crate::error::Result;
use crate::templates;

pub struct Input<'a> {
    pub wav_path: &'a Path,
    pub cancel: &'a Arc<AtomicBool>,
    /// Values from fcitx5-configtool; empty fields fall back to the JSON file.
    pub credentials: &'a crate::settings::Credentials,
}

#[derive(Clone, Copy)]
pub struct Context<'a> {
    pub id: u64,
    pub client: &'a crate::http::Transport,
    pub deadline: Option<Instant>,
}

impl Context<'_> {
    fn deadline(self, fallback: Duration) -> Instant {
        let now = Instant::now();
        match (self.deadline, now.checked_add(fallback)) {
            (Some(a), Some(b)) => a.min(b),
            (Some(a), None) | (None, Some(a)) => a,
            (None, None) => now + Duration::from_secs(120),
        }
    }

    fn remaining(self, fallback: Duration) -> Result<Duration> {
        self.deadline(fallback).checked_duration_since(Instant::now())
            .filter(|left| !left.is_zero())
            .ok_or_else(|| crate::error::Error::timeout("dictation deadline exceeded"))
    }
}

impl Default for Context<'_> {
    fn default() -> Self {
        Self { id: 0, client: crate::http::default_transport(), deadline: None }
    }
}

pub trait Provider: Send {
    fn transcribe(&self, input: &Input<'_>) -> Result<String>;

    fn transcribe_with(&self, input: &Input<'_>, _context: Context<'_>) -> Result<String> {
        self.transcribe(input)
    }
}

pub fn default_timeout(provider: &str) -> Duration {
    Duration::from_secs(match provider {
        "qwen" => 60,
        "doubao" => 90,
        "mock" => 5,
        _ => 120,
    })
}

pub fn task_timeout(provider: &str, default: Duration) -> Duration {
    if !matches!(provider, "qwen" | "doubao" | "zipformer" | "fire_red") {
        return default;
    }
    config::read_json(&format!("{provider}.json"))
        .ok().flatten()
        .and_then(|value| value.get("timeout_sec")?.as_u64())
        .filter(|&secs| secs > 0)
        .map_or(default, Duration::from_secs)
}

pub struct Descriptor {
    pub id: &'static str,
}

/// Registration order is the order `Ctrl+CapsLock` cycles through.
pub const DESCRIPTORS: &[Descriptor] = &[
    Descriptor { id: "qwen" },
    Descriptor { id: "doubao" },
    Descriptor { id: "zipformer" },
    Descriptor { id: "fire_red" },
    Descriptor { id: "mock" },
];

#[cfg(test)]
pub fn ids() -> Vec<&'static str> {
    DESCRIPTORS.iter().map(|d| d.id).collect()
}

/// Label shown in the switch notification.
///
/// Cloud backends append their configured model, local backends the model
/// directory name, and any schema problem becomes a `⚠` line.
pub fn display_name(id: &str) -> String {
    match id {
        "qwen" => {
            let (model, warning) = cloud_summary(
                "qwen.json",
                templates::qwen,
                config::qwen_fields,
                "model",
                "qwen-audio-3.1-asr-flash",
            );
            match warning {
                Some(issue) => format!("Qwen · {model}\n⚠ qwen.json: {issue}"),
                None => format!("Qwen · {model}"),
            }
        }
        "doubao" => {
            let (model, warning) = cloud_summary(
                "doubao.json",
                templates::doubao,
                config::doubao_fields,
                "model_name",
                "bigmodel",
            );
            match warning {
                Some(issue) => format!("Doubao · {model}\n⚠ doubao.json: {issue}"),
                None => format!("Doubao · {model}"),
            }
        }
        "zipformer" => {
            let (name, warning) = local_summary(
                "zipformer.json",
                templates::zipformer,
                "~/.local/share/ainput/models/zipformer-zh-en",
            );
            match warning {
                Some(issue) => format!("Zipformer · {name} (local)\n⚠ zipformer.json: {issue}"),
                None => format!("Zipformer · {name} (local)"),
            }
        }
        "fire_red" => {
            let (name, warning) = local_summary(
                "fire_red.json",
                templates::fire_red,
                "~/.local/share/ainput/models/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26",
            );
            match warning {
                Some(issue) => format!("FireRed · {name} (local)\n⚠ fire_red.json: {issue}"),
                None => format!("FireRed · {name} (local)"),
            }
        }
        "mock" => "Mock (test)".to_string(),
        other => other.to_string(),
    }
}

fn cloud_summary(
    file: &str,
    template: fn() -> String,
    fields: fn() -> Vec<config::Field>,
    model_key: &str,
    default_model: &str,
) -> (String, Option<String>) {
    config::read_or_seed(file, template);
    let value = match config::read_json(file) {
        Ok(Some(value)) => value,
        Ok(None) => return (default_model.to_string(), None),
        Err(err) => return (default_model.to_string(), Some(err)),
    };
    let issues = config::validate(&value, &fields(), "");
    let warning = config::first_issue(&issues);
    let file_model = value
        .get(model_key)
        .and_then(|v| v.as_str())
        .unwrap_or_default();
    let override_model = match file {
        "qwen.json" => &crate::settings::active_credentials().qwen_model,
        _ => &crate::settings::active_credentials().doubao_model,
    };
    let model = crate::settings::Credentials::value(override_model)
        .or_else(|| crate::settings::Credentials::value(file_model))
        .unwrap_or_else(|| default_model.to_string());
    (model, warning)
}

fn local_summary(file: &str, template: fn() -> String, default_dir: &str) -> (String, Option<String>) {
    config::read_or_seed(file, template);
    let value = match config::read_json(file) {
        Ok(Some(value)) => value,
        Ok(None) => return (default_dir.to_string(), None),
        Err(err) => return (default_dir.to_string(), Some(err)),
    };
    let issues = config::validate(&value, &config::local_fields(), "");
    let warning = config::first_issue(&issues);
    let dir = value
        .get("model_dir")
        .and_then(|v| v.as_str())
        .filter(|s| !s.is_empty())
        .unwrap_or(default_dir);
    let name = config::expand_path(dir)
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| dir.to_string());
    (name, warning)
}

pub fn create(id: &str) -> Option<Box<dyn Provider>> {
    match id {
        "qwen" => Some(Box::new(qwen::Qwen)),
        "doubao" => Some(Box::new(doubao::Doubao)),
        "zipformer" => Some(Box::new(sherpa::Sherpa::zipformer())),
        "fire_red" => Some(Box::new(sherpa::Sherpa::fire_red())),
        "mock" => Some(Box::new(mock::Mock)),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn registry_order_is_stable() {
        assert_eq!(
            ids(),
            vec!["qwen", "doubao", "zipformer", "fire_red", "mock"]
        );
    }

    #[test]
    fn every_descriptor_creates_a_provider() {
        for id in ids() {
            assert!(create(id).is_some(), "no provider for {id}");
        }
        assert!(create("nope").is_none());
    }
}
