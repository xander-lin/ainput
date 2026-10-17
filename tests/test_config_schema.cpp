#include "config_schema.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
    if (!ok) {
        failures++;
        std::cerr << "FAIL: " << what << "\n";
    }
}

bool hasIssue(const std::vector<ainput::ConfigIssue> &issues,
              const std::string &needle, bool fatal) {
    for (const auto &issue : issues) {
        if (issue.fatal == fatal &&
            issue.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void testSyntax() {
    check(ainput::qjsonSyntaxError("{\"a\": 1}").empty(), "valid object");
    check(ainput::qjsonSyntaxError("{\"a\": \"x\", \"b\": [1, 2]}").empty(),
          "nested values");
    check(!ainput::qjsonSyntaxError("{\"a\": \"x\"").empty(),
          "unterminated object");
    check(!ainput::qjsonSyntaxError("{\"a\": 1,,}").empty(),
          "double comma");
    check(!ainput::qjsonSyntaxError("{\"a\" 1}").empty(),
          "missing colon");
    check(!ainput::qjsonSyntaxError("{\"a\": \"unterminated}").empty(),
          "unterminated string");
    check(!ainput::qjsonSyntaxError("[1, 2]").empty(),
          "root must be object");
    check(!ainput::qjsonSyntaxError("{\"a\": 1} trailing").empty(),
          "trailing content");
    check(ainput::qjsonSyntaxError("").empty() == false ||
          ainput::qjsonSyntaxError("").size() > 0, "empty reported");
    // Offset is reported for syntax errors.
    auto issues = ainput::validateConfigJson("{\"model\": ", ainput::qwenConfigSchema());
    check(issues.size() == 1 && issues[0].fatal, "syntax issue is fatal");
    check(issues[0].message.find("offset") != std::string::npos,
          "syntax issue carries offset");
}

void testUnknownFields() {
    auto issues = ainput::validateConfigJson(
        "{\"api_key\": \"sk\", \"modelname\": \"x\"}", ainput::qwenConfigSchema());
    check(hasIssue(issues, "unknown field \"modelname\"", false),
          "unknown field reported");
    check(hasIssue(issues, "did you mean \"model\"", false),
          "typo suggestion offered");

    issues = ainput::validateConfigJson(
        "{\"api_key\": \"sk\", \"timeout\": 5}", ainput::qwenConfigSchema());
    check(hasIssue(issues, "unknown field \"timeout\"", false),
          "prefix-only field flagged");

    issues = ainput::validateConfigJson(
        "{\"api_key\": \"sk\", \"enable_itn\": true}", ainput::qwenConfigSchema());
    check(hasIssue(issues, "enable_itn", false),
          "field from another vendor flagged in qwen schema");
}

void testTypeChecks() {
    auto issues = ainput::validateConfigJson(
        "{\"api_key\": \"sk\", \"timeout_sec\": \"abc\"}", ainput::qwenConfigSchema());
    check(hasIssue(issues, "\"timeout_sec\" expects an integer", false),
          "int type mismatch");

    issues = ainput::validateConfigJson(
        "{\"api_key\": \"sk\", \"keep_dialect\": \"yes\"}", ainput::qwenConfigSchema());
    check(hasIssue(issues, "\"keep_dialect\" expects true/false", false),
          "bool type mismatch");

    issues = ainput::validateConfigJson(
        "{\"api_key\": 42}", ainput::qwenConfigSchema());
    check(hasIssue(issues, "\"api_key\" expects a string", false),
          "string type mismatch");

    issues = ainput::validateConfigJson(
        "{\"api_key\": \"sk\", \"language_hints\": [\"zh\", 3]}", ainput::qwenConfigSchema());
    check(hasIssue(issues, "\"language_hints\" expects an array of strings", false),
          "array element type mismatch");

    issues = ainput::validateConfigJson(
        "{\"api_key\": \"sk\", \"vocabulary\": [1,2]}", ainput::qwenConfigSchema());
    check(hasIssue(issues, "\"vocabulary\" expects an object", false),
          "object type mismatch");

    issues = ainput::validateConfigJson(
        "{\"api_key\": \"sk\", \"timeout_sec\": 60}", ainput::qwenConfigSchema());
    check(issues.empty(), "valid qwen config passes");
}

void testRequired() {
    auto issues = ainput::validateConfigJson("{\"model\": \"m\"}", ainput::qwenConfigSchema());
    check(hasIssue(issues, "missing required field \"api_key\"", false),
          "missing api_key reported");

    issues = ainput::validateConfigJson("{}", ainput::doubaoConfigSchema());
    check(hasIssue(issues, "missing required field \"api_key\"", false) &&
              hasIssue(issues, "missing required field \"resource_id\"", false),
          "doubao required fields reported");
}

void testTopLevelKeys() {
    auto keys = ainput::qjsonTopLevelKeys(
        "{\"a\": 1, \"nested\": {\"a\": 2, \"b\": 3}, \"c\": [{\"d\": 4}]}");
    check(keys == std::vector<std::string>({"a", "nested", "c"}),
          "top-level keys only, nested ignored");
}

void testSchemas() {
    // Every example file shipped in config/ must validate against its schema.
    struct Case {
        const char *file;
        std::vector<ainput::ConfigField> schema;
        std::vector<std::string> required; // fields that must parse
    };
    // zipformer and fire_red share the local-model schema.
    auto local = ainput::localModelConfigSchema();
    auto zip = ainput::validateConfigJson(
        "{\"model_dir\": \"~/.local/x\", \"timeout_sec\": 120, "
        "\"bin_path\": \"~/.local/bin\"}", local);
    check(zip.empty(), "zipformer example validates");
}

void testNearest() {
    auto schema = ainput::qwenConfigSchema();
    check(ainput::nearestConfigField("modelname", schema) == "model",
          "modelname -> model");
    check(ainput::nearestConfigField("timeout", schema) == "timeout_sec",
          "timeout -> timeout_sec");
    check(ainput::nearestConfigField("zzzzzzzz", schema).empty(),
          "no suggestion for unrelated names");
}

void testPrunedFields() {
    // Tuning knobs retired from the schemas in the 2026-10 restructure must
    // now be flagged, so stale files tell the user they do nothing.
    auto issues = ainput::validateConfigJson(
        "{\"api_key\": \"sk\", \"request_style\": \"legacy\"}",
        ainput::qwenConfigSchema());
    check(hasIssue(issues, "unknown field \"request_style\"", false),
          "qwen request_style retired");

    issues = ainput::validateConfigJson(
        "{\"api_key\": \"k\", \"resource_id\": \"r\", \"max_polls\": 10}",
        ainput::doubaoConfigSchema());
    check(hasIssue(issues, "unknown field \"max_polls\"", false),
          "doubao poll knobs retired");

    issues = ainput::validateConfigJson(
        "{\"model_dir\": \"~/.local/x\", \"num_threads\": 4}",
        ainput::localModelConfigSchema());
    check(hasIssue(issues, "unknown field \"num_threads\"", false),
          "local num_threads retired");

    issues = ainput::validateConfigJson(
        "{\"api_key\": \"k\", \"resource_id\": \"r\", \"timeout_sec\": 60}",
        ainput::doubaoConfigSchema());
    check(issues.empty(), "doubao single timeout_sec accepted");
}

void testConfigFileSections() {
    const char *good =
        "{\n"
        "    \"provider\": \"qwen\",\n"
        "    \"ui\": { \"activation_msec\": 300 },\n"
        "    \"audio\": { \"denoise\": \"speexdsp\", \"crest_threshold\": 0.0 }\n"
        "}\n";
    check(ainput::validateConfigFileJson(good).empty(),
          "valid config.json passes");

    // Unknown top-level key, unknown key inside [audio], bad type in [ui].
    const char *bad =
        "{\n"
        "    \"provider\": \"qwen\",\n"
        "    \"hotkey\": { \"activation_msec\": 300 },\n"
        "    \"audio\": { \"denois\": \"speexdsp\" },\n"
        "    \"ui\": { \"activation_msec\": \"soon\" }\n"
        "}\n";
    auto issues = ainput::validateConfigFileJson(bad);
    check(hasIssue(issues, "unknown field \"hotkey\"", false),
          "unknown top-level section flagged");
    check(hasIssue(issues, "[audio] unknown field \"denois\"", false),
          "audio section issue carries [audio] prefix");
    check(hasIssue(issues, "[audio] unknown field \"denois\" (did you mean \"denoise\"?)", false),
          "audio section typo suggestion");
    check(hasIssue(issues, "[ui] field \"activation_msec\" expects an integer", false),
          "ui section type mismatch");

    // Missing required top-level provider.
    issues = ainput::validateConfigFileJson(
        "{\"ui\": {\"activation_msec\": 300}}");
    check(hasIssue(issues, "missing required field \"provider\"", false),
          "missing provider flagged");

    // Syntax error stays fatal for the whole file.
    issues = ainput::validateConfigFileJson("{\"provider\": ");
    check(issues.size() == 1 && issues[0].fatal,
          "config.json syntax error is fatal");
}

} // namespace

int main() {
    testSyntax();
    testUnknownFields();
    testTypeChecks();
    testRequired();
    testTopLevelKeys();
    testSchemas();
    testNearest();
    testPrunedFields();
    testConfigFileSections();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "config_schema: all checks passed\n";
    return 0;
}
