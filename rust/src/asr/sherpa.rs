//! Local sherpa-onnx backends: streaming Zipformer and offline FireRed AED.
//!
//! Both run the engine as a child process, one process per utterance. That is
//! intentional: the model is loaded per call, which costs ~1 s (Zipformer) to
//! ~3 s (FireRed) but keeps the addon free of a resident server.
//!
//! Preflight checks (binary exists, model files exist) run before spawning so
//! a misconfiguration produces a specific message instead of a cryptic exit
//! code.

use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Duration;

use serde_json::Value;

use super::{Input, Provider};
use crate::config;
use crate::error::{Error, Result};
use crate::templates;

/// Matches the C++ implementation.
const NUM_THREADS: u32 = 30;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Flavor {
    Zipformer,
    FireRed,
}

pub struct Sherpa {
    flavor: Flavor,
}

impl Sherpa {
    pub fn zipformer() -> Self {
        Self {
            flavor: Flavor::Zipformer,
        }
    }

    pub fn fire_red() -> Self {
        Self {
            flavor: Flavor::FireRed,
        }
    }

    fn config_name(&self) -> &'static str {
        match self.flavor {
            Flavor::Zipformer => "zipformer.json",
            Flavor::FireRed => "fire_red.json",
        }
    }

    fn template(&self) -> fn() -> String {
        match self.flavor {
            Flavor::Zipformer => templates::zipformer,
            Flavor::FireRed => templates::fire_red,
        }
    }

    fn default_dir(&self) -> &'static str {
        match self.flavor {
            Flavor::Zipformer => "~/.local/share/ainput/models/zipformer-zh-en",
            Flavor::FireRed => {
                "~/.local/share/ainput/models/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26"
            }
        }
    }

    fn default_bin(&self) -> &'static str {
        match self.flavor {
            Flavor::Zipformer => "~/.local/share/ainput/sherpa-onnx/bin/sherpa-onnx",
            Flavor::FireRed => "~/.local/share/ainput/sherpa-onnx/bin/sherpa-onnx-offline",
        }
    }

    fn label(&self) -> &'static str {
        match self.flavor {
            Flavor::Zipformer => "Zipformer",
            Flavor::FireRed => "FireRed",
        }
    }

    /// Model files the engine will be told to open.
    fn required_files(&self) -> &'static [&'static str] {
        match self.flavor {
            Flavor::Zipformer => &[
                "encoder-epoch-99-avg-1.onnx",
                "decoder-epoch-99-avg-1.onnx",
                "joiner-epoch-99-avg-1.onnx",
                "tokens.txt",
            ],
            Flavor::FireRed => &["encoder.int8.onnx", "decoder.int8.onnx", "tokens.txt"],
        }
    }

    fn settings(&self) -> Result<Settings> {
        let name = self.config_name();
        config::read_or_seed(name, self.template());
        let value = match config::read_json(name) {
            Ok(Some(value)) => value,
            // Local files are optional: absence means defaults apply.
            Ok(None) => Value::Null,
            Err(err) => return Err(Error::config_invalid(err)),
        };
        if !value.is_null() {
            let issues = config::validate(&value, &config::local_fields(), "");
            config::report(name, name, &issues);
        }
        Ok(Settings {
            model_dir: config::expand_path(
                value
                    .get("model_dir")
                    .and_then(Value::as_str)
                    .filter(|s| !s.is_empty())
                    .unwrap_or(self.default_dir()),
            ),
            bin_path: config::expand_path(
                value
                    .get("bin_path")
                    .and_then(Value::as_str)
                    .filter(|s| !s.is_empty())
                    .unwrap_or(self.default_bin()),
            ),
            timeout_secs: value
                .get("timeout_sec")
                .and_then(Value::as_i64)
                .filter(|v| *v > 0)
                .unwrap_or(120) as u64,
        })
    }

    fn arguments(&self, settings: &Settings, wav: &Path) -> Vec<String> {
        let dir = settings.model_dir.to_string_lossy().to_string();
        let mut args = Vec::new();
        match self.flavor {
            Flavor::Zipformer => {
                args.push(format!("--encoder={dir}/encoder-epoch-99-avg-1.onnx"));
                args.push(format!("--decoder={dir}/decoder-epoch-99-avg-1.onnx"));
                args.push(format!("--joiner={dir}/joiner-epoch-99-avg-1.onnx"));
                args.push(format!("--tokens={dir}/tokens.txt"));
            }
            Flavor::FireRed => {
                args.push(format!("--fire-red-asr-encoder={dir}/encoder.int8.onnx"));
                args.push(format!("--fire-red-asr-decoder={dir}/decoder.int8.onnx"));
                args.push(format!("--tokens={dir}/tokens.txt"));
            }
        }
        args.push("--provider=cpu".to_string());
        args.push(format!("--num-threads={NUM_THREADS}"));
        args.push(wav.to_string_lossy().to_string());
        args
    }
}

