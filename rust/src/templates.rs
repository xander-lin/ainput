//! Commented templates for the provider config files.
//!
//! Every template lists **all** fields with their default value, so opening a
//! file shows the complete set of knobs instead of hiding the ones a user did
//! not think to add. After `//` comments are stripped each template must be
//! valid JSON and pass `config::validate` (enforced by tests).
//!
//! English only: the README has a Chinese translation, generated files do not.

pub fn qwen() -> String {
    format!(
        r#"{{
    // Alibaba DashScope (Model Studio) API key.
    // https://bailian.console.aliyun.com/ -> API-KEY
    // Re-read before every recognition. chmod 600 this file.
    "api_key": "{}",

    // ASR model id. "qwen-audio-3.1-asr-flash" is the current default.
    // "qwen3-asr-flash" still works and uses an older wire format; the format
    // is chosen from the model name, not from a separate setting.
    "model": "qwen-audio-3.1-asr-flash",

    // Full request URL. Change this when moving to a workspace-specific
    // {{workspace}}.cn-beijing.maas.aliyuncs.com endpoint.
    "endpoint": "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation",

    // Language hints, e.g. ["zh", "en"]. At most 4 entries.
    // Leave empty to let the service detect the language.
    "language_hints": [],

    // Instant hot words: {{"term": weight}}. Weight is 1-5, or 50 for a
    // super hot word. Leave empty to disable.
    "vocabulary": {{}},

    // Precompiled hot-word list id from the console. Empty = unused.
    "vocabulary_id": "",

    // qwen-audio-3.1-asr-flash only: keep dialect wording instead of
    // rendering everything as standard Mandarin.
    "keep_dialect": false,

    // qwen-audio-3.1-asr-flash only: label speakers in the result.
    "speaker_diarization": false,

    // Whole-request timeout in seconds.
    "timeout_sec": 60
}}
"#,
        crate::config::PLACEHOLDER_KEY
    )
}

pub fn doubao() -> String {
    format!(
        r#"{{
    // Volcengine (ByteDance) app key. Re-read before every recognition.
    // chmod 600 this file.
    "api_key": "{}",

    // Resource id that selects the ASR product tier.
    // volc.seedasr.auc is the current-generation one.
    "resource_id": "volc.seedasr.auc",

    // Sent as request.model_name in the submit call.
    "model_name": "bigmodel",

    // Inverse text normalization: digits, dates and units in the result.
    "enable_itn": true,

    // Punctuation restoration in the result.
    "enable_punc": true,

    // Whole-recognition budget in seconds. Submit and result polling share
    // this single deadline.
    "timeout_sec": 90
}}
"#,
        crate::config::PLACEHOLDER_KEY
    )
}

pub fn zipformer() -> String {
    r#"{
    // Directory containing the model produced by the
    // sherpa-onnx-streaming-zipformer-bilingual-zh-en archive:
    //   encoder-epoch-99-avg-1.onnx
    //   decoder-epoch-99-avg-1.onnx
    //   joiner-epoch-99-avg-1.onnx
    //   tokens.txt
    // See the README for the download commands.
    "model_dir": "~/.local/share/ainput/models/zipformer-zh-en",

    // Absolute path or ~-prefixed path to the sherpa-onnx binary.
    "bin_path": "~/.local/share/ainput/sherpa-onnx/bin/sherpa-onnx",

    // Wall-clock timeout for one recognition, in seconds.
    "timeout_sec": 120
}
"#
    .to_string()
}

pub fn fire_red() -> String {
    r#"{
    // Directory containing the model produced by the
    // sherpa-onnx-fire-red-asr2-zh_en-int8 archive:
    //   encoder.int8.onnx
    //   decoder.int8.onnx
    //   tokens.txt
    // See the README for the download commands.
    "model_dir": "~/.local/share/ainput/models/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26",

    // Path to the sherpa-onnx-offline binary.
    "bin_path": "~/.local/share/ainput/sherpa-onnx/bin/sherpa-onnx-offline",

    // Wall-clock timeout for one recognition, in seconds.
    "timeout_sec": 120
}
"#
    .to_string()
}

/// Template for a given provider id, used by the `ainput_config_template` FFI
/// call so the shell can regenerate a file on demand.
pub fn by_id(which: i32) -> Option<String> {
    match which {
        1 => Some(qwen()),
        2 => Some(doubao()),
        3 => Some(zipformer()),
        4 => Some(fire_red()),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_valid(name: &str, text: String) {
        let stripped = crate::config::strip_comments(&text);
        let value: serde_json::Value = serde_json::from_str(&stripped)
            .unwrap_or_else(|e| panic!("{name} template is not valid JSON: {e}\n{stripped}"));
        let fields = match name {
            "qwen.json" => crate::config::qwen_fields(),
            "doubao.json" => crate::config::doubao_fields(),
            _ => crate::config::local_fields(),
        };
        let issues = crate::config::validate(&value, &fields, "");
        assert!(
            issues.is_empty(),
            "{name} template has schema issues: {issues:?}"
        );
    }

    #[test]
    fn templates_are_valid() {
        assert_valid("qwen.json", qwen());
        assert_valid("doubao.json", doubao());
        assert_valid("zipformer.json", zipformer());
        assert_valid("fire_red.json", fire_red());
    }

    #[test]
    fn templates_list_every_field() {
        // A template must mention each schema field, otherwise "all options
        // are visible in the file" is not true.
        for (name, text, fields) in [
            ("qwen", qwen(), crate::config::qwen_fields()),
            ("doubao", doubao(), crate::config::doubao_fields()),
            ("zipformer", zipformer(), crate::config::local_fields()),
        ] {
            for field in fields {
                assert!(
                    text.contains(&format!("\"{}\"", field.name)),
                    "{name} template is missing field \"{}\"",
                    field.name
                );
            }
        }
    }
}
