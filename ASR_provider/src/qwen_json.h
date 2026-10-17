#pragma once

// Dependency-free JSON utilities for the Qwen (Alibaba DashScope) provider.
// Covers config reading (string / raw passthrough values) and schema-tolerant
// response parsing across DashScope ASR generations:
//   legacy (qwen3-asr-flash):  output.choices[0].message.content[0].text
//   current (qwen-audio-3.x / fun-asr-flash):  output.text | output.sentence.text
// Kept header-only inline so tests can exercise the parsing directly.

#include <cstdint>
#include <cstdio>
#include <string>

namespace ainput {

inline void appendUtf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

inline std::string jsonUnescape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
        char c = s[++i];
        switch (c) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
            if (i + 4 >= s.size()) { out += 'u'; break; }
            auto hex = [&](size_t off) -> uint32_t {
                uint32_t v = 0;
                for (size_t k = 0; k < 4; k++) {
                    char h = s[off + k];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (uint32_t)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (uint32_t)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (uint32_t)(h - 'A' + 10);
                    else return 0xFFFFFFFF;
                }
                return v;
            };
            uint32_t cp = hex(i + 1);
            i += 4;
            if (cp == 0xFFFFFFFF) { out += 'u'; break; }
            // Surrogate pair.
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() &&
                s[i + 1] == '\\' && s[i + 2] == 'u') {
                uint32_t lo = hex(i + 3);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            appendUtf8(out, cp);
            break;
        }
        default: out += c; break;
        }
    }
    return out;
}

inline std::string jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// Index just past the closing quote of the JSON string starting at pos
// (json[pos] must be '"'). Escape-aware.
inline size_t qjsonSkipString(const std::string &json, size_t pos) {
    pos++;
    while (pos < json.size()) {
        if (json[pos] == '\\') { pos += 2; continue; }
        if (json[pos] == '"') return pos + 1;
        pos++;
    }
    return std::string::npos;
}

// Value of <key> when it appears at the top level of the JSON object starting
// at objStart ('{'). Returns "" when absent, nested deeper, or non-string.
// Robust to field order and to same-named keys inside nested objects
// (e.g. output.sentence.words[].text must not shadow output.text).
inline std::string qjsonTopLevelString(const std::string &json, size_t objStart,
                                       const std::string &key) {
    if (objStart == std::string::npos || objStart >= json.size() ||
        json[objStart] != '{') {
        return "";
    }
    size_t pos = objStart + 1;
    int depth = 0;
    bool expectKey = true;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') {
            size_t end = qjsonSkipString(json, pos);
            if (end == std::string::npos) return "";
            std::string str = json.substr(pos + 1, end - pos - 2);
            pos = end;
            if (depth == 0) {
                if (expectKey && str == key) {
                    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) pos++;
                    if (pos < json.size() && json[pos] == '"') {
                        size_t vend = qjsonSkipString(json, pos);
                        if (vend == std::string::npos) return "";
                        return jsonUnescape(json.substr(pos + 1, vend - pos - 2));
                    }
                    return "";  // key found but value is not a string
                }
                expectKey = !expectKey;
            }
            continue;
        }
        if (c == '{' || c == '[') { depth++; pos++; continue; }
        if (c == '}' || c == ']') {
            if (depth == 0 && c == '}') return "";  // end of this object
            depth--;
            if (depth < 0) return "";
            pos++;
            continue;
        }
        if (c == ',') { if (depth == 0) expectKey = true; pos++; continue; }
        pos++;
    }
    return "";
}

// Start index of the nested object for <key> at the top level of the object
// starting at objStart, or npos.
inline size_t qjsonTopLevelObject(const std::string &json, size_t objStart,
                                  const std::string &key) {
    if (objStart == std::string::npos || objStart >= json.size() ||
        json[objStart] != '{') {
        return std::string::npos;
    }
    size_t pos = objStart + 1;
    int depth = 0;
    bool expectKey = true;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') {
            size_t end = qjsonSkipString(json, pos);
            if (end == std::string::npos) return std::string::npos;
            std::string str = json.substr(pos + 1, end - pos - 2);
            pos = end;
            if (depth == 0) {
                if (expectKey && str == key) {
                    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) pos++;
                    if (pos < json.size() && json[pos] == '{') return pos;
                    return std::string::npos;
                }
                expectKey = !expectKey;
            }
            continue;
        }
        if (c == '{' || c == '[') { depth++; pos++; continue; }
        if (c == '}' || c == ']') {
            if (depth == 0 && c == '}') return std::string::npos;
            depth--;
            if (depth < 0) return std::string::npos;
            pos++;
            continue;
        }
        if (c == ',') { if (depth == 0) expectKey = true; pos++; continue; }
        pos++;
    }
    return std::string::npos;
}