struct Settings {
    model_dir: PathBuf,
    bin_path: PathBuf,
    timeout_secs: u64,
}

/// Pulls the last JSON object that carries a `text` field.
///
/// sherpa-onnx writes its result to stderr as a JSON line; stdout is also
/// captured because the split differs between builds.
fn extract_text(output: &str) -> Option<String> {
    for line in output.lines().rev() {
        let trimmed = line.trim();
        if !trimmed.starts_with('{') || !trimmed.contains("\"text\"") {
            continue;
        }
        if let Ok(value) = serde_json::from_str::<Value>(trimmed) {
            if let Some(text) = value.get("text").and_then(Value::as_str) {
                if !text.trim().is_empty() {
                    return Some(text.to_string());
                }
            }
        }
    }
    None
}

/// Last non-empty line, used to make a non-zero exit actionable.
fn last_line(output: &str) -> String {
    output
        .lines()
        .rev()
        .map(str::trim)
        .find(|line| !line.is_empty())
        .unwrap_or("no output")
        .chars()
        .take(160)
        .collect()
}

impl Provider for Sherpa {
    fn transcribe(&self, input: &Input<'_>) -> Result<String> {
        self.transcribe_with(input, super::Context::default())
    }

    fn transcribe_with(&self, input: &Input<'_>, context: super::Context<'_>) -> Result<String> {
        let settings = self.settings()?;
        let label = self.label();

        if !settings.bin_path.is_file() {
            return Err(Error::local_setup(format!(
                "{label}: binary not found at {} (set bin_path in ~/.config/ainput/{})",
                settings.bin_path.display(),
                self.config_name()
            )));
        }
        for name in self.required_files() {
            let path = settings.model_dir.join(name);
            if !path.is_file() {
                return Err(Error::model_not_found(format!(
                    "{label}: model file missing: {} (set model_dir in ~/.config/ainput/{})",
                    path.display(),
                    self.config_name()
                )));
            }
        }

        let deadline = context.deadline(Duration::from_secs(settings.timeout_secs));
        let mut command = Command::new(&settings.bin_path);
        command.args(self.arguments(&settings, input.wav_path));
        let output = crate::process::output_file(&mut command)
            .map_err(|err| Error::runtime(format!("{label}: cannot open output: {err}")))?;
        let status = crate::process::run(&mut command, label, Some(deadline), input.cancel)?;
        let combined = crate::process::read_output(output)
            .map_err(|err| Error::runtime(format!("{label}: cannot read output: {err}")))?;

        if !status.success() {
            return Err(Error::runtime(format!(
                "{label}: engine exited with {status}: {}",
                last_line(&combined)
            )));
        }

        match extract_text(&combined) {
            Some(text) => Ok(text),
            None => Err(Error::empty_result(format!(
                "{label}: engine produced no text: {}",
                last_line(&combined)
            ))),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extracts_last_json_text_line() {
        let output = "Recognizer created in 1.3 s\n\
                      { \"text\": \"first\", \"tokens\": [] }\n\
                      { \"text\": \"昨天是星期三\", \"tokens\": [] }\n";
        assert_eq!(extract_text(output).as_deref(), Some("昨天是星期三"));
    }

    #[test]
    fn ignores_noise_without_text() {
        let output = "Start to create recognizer\nRecognizer created in 1.0 s\n";
        assert!(extract_text(output).is_none());
    }

    #[test]
    fn empty_text_is_not_a_result() {
        assert!(extract_text("{ \"text\": \"  \" }").is_none());
    }

    #[test]
    fn arguments_match_the_cli_contract() {
        let sherpa = Sherpa::zipformer();
        let settings = Settings {
            model_dir: PathBuf::from("/models/z"),
            bin_path: PathBuf::from("/bin/sherpa-onnx"),
            timeout_secs: 120,
        };
        let args = sherpa.arguments(&settings, &PathBuf::from("/tmp/a.wav"));
        assert!(args.iter().any(|a| a.starts_with("--encoder=")));
        assert!(args.iter().any(|a| a == "--provider=cpu"));
        assert_eq!(args.last().unwrap(), "/tmp/a.wav");

        let fire_red = Sherpa::fire_red();
        let args = fire_red.arguments(&settings, &PathBuf::from("/tmp/a.wav"));
        assert!(args
            .iter()
            .any(|a| a.starts_with("--fire-red-asr-encoder=")));
    }
}
