#include "qwen_json.h"
#include "config_schema.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
    if (!ok) {
        failures++;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void checkEq(const std::string &got, const std::string &want,
             const std::string &what) {
    if (got != want) {
        failures++;
        std::cerr << "FAIL: " << what << "\n  got:  \"" << got << "\"\n"
                  << "  want: \"" << want << "\"\n";
    }
}

// Current generation response (qwen-audio-3.x / fun-asr-flash), words listed
// after the sentence text.
const char *kNewGenWordsAfterText =
    "{\"output\":{\"sentence\":{\"begin_time\":760,\"channel_id\":0,"
    "\"end_time\":3800,\"sentence_end\":true,\"sentence_id\":1,"
    "\"text\":\"Hello World，这里是阿里巴巴语音实验室。\","
    "\"words\":[{\"begin_time\":760,\"end_time\":1040,\"fixed\":true,"
    "\"punctuation\":\"\",\"text\":\"Hello\"},{\"begin_time\":1040,"
    "\"end_time\":1240,\"fixed\":true,\"punctuation\":\"，\","
    "\"text\":\" World\"}]},"
    "\"text\":\"Hello World，这里是阿里巴巴语音实验室。\"},"
    "\"usage\":{\"duration\":4},"
    "\"request_id\":\"40e0734d-096f-9ae3-86c1-a8c013287561\"}";

// Same schema with words before the sentence text (field order not
// guaranteed).
const char *kNewGenWordsBeforeText =
    "{\"output\":{\"sentence\":{\"words\":[{\"text\":\"你好\"}],"
    "\"text\":\"你好世界\"},\"text\":\"你好世界\"},\"request_id\":\"r\"}";

// Speaker diarization response: output.sentences array instead of sentence;
// top-level output.text must win over nested sentence texts.
const char *kNewGenDiarization =
    "{\"output\":{\"sentences\":[{\"speaker_id\":0,\"text\":\"第一句\"},"
    "{\"speaker_id\":1,\"text\":\"第二句\"}],"
    "\"text\":\"第一句第二句\"},\"request_id\":\"r2\"}";

// Legacy qwen3-asr-flash response.
const char *kLegacyResponse =
    "{\"output\":{\"choices\":[{\"finish_reason\":\"stop\",\"message\":"
    "{\"content\":[{\"text\":\"识别出的文本内容\"}],\"role\":\"assistant\"}}]},"
    "\"request_id\":\"legacy\",\"usage\":{\"input_tokens\":10}}";

void testTextExtraction() {
    checkEq(ainput::qwenExtractText(kNewGenWordsAfterText),
            "Hello World，这里是阿里巴巴语音实验室。",
            "new-gen text with words after text");
    checkEq(ainput::qwenExtractText(kNewGenWordsBeforeText), "你好世界",
            "new-gen text with words before text");
    checkEq(ainput::qwenExtractText(kNewGenDiarization), "第一句第二句",
            "new-gen diarization output.text wins");
    checkEq(ainput::qwenExtractText(kLegacyResponse), "识别出的文本内容",
            "legacy choices/content text");
    checkEq(ainput::qwenExtractText("{\"output\":{\"text\":\"普通话\"}}"),
            "普通话", "minimal new-gen response");
    checkEq(ainput::qwenExtractText(""), "", "empty body yields no text");
    checkEq(ainput::qwenExtractText("{\"output\":{}}"), "",
            "missing text yields empty");
}

void testEscapes() {
    // \u4f60\u597d = 你好
    checkEq(ainput::jsonUnescape("\\u4f60\\u597d"), "你好",
            "unicode escape");
    checkEq(ainput::jsonUnescape("a\\\"b"), "a\"b", "quote escape");
    checkEq(ainput::jsonUnescape("a\\\\b"), "a\\b", "backslash escape");
    checkEq(ainput::jsonUnescape("a\\nb"), "a\nb", "newline escape");
    // Surrogate pair U+4E2D U+D860? Use a valid pair: U+20BB7 (𠮷).
    checkEq(ainput::jsonUnescape("\\ud842\\udfb7"), "\xF0\xA0\xAE\xB7",
            "surrogate pair escape");
    checkEq(ainput::jsonEscape("a\"b\\c\n"), "a\\\"b\\\\c\\n",
            "jsonEscape round trip characters");
    checkEq(ainput::jsonUnescape(ainput::jsonEscape("引号\"反斜杠\\")),
            "引号\"反斜杠\\", "escape round trip");
}

void testTopLevelString() {
    // Nested same-name key must not shadow the outer one.
    std::string json = "{\"text\":\"outer\",\"words\":[{\"text\":\"inner\"}]}";
    size_t obj = json.find('{');
    checkEq(ainput::qjsonTopLevelString(json, obj, "text"), "outer",
            "top-level text beats nested word text");
    checkEq(ainput::qjsonTopLevelString(json, obj, "missing"), "",
            "missing key yields empty");
    // Value of the nested object.
    size_t words = ainput::qjsonTopLevelObject(json, obj, "words");
    check(words == std::string::npos, "words is an array, not an object");
    std::string json2 = "{\"sentence\":{\"text\":\"句\"},\"text\":\"总\"}";
    size_t obj2 = json2.find('{');
    checkEq(ainput::qjsonTopLevelString(json2, obj2, "text"), "总",
            "outer text preferred over sentence text");
    size_t sentence = ainput::qjsonTopLevelObject(json2, obj2, "sentence");
    check(sentence != std::string::npos, "sentence object found");
    checkEq(ainput::qjsonTopLevelString(json2, sentence, "text"), "句",
            "sentence text extracted from nested object");
}

void testRawValue() {
    std::string cfg =
        "{\"api_key\":\"sk-1\",\"language_hints\":[\"zh\",\"en\"],"
        "\"vocabulary\":{\"Ainput\":5,\"fcitx\":3},"
        "\"keep_dialect\":true,\"endpoint\":\"https://x/y\","
        "\"note\":\"contains } brace\"}";
    checkEq(ainput::qjsonRawValue(cfg, "language_hints"), "[\"zh\",\"en\"]",
            "raw array value");
    checkEq(ainput::qjsonRawValue(cfg, "vocabulary"),
            "{\"Ainput\":5,\"fcitx\":3}", "raw object value");
    checkEq(ainput::qjsonRawValue(cfg, "keep_dialect"), "true",
            "raw boolean value");
    checkEq(ainput::qjsonRawValue(cfg, "endpoint"), "\"https://x/y\"",
            "raw string keeps quotes");
    checkEq(ainput::qjsonRawValue(cfg, "api_key"), "\"sk-1\"",
            "raw api key");
    checkEq(ainput::qjsonRawValue(cfg, "note"), "\"contains } brace\"",
            "brace inside string does not terminate object");
    checkEq(ainput::qjsonRawValue(cfg, "absent"), "", "absent key");
    checkEq(ainput::qjsonStringValue(cfg, "api_key"), "sk-1",
            "string value without quotes");
    checkEq(ainput::qjsonStringValue(cfg, "endpoint"), "https://x/y",
            "string value for endpoint");
}

void testErrorDetail() {
    checkEq(ainput::qwenExtractErrorDetail(
                "{\"code\":\"InvalidApiKey\",\"message\":\"Invalid API-key\","
                "\"request_id\":\"r\"}"),
            "InvalidApiKey: Invalid API-key", "code+message detail");
    checkEq(ainput::qwenExtractErrorDetail("{\"message\":\"boom\"}"), "boom",
            "message-only detail");
    checkEq(ainput::qwenExtractErrorDetail(""), "", "empty body detail");
}

void testRemoveTopLevelProperty() {
    using ainput::qjsonRemoveTopLevelProperty;
    checkEq(qjsonRemoveTopLevelProperty(
                "{\"api_key\":\"x\",\"model\":\"y\"}", "api_key"),
            "{\"model\":\"y\"}", "remove first property");
    checkEq(qjsonRemoveTopLevelProperty(
                "{\"model\":\"y\",\"api_key\":\"x\",\"endpoint\":\"z\"}",
                "api_key"),
            "{\"model\":\"y\",\"endpoint\":\"z\"}", "remove middle property");
    checkEq(qjsonRemoveTopLevelProperty(
                "{\"model\":\"y\",\"api_key\":\"x\"}", "api_key"),
            "{\"model\":\"y\"}", "remove last property");
    checkEq(qjsonRemoveTopLevelProperty("{\"api_key\":\"x\"}", "api_key"),
            "{}", "remove only property");
    // Pretty-printed with whitespace and newlines.
    checkEq(qjsonRemoveTopLevelProperty(
                "{\n  \"api_key\": \"sk-1\",\n  \"model\": \"m\"\n}\n",
                "api_key"),
            "{\n  \"model\": \"m\"\n}\n", "remove in pretty-printed JSON");
    // Nested same-name key must survive.
    std::string nested =
        "{\"vocabulary\":{\"api_key\":\"keep\"},\"api_key\":\"drop\","
        "\"model\":\"m\"}";
    std::string stripped = qjsonRemoveTopLevelProperty(nested, "api_key");
    checkEq(ainput::qjsonStringValue(stripped, "model"), "m",
            "model survives nested case");
    check(stripped.find("keep") != std::string::npos,
          "nested api_key value kept");
    check(stripped.find("drop") == std::string::npos,
          "top-level api_key dropped");
    // Value containing commas and braces inside a string.
    checkEq(qjsonRemoveTopLevelProperty(
                "{\"api_key\":\"a,b}c\",\"model\":\"y\"}", "api_key"),
            "{\"model\":\"y\"}", "commas/braces inside value string");
    // Non-string value.
    checkEq(qjsonRemoveTopLevelProperty(
                "{\"api_key\":12345,\"model\":\"y\"}", "api_key"),
            "{\"model\":\"y\"}", "remove non-string value");
    // Object value.
    checkEq(qjsonRemoveTopLevelProperty(
                "{\"api_key\":{\"a\":[1,2]},\"model\":\"y\"}", "api_key"),
            "{\"model\":\"y\"}", "remove object value");
    // Absent / invalid.
    checkEq(qjsonRemoveTopLevelProperty("{\"model\":\"y\"}", "api_key"), "",
            "absent key returns empty");
    checkEq(qjsonRemoveTopLevelProperty("not json", "api_key"), "",
            "invalid json returns empty");
}

void testLegacyDetection() {
    check(ainput::qwenIsLegacyModel("qwen3-asr-flash"), "qwen3 mainline");
    check(ainput::qwenIsLegacyModel("qwen3-asr-flash-2025-09-08"),
          "qwen3 snapshot");
    check(!ainput::qwenIsLegacyModel("qwen-audio-3.1-asr-flash"),
          "new-gen is not legacy");
    check(!ainput::qwenIsLegacyModel("fun-asr-flash-2026-06-15"),
          "fun-asr is not legacy");
    check(!ainput::qwenIsLegacyModel("some-future-model"),
          "unknown models default to new style");

    check(ainput::qwenUsesLegacyRequest("qwen3-asr-flash"),
          "legacy wire format for qwen3");
    check(!ainput::qwenUsesLegacyRequest("qwen-audio-3.1-asr-flash"),
          "new style for 3.1");
    check(!ainput::qwenUsesLegacyRequest("some-future-model"),
          "unknown models default to new style");
}

void testStripComments() {
    auto stripped = ainput::qjsonStripComments(
        "{\n  // full line comment\n  \"a\": 1 // trailing\n}");
    check(stripped.find("//") == std::string::npos, "comments removed");
    check(ainput::qjsonSyntaxError(stripped).empty(), "stripped text is JSON");
    check(stripped.size() == std::string("{\n  // full line comment\n  \"a\": 1 // trailing\n}").size(),
          "stripping preserves length");

    // '//' inside a string value is not a comment.
    std::string withUrl =
        "{\"endpoint\": \"https://dashscope.aliyuncs.com/api/v1\", \"a\": 1}";
    check(ainput::qjsonStripComments(withUrl) == withUrl,
          "URL inside string untouched");
}

void testSetTopLevelString() {
    // Replace an existing property.
    auto json = ainput::qjsonSetTopLevelString(
        "{\"provider\": \"qwen\", \"ui\": {\"a\": 1}}", "provider", "doubao");
    check(ainput::qjsonStringValue(json, "provider") == "doubao",
          "existing value replaced");
    check(ainput::qjsonStringValue(json, "ui") == "",
          "other properties untouched (object value)");

    // Insert into a non-empty object.
    json = ainput::qjsonSetTopLevelString("{\"a\": 1}", "provider", "qwen");
    check(ainput::qjsonSyntaxError(json).empty(), "insert keeps valid JSON");
    check(ainput::qjsonStringValue(json, "provider") == "qwen" &&
              ainput::qjsonRawValue(json, "a") == "1",
          "insert preserves existing keys");

    // Insert into an empty object.
    json = ainput::qjsonSetTopLevelString("{}", "provider", "mock");
    check(ainput::qjsonSyntaxError(json).empty(), "empty object stays valid");
    check(ainput::qjsonStringValue(json, "provider") == "mock",
          "value inserted into empty object");

    // Missing file (empty text) bootstraps a fresh object.
    json = ainput::qjsonSetTopLevelString("", "provider", "mock");
    check(ainput::qjsonSyntaxError(json).empty(), "bootstrap object valid");
    check(ainput::qjsonStringValue(json, "provider") == "mock",
          "bootstrap value present");

    // Value is escaped; insert does not disturb nested objects.
    json = ainput::qjsonSetTopLevelString(
        "{\"audio\": {\"denoise\": \"speexdsp\"}}", "provider", "a\"b");
    check(ainput::qjsonSyntaxError(json).empty(), "escaped quote valid");
    check(ainput::qjsonRawValue(json, "audio") == "{\"denoise\": \"speexdsp\"}",
          "nested section byte-identical");
}

void testSetSectionString() {
    // Update a key inside an existing section.
    std::string doc = "{\n  \"provider\": \"qwen\",\n  \"audio\": {\"lufs_target\": -16}\n}";
    auto json = ainput::qjsonSetSectionString(doc, "audio", "denoise", "deepfilter");
    check(ainput::qjsonSyntaxError(json).empty(), "section update stays valid");
    check(ainput::qjsonRawValue(json, "audio") ==
              "{\"denoise\":\"deepfilter\",\"lufs_target\": -16}",
          "section key updated, siblings kept");
    check(ainput::qjsonStringValue(json, "provider") == "qwen",
          "top-level keys untouched");

    // Section absent: created with the single key.
    json = ainput::qjsonSetSectionString("{\"provider\": \"qwen\"}",
                                         "audio", "denoise", "speexdsp");
    check(ainput::qjsonSyntaxError(json).empty(), "section insert stays valid");
    check(ainput::qjsonRawValue(json, "audio") == "{\"denoise\":\"speexdsp\"}",
          "section created");

    // Empty file bootstraps.
    json = ainput::qjsonSetSectionString("", "audio", "denoise", "none");
    check(ainput::qjsonSyntaxError(json).empty(), "bootstrap section valid");
}

} // namespace

int main() {
    testTextExtraction();
    testEscapes();
    testTopLevelString();
    testRawValue();
    testErrorDetail();
    testRemoveTopLevelProperty();
    testLegacyDetection();
    testStripComments();
    testSetTopLevelString();
    testSetSectionString();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "qwen_json: all checks passed\n";
    return 0;
}
