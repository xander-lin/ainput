#pragma once

// Schema-driven validation for the plain-JSON config files. Every config
// reader validates its file and reports issues (unknown fields get a
// "did you mean" suggestion, type mismatches and JSON syntax errors are
// pinpointed with a byte offset). Fatal issues (syntax errors) also reach
// the input panel as a ConfigInvalid recognition error; warnings are logged
// and appended to the provider switch notification.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "qwen_json.h"

namespace ainput {

enum class ConfigFieldType { String, Int, Double, Bool, StringArray, Object };

struct ConfigField {
    std::string name;
    ConfigFieldType type;
    bool required = false;
};

struct ConfigIssue {
    bool fatal = false;      // syntax error: config cannot be trusted
    std::string message;     // human-readable description
};

namespace config_schema_detail {

inline const char *typeName(ConfigFieldType t) {
    switch (t) {
    case ConfigFieldType::String: return "a string";
    case ConfigFieldType::Int: return "an integer";
    case ConfigFieldType::Double: return "a number";
    case ConfigFieldType::Bool: return "true/false";
    case ConfigFieldType::StringArray: return "an array of strings";
    case ConfigFieldType::Object: return "an object";
    }
    return "a value";
}

inline bool looksLikeInt(const std::string &raw) {
    size_t i = 0;
    if (!raw.empty() && (raw[0] == '-')) i = 1;
    if (i >= raw.size()) return false;
    for (; i < raw.size(); i++) {
        if (raw[i] < '0' || raw[i] > '9') return false;
    }
    return true;
}

inline bool looksLikeDouble(const std::string &raw) {
    size_t i = 0;
    if (!raw.empty() && raw[0] == '-') i = 1;
    bool digits = false, dot = false;
    for (; i < raw.size(); i++) {
        if (raw[i] >= '0' && raw[i] <= '9') { digits = true; continue; }
        if (raw[i] == '.' && !dot) { dot = true; continue; }
        return false;
    }
    return digits;
}

inline bool looksLikeStringArray(const std::string &raw) {
    if (raw.size() < 2 || raw.front() != '[') return false;
    // Walk elements: each must be a string (empty array is fine).
    size_t pos = 1;
    auto skipWs = [&](size_t &p) {
        while (p < raw.size() && (raw[p] == ' ' || raw[p] == '\t' ||
                                  raw[p] == '\n' || raw[p] == '\r')) {
            p++;
        }
    };
    skipWs(pos);
    if (pos < raw.size() && raw[pos] == ']') return true;
    while (pos < raw.size()) {
        if (raw[pos] != '"') return false;
        size_t end = qjsonSkipString(raw, pos);
        if (end == std::string::npos) return false;
        pos = end;
        skipWs(pos);
        if (pos < raw.size() && raw[pos] == ',') { pos++; skipWs(pos); continue; }
        break;
    }
    return pos < raw.size() && raw[pos] == ']';
}

inline bool matchesType(ConfigFieldType type, const std::string &raw) {
    switch (type) {
    case ConfigFieldType::String: return !raw.empty() && raw.front() == '"';
    case ConfigFieldType::Int: return looksLikeInt(raw);
    case ConfigFieldType::Double: return looksLikeDouble(raw) || looksLikeInt(raw);
    case ConfigFieldType::Bool: return raw == "true" || raw == "false";
    case ConfigFieldType::StringArray: return looksLikeStringArray(raw);
    case ConfigFieldType::Object: return !raw.empty() && raw.front() == '{';
    }
    return false;
}

// Small recursive-descent JSON syntax validator. Returns "" when the text is
// a well-formed JSON document, else a description with the first bad offset.
struct SyntaxChecker {
    const std::string &j;
    size_t pos = 0;
    bool ok = true;
    size_t errPos = 0;
    std::string errMsg;

    SyntaxChecker(const std::string &json) : j(json) {}

    void fail(const std::string &msg) {
        if (ok) {
            ok = false;
            errPos = pos;
            errMsg = msg;
        }
    }

    void ws() {
        while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t' ||
                                  j[pos] == '\n' || j[pos] == '\r')) {
            pos++;
        }
    }

    bool literal(const char *lit) {
        size_t len = strlen(lit);
        if (j.compare(pos, len, lit) == 0) { pos += len; return true; }
        return false;
    }

    void skipString() {
        pos++;  // opening quote (caller checked)
        while (pos < j.size()) {
            if (j[pos] == '\\') { pos += 2; continue; }
            if (j[pos] == '"') { pos++; return; }
            pos++;
        }
        fail("unterminated string");
    }

    void skipNumber() {
        if (pos < j.size() && j[pos] == '-') pos++;
        size_t start = pos;
        while (pos < j.size() && ((j[pos] >= '0' && j[pos] <= '9') ||
                                  j[pos] == '.' || j[pos] == 'e' ||
                                  j[pos] == 'E' || j[pos] == '+' || j[pos] == '-')) {
            pos++;
        }
        if (pos == start) fail("expected a number");
    }

