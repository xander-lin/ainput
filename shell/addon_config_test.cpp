// Loads the real addon into an embedded fcitx5 instance and checks what it
// does to its configuration file on start: the file must list every option
// with its value, and an older file must be completed without losing the
// values it already held.

#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>
#include <fcitx-utils/standardpaths.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "configfile.h"

extern "C" fcitx::AddonFactory *fcitx_addon_factory_instance();

namespace {

namespace fs = std::filesystem;

void check(bool condition) {
    if (!condition) std::abort();
}

std::string readFile(const fs::path &path) {
    std::ifstream in(path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// Every shared option of the addon, including the ones hidden from the
/// settings dialog: those can only be changed in the file.
const std::vector<std::string> kOptions = {
    "Provider",       "ActivationMsec", "NotificationTimeout", "MaxRecordingSec",
    "BufferMs",       "Denoise",        "LufsTarget",          "SpeexLevel",
    "CrestThreshold", "MinActiveMsec",  "LufsMargin",          "QwenApiKey",
    "QwenModel",      "DoubaoApiKey",   "DoubaoModel",
};

std::string valueOf(const std::string &text, const std::string &key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(key + "=", 0) == 0) {
            return line.substr(key.size() + 1);
        }
    }
    return "<missing>";
}

void checkListsEveryOption(const std::string &text) {
    for (const auto &key : kOptions) {
        check(valueOf(text, key) != "<missing>");
    }
    // A default must be an active line, not fcitx5's commented form
    // ("# Key=value"). A description may contain "=" (e.g. "empty =
    // bigmodel"), but never right after the option name.
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("# ", 0) != 0) continue;
        const std::string rest = line.substr(2);
        const auto equal = rest.find('=');
        check(equal == std::string::npos || rest.find(' ') < equal);
    }
}

} // namespace

int main() {
    // Everything the addon reads or writes lives under one directory that is
    // thrown away afterwards. HOME and XDG_CONFIG_HOME are set before
    // anything of fcitx5 or the addon is constructed.
    const char *fromMeson = std::getenv("MESON_TEST_TMPDIR");
    const fs::path root = fromMeson ? fs::path(fromMeson) / "addon-config"
                                    : fs::path("/tmp/ainput-addon-config-test");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "config");
    setenv("HOME", root.c_str(), 1);
    setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);

    char name[] = "ainput-addon-config-test";
    char disable[] = "--disable=all";
    char *args[] = {name, disable, nullptr};
    fcitx::Instance instance(2, args);
    // Focus rules do not apply here, but initializing first keeps this test
    // in line with the other embedded-instance test (found.md #35).
    instance.initialize();
    // The directory fcitx5 itself uses to save a PkgConfig file, i.e.
    // `$XDG_CONFIG_HOME/fcitx5`.
    const fs::path file = fcitx::StandardPaths::global().userDirectory(
                              fcitx::StandardPathsType::PkgConfig) /
                          ainput::kConfigFile;
    // The isolation the rest of this test relies on.
    check(file == root / "config" / "fcitx5" / "conf" / "ainput.conf");

    auto *factory = fcitx_addon_factory_instance();
    check(factory != nullptr);

    // First run: there is no file, and the addon writes a complete one.
    check(!fs::exists(file));
    auto *addon = factory->create(&instance.addonManager());
    check(addon != nullptr);
    delete addon;
    check(fs::exists(file));
    std::string text = readFile(file);
    checkListsEveryOption(text);
    check(valueOf(text, "Provider") == "qwen");
    check(valueOf(text, "Denoise") == "none");
    check(valueOf(text, "MinActiveMsec") == "250");
    check(valueOf(text, "LufsMargin") == "4.2");

    // An older file, as fcitx5 writes it: defaults are comments and only the
    // changed values are set. It is completed and those values are kept.
    {
        std::ofstream out(file);
        out << "# ASR backend (qwen, doubao, zipformer, fire_red, mock)\n"
               "# Provider=qwen\n"
               "# Noise reduction method\n"
               "Denoise=speexdsp\n"
               "# Optional crest and VAD gate (0 skips both checks)\n"
               "CrestThreshold=0\n";
    }
    addon = factory->create(&instance.addonManager());
    check(addon != nullptr);
    delete addon;
    text = readFile(file);
    checkListsEveryOption(text);
    check(valueOf(text, "Denoise") == "speexdsp");
    check(valueOf(text, "CrestThreshold") == "0");
    check(valueOf(text, "Provider") == "qwen");
    check(valueOf(text, "MinActiveMsec") == "250");

    // The addon seeds its provider files on creation; they must have gone to
    // the temporary HOME, not to the real one.
    check(fs::exists(root / ".config" / "ainput" / "qwen.json"));
    fs::remove_all(root, ec);
}