// Raw (still-encoded) JSON value for a key; used for config passthrough where
// the exact substring is spliced verbatim into a request (language_hints,
// vocabulary). Handles strings, objects, arrays (string-aware bracket
// matching) and scalar values.
inline std::string qjsonRawValue(const std::string &json, const std::string &key) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos += key.size() + 2;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size() || json[pos] != ':') return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size()) return "";
    char c = json[pos];
    size_t end = pos + 1;
    if (c == '"') {
        end = qjsonSkipString(json, pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }
    if (c == '{' || c == '[') {
        int depth = 0;
        size_t i = pos;  // count the opening bracket itself
        while (i < json.size()) {
            char d = json[i];
            if (d == '"') {
                size_t after = qjsonSkipString(json, i);
                if (after == std::string::npos) return "";
                i = after;
                continue;
            }
            if (d == '{' || d == '[') depth++;
            else if (d == '}' || d == ']') {
                depth--;
                if (depth == 0) return json.substr(pos, i - pos + 1);
            }
            i++;
        }
        return "";
    }
    while (end < json.size() && json[end] != ',' && json[end] != '}' &&
           json[end] != ']' && json[end] != ' ' && json[end] != '\n') {
        end++;
    }
    return json.substr(pos, end - pos);
}

// Unescaped string value for a key found anywhere (first occurrence).
inline std::string qjsonStringValue(const std::string &json, const std::string &key) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    size_t end = qjsonSkipString(json, pos);
    if (end == std::string::npos) return "";
    return jsonUnescape(json.substr(pos + 1, end - pos - 2));
}

// Transcription text from a DashScope ASR response, schema-tolerant.
inline std::string qwenExtractText(const std::string &body) {
    if (body.find("\"choices\"") != std::string::npos) {
        // Legacy: content is an array of objects: [{"text": "..."}].
        size_t content = body.find("\"content\"");
        if (content != std::string::npos) {
            size_t textKey = body.find("\"text\"", content);
            if (textKey != std::string::npos) {
                size_t pos = body.find(':', textKey + 6);
                if (pos != std::string::npos) {
                    pos++;
                    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
                    if (pos < body.size() && body[pos] == '"') {
                        size_t end = qjsonSkipString(body, pos);
                        if (end != std::string::npos)
                            return jsonUnescape(body.substr(pos + 1, end - pos - 2));
                    }
                }
            }
        }
        // fall through to generic paths before giving up
    }
    size_t outputKey = body.find("\"output\"");
    if (outputKey != std::string::npos) {
        size_t outputObj = body.find('{', outputKey);
        std::string text = qjsonTopLevelString(body, outputObj, "text");
        if (!text.empty()) return text;
        size_t sentenceObj = qjsonTopLevelObject(body, outputObj, "sentence");
        if (sentenceObj != std::string::npos) {
            text = qjsonTopLevelString(body, sentenceObj, "text");
            if (!text.empty()) return text;
        }
    }
    size_t root = body.find('{');
    if (root != std::string::npos) {
        std::string text = qjsonTopLevelString(body, root, "text");
        if (!text.empty()) return text;
    }
    return "";
}

// Best-effort detail from a DashScope error body, e.g.
// {"code":"InvalidApiKey","message":"Invalid API-key","request_id":"..."}
inline std::string qwenExtractErrorDetail(const std::string &body) {
    if (body.empty()) return "";
    size_t root = body.find('{');
    if (root == std::string::npos) return "";
    std::string code = qjsonTopLevelString(body, root, "code");
    std::string message = qjsonTopLevelString(body, root, "message");
    if (!code.empty() && !message.empty()) return code + ": " + message;
    if (!message.empty()) return message;
    return code;
}