    void value() {
        if (!ok) return;
        ws();
        if (pos >= j.size()) { fail("unexpected end of file"); return; }
        char c = j[pos];
        if (c == '"') { skipString(); return; }
        if (c == '{') { object(); return; }
        if (c == '[') { array(); return; }
        if (c == '-' || (c >= '0' && c <= '9')) { skipNumber(); return; }
        if (literal("true") || literal("false") || literal("null")) return;
        fail(std::string("unexpected character '") + c + "'");
    }

    void object() {
        pos++;  // '{'
        ws();
        if (pos < j.size() && j[pos] == '}') { pos++; return; }
        while (ok) {
            ws();
            if (pos >= j.size()) { fail("unterminated object"); return; }
            if (j[pos] != '"') { fail("expected a field name"); return; }
            skipString();
            if (!ok) return;
            ws();
            if (pos >= j.size() || j[pos] != ':') { fail("expected ':' after field name"); return; }
            pos++;
            value();
            if (!ok) return;
            ws();
            if (pos < j.size() && j[pos] == ',') { pos++; continue; }
            if (pos < j.size() && j[pos] == '}') { pos++; return; }
            fail("expected ',' or '}'");
            return;
        }
    }

    void array() {
        pos++;  // '['
        ws();
        if (pos < j.size() && j[pos] == ']') { pos++; return; }
        while (ok) {
            value();
            if (!ok) return;
            ws();
            if (pos < j.size() && j[pos] == ',') { pos++; continue; }
            if (pos < j.size() && j[pos] == ']') { pos++; return; }
            fail("expected ',' or ']'");
            return;
        }
    }

    std::string run() {
        ws();
        if (pos >= j.size()) return "empty file";
        if (j[pos] != '{') return "config must be a JSON object";
        value();
        if (!ok) return errMsg + " (at offset " + std::to_string(errPos) + ")";
        ws();
        if (pos < j.size()) return "unexpected trailing content (at offset " +
                                  std::to_string(pos) + ")";
        return "";
    }
};

inline size_t editDistance(const std::string &a, const std::string &b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); j++) {
            size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min(std::min(cur[j - 1] + 1, prev[j] + 1), prev[j - 1] + cost);
        }
        prev = cur;
    }
    return prev[b.size()];
}

} // namespace config_schema_detail

// Returns "" when the text is a well-formed JSON object, else a description
// of the first syntax problem (with byte offset).
inline std::string qjsonSyntaxError(const std::string &json) {
    if (json.empty()) return "empty file";
    return config_schema_detail::SyntaxChecker(json).run();
}

// Top-level key names of a JSON object, in order of appearance.
inline std::vector<std::string> qjsonTopLevelKeys(const std::string &json) {
    std::vector<std::string> keys;
    size_t obj = json.find('{');
    if (obj == std::string::npos) return keys;
    size_t pos = obj + 1;
    int depth = 0;
    bool expectKey = true;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') {
            size_t end = qjsonSkipString(json, pos);
            if (end == std::string::npos) return keys;
            std::string str = json.substr(pos + 1, end - pos - 2);
            pos = end;
            if (depth == 0 && expectKey) {
                keys.push_back(str);
                expectKey = false;
            }
            continue;
        }
        if (c == '{' || c == '[') { depth++; pos++; continue; }
        if (c == '}' || c == ']') {
            if (depth == 0 && c == '}') return keys;
            depth--;
            pos++;
            continue;
        }
        if (c == ',') { if (depth == 0) expectKey = true; pos++; continue; }
        pos++;
    }
    return keys;
}

// Nearest schema field for a typo suggestion ("" when nothing is close).
// Accepts edit distance <= 2, or a larger distance when the strings share a
// long prefix (catches "modelname" -> "model", "timeout" -> "timeout_sec").
inline std::string nearestConfigField(const std::string &name,
                                      const std::vector<ConfigField> &schema) {
    auto commonPrefix = [](const std::string &a, const std::string &b) {
        size_t n = std::min(a.size(), b.size()), i = 0;
        while (i < n && a[i] == b[i]) i++;
        return i;
    };
    std::string best;
    size_t bestDist = SIZE_MAX;
    for (const auto &f : schema) {
        size_t d = config_schema_detail::editDistance(name, f.name);
        size_t longer = std::max(name.size(), f.name.size());
        bool close = d <= 2 ||
                     (commonPrefix(name, f.name) >= 3 && d <= longer / 2);
        if (close && d < bestDist) {
            bestDist = d;
            best = f.name;
        }
    }
    return best;
}

