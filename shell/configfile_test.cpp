#include "configfile.h"

#include <fcitx-config/enum.h>
#include <fcitx-config/iniparser.h>

#include <cstdlib>
#include <sstream>
#include <string>

namespace {

using namespace fcitx;

FCITX_CONFIG_ENUM(TestDenoise, none, speexdsp);

FCITX_CONFIGURATION(
    TestConfig,
    Option<std::string> provider{this, "Provider", "ASR backend", "qwen"};
    HiddenOption<int, IntConstrain> minActive{
        this, "MinActive", "Minimum active audio (ms)", 250, fcitx::IntConstrain(20, 10000)};
    Option<TestDenoise> denoise{this, "Denoise", "Noise reduction", TestDenoise::none};
    Option<std::string> key{this, "Key", "An empty default", ""};
);

std::string textOf(const RawConfig &raw) {
    std::ostringstream out;
    writeAsIni(raw, out);
    return out.str();
}

void check(bool condition) {
    if (!condition) std::abort();
}

constexpr auto npos = std::string::npos;

} // namespace

int main() {
    TestConfig config;

    // fcitx5's own writer leaves a default as a comment. That is the format
    // the completeness check has to tell apart from a complete file.
    RawConfig fcitxStyle;
    config.save(fcitxStyle);
    // fcitx5's writer prefixes a comment with "# ".
    check(textOf(fcitxStyle).find("# Provider=qwen\n") != npos);
    RawConfig readFcitxStyle;
    std::istringstream fcitxStyleIn(textOf(fcitxStyle));
    readFromIni(readFcitxStyle, fcitxStyleIn);
    check(!ainput::listsEveryOption(readFcitxStyle, config));
    check(!ainput::listsEveryOption(RawConfig(), config));

    // Every option is an active line carrying its value, under its comment.
    const std::string text = textOf(ainput::completeConfig(config));
    check(text.find("# ASR backend\nProvider=qwen\n") == 0);
    check(text.find("\n# Minimum active audio (ms)\nMinActive=250\n") != npos);
    check(text.find("\n# Noise reduction\nDenoise=none\n") != npos);
    check(text.find("\n# An empty default\nKey=\n") != npos);
    check(text.find("# Provider=") == npos);
    check(text.find("# MinActive=") == npos);

    // Read back the way fcitx5 reads the file, the values are the same.
    RawConfig read;
    std::istringstream in(text);
    readFromIni(read, in);
    check(ainput::listsEveryOption(read, config));
    TestConfig reloaded;
    reloaded.load(read);
    check(reloaded.provider.value() == "qwen");
    check(reloaded.minActive.value() == 250);
    check(reloaded.denoise.value() == TestDenoise::none);
    check(reloaded.key.value().empty());

    // Changed values are written as they are.
    config.minActive.setValue(400);
    config.denoise.setValue(TestDenoise::speexdsp);
    config.key.setValue("sk-x");
    const std::string changed = textOf(ainput::completeConfig(config));
    check(changed.find("\nMinActive=400\n") != npos);
    check(changed.find("\nDenoise=speexdsp\n") != npos);
    check(changed.find("\nKey=sk-x\n") != npos);
}