// Removes a top-level property (key and value plus the appropriate comma)
// from the JSON object text starting at objStart, leaving nested same-name
// keys untouched. Returns the rewritten JSON text, or "" when the key is
// absent or the removal cannot be done safely (caller keeps the original).
// Used to strip "api_key" from a vendor config after importing the secret
// into the encrypted store.
inline std::string qjsonRemoveTopLevelProperty(const std::string &json,
                                               const std::string &key) {
    size_t objStart = json.find('{');
    if (objStart == std::string::npos) return "";
    size_t pos = objStart + 1;
    int depth = 0;
    bool expectKey = true;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '"') {
            size_t keyStart = pos;
            size_t end = qjsonSkipString(json, pos);
            if (end == std::string::npos) return "";
            std::string str = json.substr(keyStart + 1, end - keyStart - 2);
            pos = end;
            if (depth == 0 && expectKey && str == key) {
                while (pos < json.size() &&
                       (json[pos] == ' ' || json[pos] == '\t' ||
                        json[pos] == '\n' || json[pos] == '\r' || json[pos] == ':')) {
                    pos++;
                }
                size_t valueEnd = pos;  // one past the value
                if (pos >= json.size()) return "";
                if (json[pos] == '"') {
                    valueEnd = qjsonSkipString(json, pos);
                } else if (json[pos] == '{' || json[pos] == '[') {
                    int vdepth = 0;
                    while (valueEnd < json.size()) {
                        char d = json[valueEnd];
                        if (d == '"') {
                            valueEnd = qjsonSkipString(json, valueEnd);
                            if (valueEnd == std::string::npos) return "";
                            continue;
                        }
                        if (d == '{' || d == '[') vdepth++;
                        else if (d == '}' || d == ']') {
                            vdepth--;
                            if (vdepth == 0) { valueEnd++; break; }
                        }
                        valueEnd++;
                    }
                } else {
                    while (valueEnd < json.size() && json[valueEnd] != ',' &&
                           json[valueEnd] != '}' && json[valueEnd] != ']' &&
                           json[valueEnd] != ' ' && json[valueEnd] != '\n' &&
                           json[valueEnd] != '\r' && json[valueEnd] != '\t') {
                        valueEnd++;
                    }
                }
                if (valueEnd == std::string::npos || valueEnd >= json.size())
                    return "";
                // Extend past a trailing comma when the property is first.
                size_t after = valueEnd;
                while (after < json.size() && (json[after] == ' ' || json[after] == '\t' ||
                                               json[after] == '\n' || json[after] == '\r')) {
                    after++;
                }
                if (json[after] == ',') after++;
                // Include a preceding comma when the property is not first.
                size_t removalStart = keyStart;
                size_t back = keyStart;
                do {
                    back--;
                } while (back > objStart && (json[back] == ' ' || json[back] == '\t' ||
                                             json[back] == '\n' || json[back] == '\r'));
                if (back > objStart && json[back] == ',') {
                    // Property is not first: drop the preceding comma too.
                    removalStart = back;
                    after = valueEnd;  // keep the following comma
                } else {
                    // First property: also swallow its indentation.
                    while (removalStart > objStart + 1 &&
                           (json[removalStart - 1] == ' ' || json[removalStart - 1] == '\t' ||
                            json[removalStart - 1] == '\n' || json[removalStart - 1] == '\r')) {
                        removalStart--;
                    }
                }
                return json.substr(0, removalStart) + json.substr(after);
            }
            if (depth == 0) expectKey = !expectKey;
            continue;
        }
        if (c == '{' || c == '[') { depth++; pos++; continue; }
        if (c == '}' || c == ']') {
            if (depth == 0 && c == '}') return "";  // end, key not found
            depth--;
            if (depth < 0) return "";
            pos++;
            continue;
        }
        if (c == ',') { if (depth == 0) expectKey = true; pos++; continue; }
        pos++;
    }
    return "";
}

// qwen3-asr-flash / qwen2-audio era models use the old wire format// ({"audio": ...} + asr_options). Everything newer (qwen-audio-3.x,
// fun-asr-flash, and by default unknown future names) uses input_audio.
inline bool qwenIsLegacyModel(const std::string &model) {
    return model.rfind("qwen3-asr-flash", 0) == 0 ||
           model.rfind("qwen2-audio", 0) == 0 ||
           model.rfind("qwen2-asr", 0) == 0;
}

inline bool qwenUsesLegacyRequest(const std::string &model) {
    return qwenIsLegacyModel(model);
}

// Strip // line comments: comment bytes become spaces (newlines kept), so the
// result has the exact same length and byte offsets as the original — syntax
// error offsets still point at the user's file. '/' inside a string value
// (e.g. "https://dashscope.aliyuncs.com") is never treated as a comment.
inline std::string qjsonStripComments(const std::string &json) {
    std::string out = json;
    bool inString = false;
    for (size_t i = 0; i < out.size(); i++) {
        char c = out[i];
        if (inString) {
            if (c == '\\') { i++; continue; }
            if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') {
                out[i] = ' ';
                i++;
            }
            // keep the newline itself
        }
    }
    return out;
}