// Validates a config file's text against a schema. A syntax error produces
// a single fatal issue; otherwise unknown keys (with suggestions), type
// mismatches and missing required fields are collected.
inline std::vector<ConfigIssue> validateConfigJson(
    const std::string &json, const std::vector<ConfigField> &schema) {
    std::vector<ConfigIssue> issues;
    std::string syntax = qjsonSyntaxError(json);
    if (!syntax.empty()) {
        issues.push_back({true, syntax});
        return issues;
    }
    if (json.empty()) return issues;  // missing file is not a schema issue
    for (const auto &key : qjsonTopLevelKeys(json)) {
        const ConfigField *field = nullptr;
        for (const auto &f : schema) {
            if (f.name == key) { field = &f; break; }
        }
        if (!field) {
            std::string near = nearestConfigField(key, schema);
            issues.push_back({false, "unknown field \"" + key + "\"" +
                                         (near.empty() ? "" : " (did you mean \"" + near + "\"?)")});
            continue;
        }
        std::string raw = qjsonRawValue(json, key);
        if (!config_schema_detail::matchesType(field->type, raw)) {
            issues.push_back({false, "field \"" + key + "\" expects " +
                                         config_schema_detail::typeName(field->type) +
                                         " but got " +
                                         (raw.empty() ? "nothing" : raw.substr(0, 24))});
        }
    }
    for (const auto &f : schema) {
        if (!f.required) continue;
        if (qjsonRawValue(json, f.name).empty()) {
            issues.push_back({false, "missing required field \"" + f.name + "\""});
        }
    }
    return issues;
}

// Shared schemas for the config files. Layout (since 2026-10):
//   ~/.config/ainput/config.json  — provider selection + "ui" + "audio"
//   ~/.config/ainput/<provider>.json — qwen / doubao / zipformer / fire_red
inline std::vector<ConfigField> configFileSchema() {
    return {
        {"provider", ConfigFieldType::String, true},
        {"ui", ConfigFieldType::Object, false},
        {"audio", ConfigFieldType::Object, false},
    };
}

inline std::vector<ConfigField> uiConfigSchema() {
    return {
        {"activation_msec", ConfigFieldType::Int, false},
        {"notification_timeout", ConfigFieldType::Int, false},
        {"debounce_count", ConfigFieldType::Int, false},
    };
}

inline std::vector<ConfigField> audioConfigSchema() {
    return {
        {"denoise", ConfigFieldType::String, false},
        {"lufs_target", ConfigFieldType::Double, false},
        {"speex_level", ConfigFieldType::Int, false},
        {"crest_threshold", ConfigFieldType::Double, false},
    };
}

inline std::vector<ConfigField> qwenConfigSchema() {
    return {
        {"api_key", ConfigFieldType::String, true},
        {"model", ConfigFieldType::String, false},
        {"endpoint", ConfigFieldType::String, false},
        {"language_hints", ConfigFieldType::StringArray, false},
        {"vocabulary", ConfigFieldType::Object, false},
        {"vocabulary_id", ConfigFieldType::String, false},
        {"keep_dialect", ConfigFieldType::Bool, false},
        {"speaker_diarization", ConfigFieldType::Bool, false},
        {"timeout_sec", ConfigFieldType::Int, false},
    };
}

inline std::vector<ConfigField> doubaoConfigSchema() {
    return {
        {"api_key", ConfigFieldType::String, true},
        {"resource_id", ConfigFieldType::String, true},
        {"model_name", ConfigFieldType::String, false},
        {"enable_itn", ConfigFieldType::Bool, false},
        {"enable_punc", ConfigFieldType::Bool, false},
        {"timeout_sec", ConfigFieldType::Int, false},
    };
}

inline std::vector<ConfigField> localModelConfigSchema() {
    return {
        {"model_dir", ConfigFieldType::String, false},
        {"timeout_sec", ConfigFieldType::Int, false},
        {"bin_path", ConfigFieldType::String, false},
    };
}

// Validate config.json as a whole: top-level keys plus the "ui" and "audio"
// sections. Section issues are prefixed so the file and section are obvious.
inline std::vector<ConfigIssue> validateConfigFileJson(const std::string &json) {
    auto issues = validateConfigJson(json, configFileSchema());
    if (!issues.empty() && issues.front().fatal) return issues;
    struct Section {
        const char *name;
        std::vector<ConfigField> schema;
    };
    for (const Section &sec : {
             Section{"audio", audioConfigSchema()},
             Section{"ui", uiConfigSchema()},
         }) {
        std::string raw = qjsonRawValue(json, sec.name);
        if (raw.empty()) continue;
        for (auto &issue : validateConfigJson(raw, sec.schema)) {
            issue.message = std::string("[") + sec.name + "] " + issue.message;
            issues.push_back(issue);
        }
    }
    return issues;
}

// Report issues to stderr (always) and the diagnostic log. Called by every
// config reader; cheap because config files are tiny.
inline void reportConfigIssues(const std::string &owner,
                               const std::string &file,
                               const std::vector<ConfigIssue> &issues) {
    for (const auto &issue : issues) {
        fprintf(stderr, "Ainput %s: %s: %s%s\n", owner.c_str(), file.c_str(),
                issue.fatal ? "JSON syntax error: " : "config warning: ",
                issue.message.c_str());
    }
}

// First issue message ("" when none) — appended to provider display names
// so the switch notification surfaces config problems.
inline std::string firstConfigIssue(const std::vector<ConfigIssue> &issues) {
    for (const auto &issue : issues) return issue.message;
    return "";
}

} // namespace ainput
