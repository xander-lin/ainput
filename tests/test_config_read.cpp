#include "config_schema.h"
#include "config_templates.h"
#include "ainput_config.h"

#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static int failures = 0;

static void check(bool ok, const std::string &what) {
    if (!ok) {
        failures++;
        std::cerr << "FAIL: " << what << "\n";
    }
}

static void writeFile(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

static void testTemplatesAreValid() {
    // The templates must be valid JSON after comment stripping and pass
    // their schemas (an invalid template would break every fresh install).
    std::string cfg = ainput::qjsonStripComments(ainput::configJsonTemplate());
    check(ainput::qjsonSyntaxError(cfg).empty(), "config.json template is JSON");
    check(ainput::validateConfigFileJson(cfg).empty(),
          "config.json template passes schema");

    std::string qwen = ainput::qjsonStripComments(ainput::qwenJsonTemplate());
    check(ainput::qjsonSyntaxError(qwen).empty(), "qwen.json template is JSON");
    check(ainput::validateConfigJson(qwen, ainput::qwenConfigSchema()).empty(),
          "qwen.json template passes schema");
    check(ainput::qjsonStringValue(qwen, "api_key") == ainput::kApiKeyPlaceholder,
          "qwen template carries the placeholder key");

    std::string doubao =
        ainput::qjsonStripComments(ainput::doubaoJsonTemplate());
    check(ainput::qjsonSyntaxError(doubao).empty(),
          "doubao.json template is JSON");
    check(ainput::validateConfigJson(doubao, ainput::doubaoConfigSchema())
              .empty(),
          "doubao.json template passes schema");
}

int main() {
    auto base = fs::temp_directory_path() /
                ("ainput-config-read-" + std::to_string(getpid()));
    auto home = base / "home";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);

    testTemplatesAreValid();

    // Seeded files: reading a missing monitored file recreates the template.
    auto cfgPath = home / ".config/ainput/config.json";
    auto cfg = ainput::readConfigFile("config.json");
    check(fs::exists(cfgPath), "missing config.json was seeded");
    check(ainput::jsonStr(cfg, "provider") == "qwen",
          "seeded template carries provider=qwen");

    // Renaming the file away and reading again regenerates it.
    fs::rename(cfgPath, home / ".config/ainput/config.json.bak");
    cfg = ainput::readConfigFile("config.json");
    check(fs::exists(cfgPath), "renamed-away config.json regenerated");
    check(ainput::jsonStr(cfg, "provider") == "qwen",
          "regenerated template still parses");

    // Existing files are never overwritten.
    writeFile(home / ".config/ainput/qwen.json",
              "{\"api_key\":\"sk-real\",\"model\":\"fun-asr-flash\"}");
    ainput::readConfigFile("qwen.json");
    std::string qwen =
        ainput::readFileIfExists(home / ".config/ainput/qwen.json");
    check(qwen.find("sk-real") != std::string::npos &&
              qwen.find(ainput::kApiKeyPlaceholder) == std::string::npos,
          "existing qwen.json untouched by seeding");

    // Seeded credential template appears for doubao and is valid.
    auto doubao = ainput::readConfigFile("doubao.json");
    check(fs::exists(home / ".config/ainput/doubao.json"),
          "missing doubao.json was seeded");
    check(ainput::qjsonStringValue(doubao, "api_key") ==
              ainput::kApiKeyPlaceholder,
          "doubao template placeholder present");

    // Unmonitored files stay absent: local providers are not seeded.
    check(ainput::readConfigFile("zipformer.json").empty(),
          "zipformer.json still reads as empty");
    check(!fs::exists(home / ".config/ainput/zipformer.json"),
          "zipformer.json is not seeded");

    // Mode 0600 on seeded files.
    struct stat st {};
    stat((home / ".config/ainput/doubao.json").c_str(), &st);
    check((st.st_mode & 0777) == 0600, "seeded file mode is 0600");

    // Comments are stripped as spaces: offsets still match the original file.
    writeFile(home / ".config/ainput/config.json", R"JSON({
    // which provider is active; Ctrl+CapsLock rewrites this line
    "provider": "qwen",
    "ui": { "activation_msec": 250 },   // faster hold activation
    "audio": {
        "denoise": "speexdsp",
        "lufs_target": -16.0
    }
})JSON");
    cfg = ainput::readConfigFile("config.json");
    check(cfg.size() == fs::file_size(home / ".config/ainput/config.json"),
          "stripping preserves length (byte offsets stay valid)");

    // '/' inside strings must never be treated as a comment.
    writeFile(home / ".config/ainput/qwen.json",
              "{\"api_key\":\"sk-x\", \"endpoint\":\"https://dashscope.aliyuncs.com/x\"}");
    qwen = ainput::readConfigFile("qwen.json");
    check(ainput::jsonStr(qwen, "endpoint") == "https://dashscope.aliyuncs.com/x",
          "URL inside string survives comment stripping");

    // Section extraction.
    auto audio = ainput::readConfigSection("config.json", "audio");
    check(ainput::jsonStr(audio, "denoise") == "speexdsp",
          "audio section extracted");
    auto ui = ainput::readConfigSection("config.json", "ui");
    check(ainput::jsonInt(ui, "activation_msec") == 250,
          "ui section extracted");
    check(ainput::readConfigSection("config.json", "missing").empty(),
          "absent section reads as empty");

    fs::remove_all(base);
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "config_read: all checks passed\n";
    return 0;
}