// Locate a top-level property's value span [valStart, valEnd). Returns false
// when the key is absent. String-aware; only looks at the root object.
inline bool qjsonFindTopLevelProperty(const std::string &json,
                                      const std::string &key,
                                      size_t &valStart, size_t &valEnd) {
    size_t obj = json.find('{');
    if (obj == std::string::npos) return false;
    size_t pos = obj + 1;
    while (pos < json.size()) {
        // next key
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                     json[pos] == '\n' || json[pos] == '\r' ||
                                     json[pos] == ',')) {
            pos++;
        }
        if (pos >= json.size() || json[pos] == '}') return false;
        if (json[pos] != '"') return false;
        size_t keyEnd = qjsonSkipString(json, pos);
        if (keyEnd == std::string::npos) return false;
        std::string name = json.substr(pos + 1, keyEnd - pos - 2);
        pos = keyEnd;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                     json[pos] == '\n' || json[pos] == '\r')) {
            pos++;
        }
        if (pos >= json.size() || json[pos] != ':') return false;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                     json[pos] == '\n' || json[pos] == '\r')) {
            pos++;
        }
        if (pos >= json.size()) return false;
        valStart = pos;
        // value end
        char c = json[pos];
        if (c == '"') {
            size_t end = qjsonSkipString(json, pos);
            if (end == std::string::npos) return false;
            valEnd = end;
        } else if (c == '{' || c == '[') {
            int depth = 0;
            bool str = false;
            size_t end = pos;
            for (; end < json.size(); end++) {
                char d = json[end];
                if (str) {
                    if (d == '\\') { end++; continue; }
                    if (d == '"') str = false;
                    continue;
                }
                if (d == '"') { str = true; continue; }
                if (d == '{' || d == '[') depth++;
                else if (d == '}' || d == ']') {
                    depth--;
                    if (depth == 0) { end++; break; }
                }
            }
            if (depth != 0) return false;
            valEnd = end;
        } else {
            size_t end = pos;
            while (end < json.size() && json[end] != ',' && json[end] != '}' &&
                   json[end] != ']' && json[end] != ' ' && json[end] != '\n' &&
                   json[end] != '\t' && json[end] != '\r') {
                end++;
            }
            valEnd = end;
        }
        if (name == key) return true;
        pos = valEnd;
    }
    return false;
}

// Replace or insert a top-level string property; returns the new JSON text.
// Used to persist the active provider selection in config.json.
inline std::string qjsonSetTopLevelString(const std::string &json,
                                          const std::string &key,
                                          const std::string &value) {
    std::string quoted = "\"" + jsonEscape(value) + "\"";
    size_t valStart = 0, valEnd = 0;
    if (qjsonFindTopLevelProperty(json, key, valStart, valEnd)) {
        return json.substr(0, valStart) + quoted + json.substr(valEnd);
    }
    size_t obj = json.find('{');
    if (obj == std::string::npos) {
        return "{\"" + key + "\":" + quoted + "}";
    }
    size_t pos = obj + 1;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) {
        pos++;
    }
    bool empty = pos < json.size() && json[pos] == '}';
    std::string prop = "\"" + key + "\":" + quoted;
    if (empty) {
        return json.substr(0, obj + 1) + prop + json.substr(obj + 1);
    }
    return json.substr(0, obj + 1) + prop + "," + json.substr(obj + 1);
}

// Replace or insert a string property inside a top-level object section
// (e.g. config.json [audio].denoise). The section is created when absent.
// Like qjsonSetTopLevelString this edits raw text so comments survive.
inline std::string qjsonSetSectionString(const std::string &json,
                                         const std::string &section,
                                         const std::string &key,
                                         const std::string &value) {
    size_t vs = 0, ve = 0;
    if (qjsonFindTopLevelProperty(json, section, vs, ve)) {
        std::string inner = json.substr(vs, ve - vs);
        std::string updated = qjsonSetTopLevelString(inner, key, value);
        return json.substr(0, vs) + updated + json.substr(ve);
    }
    std::string prop = "\"" + section + "\":{\"" + key + "\":" +
                       "\"" + jsonEscape(value) + "\"}";
    size_t obj = json.find('{');
    if (obj == std::string::npos) {
        return "{" + prop + "}";
    }
    size_t pos = obj + 1;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) {
        pos++;
    }
    bool empty = pos < json.size() && json[pos] == '}';
    return json.substr(0, obj + 1) + prop + (empty ? "" : ",") + json.substr(obj + 1);
}

} // namespace ainput
