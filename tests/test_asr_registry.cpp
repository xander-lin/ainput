#include "asr_provider.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    const char *tmp = std::getenv("MESON_TEST_TMPDIR");
    fs::path root = (tmp && *tmp) ? tmp : "/tmp/ainput-asr-registry";
    fs::path cfgDir = root / ".config/ainput";
    fs::create_directories(cfgDir);
    setenv("HOME", root.c_str(), 1);

    {
        std::ofstream f(cfgDir / "qwen.json");
        f << "{\"api_key\":\"sk-test\",\"model\":\"qwen-audio-3.1-asr-flash\"}\n";
    }
    {
        std::ofstream f(cfgDir / "doubao.json");
        f << "{\"api_key\":\"k\",\"resource_id\":\"volc.seedasr.auc\",\"model_name\":\"bigmodel\"}\n";
    }

    auto &registry = ainput::AsrProviderRegistry::instance();
    auto factories = registry.listFactories();

    bool foundMock = false;
    std::string qwenDisplay, doubaoDisplay;
    for (const auto &factory : factories) {
        if (factory.first == "mock") foundMock = true;
        if (factory.first == "qwen") qwenDisplay = factory.second;
        if (factory.first == "doubao") doubaoDisplay = factory.second;
    }

    if (!foundMock) {
        std::cerr << "mock ASR provider is not registered\n";
        return 1;
    }

    // listFactories reports config-driven display names with the actual model.
    if (qwenDisplay != "Qwen · qwen-audio-3.1-asr-flash") {
        std::cerr << "qwen display name: '" << qwenDisplay
                  << "' (expected 'Qwen · qwen-audio-3.1-asr-flash')\n";
        return 1;
    }
    if (doubaoDisplay != "Doubao · bigmodel") {
        std::cerr << "doubao display name: '" << doubaoDisplay
                  << "' (expected 'Doubao · bigmodel')\n";
        return 1;
    }

    auto provider = registry.create("mock");
    if (!provider) {
        std::cerr << "failed to create mock ASR provider\n";
        return 1;
    }

    std::string result;
    bool final = false;
    provider->setResultCallback([&](const std::string &text, bool isFinal) {
        result = text;
        final = isFinal;
    });

    provider->transcribe(std::vector<int16_t>{1, -1, 2, -2}, "");

    if (result != "hello world" || !final) {
        std::cerr << "unexpected mock result: text='" << result
                  << "' final=" << final << "\n";
        return 1;
    }

    return 0;
}
