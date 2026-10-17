#include "fire_red_provider.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::filesystem::path makeFakeSherpa(const std::filesystem::path &root,
                                     const std::string &name,
                                     const std::string &body) {
    auto path = root / name;
    {
        std::ofstream script(path);
        script << "#!/bin/sh\n" << body;
    }
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write |
            std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);
    return path;
}

void writeConfig(const std::filesystem::path &path,
                 const std::filesystem::path &root,
                 const std::filesystem::path &fakeSherpa) {
    std::ofstream config(path);
    config << "{\"model_dir\":\"" << root.string()
           << "\",\"bin_path\":\"" << fakeSherpa.string()
           << "\",\"timeout_sec\":3}\n";
    // The provider preflight requires the model files to exist; the fake
    // sherpa binary never reads them.
    for (const char *f : {"encoder.int8.onnx", "decoder.int8.onnx", "tokens.txt"}) {
        std::ofstream m(root / f);
        m << "placeholder";
    }
}

bool testNormalCompletion(const std::filesystem::path &root,
                          const std::filesystem::path &configPath) {
    auto fakeSherpa = makeFakeSherpa(
        root, "fake-sherpa-result",
        "sleep 0.1\nprintf '{\"text\": \"async result\"}\\n'\n");
    writeConfig(configPath, root, fakeSherpa);

    std::mutex mutex;
    std::condition_variable ready;
    bool completed = false;
    std::string result;
    std::string error;

    auto provider = std::make_unique<ainput::FireRedAsrProvider>();
    provider->setResultCallback([&](const std::string &text, bool) {
        std::lock_guard<std::mutex> lock(mutex);
        result = text;
        completed = true;
        ready.notify_one();
    });
    provider->setErrorCallback([&](const std::string &message, ainput::AsrErrorCategory) {
        std::lock_guard<std::mutex> lock(mutex);
        error = message;
        completed = true;
        ready.notify_one();
    });
    provider->transcribe({}, (root / "normal.wav").string());

    std::unique_lock<std::mutex> lock(mutex);
    if (!ready.wait_for(lock, std::chrono::seconds(3), [&] { return completed; })) {
        std::cerr << "normal asynchronous provider did not complete\n";
        return false;
    }
    lock.unlock();
    provider.reset();

    if (!error.empty() || result != "async result") {
        std::cerr << "unexpected normal result='" << result
                  << "' error='" << error << "'\n";
        return false;
    }
    return true;
}

bool testHungChildCancellation(const std::filesystem::path &root,
                               const std::filesystem::path &configPath) {
    auto fakeSherpa = makeFakeSherpa(
        root, "fake-sherpa-hung", "trap '' TERM\nsleep 30\n");
    writeConfig(configPath, root, fakeSherpa);

    auto provider = std::make_unique<ainput::FireRedAsrProvider>();
    provider->transcribe({}, (root / "hung.wav").string());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto start = Clock::now();
    provider.reset();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start);
    if (elapsed > std::chrono::seconds(2)) {
        std::cerr << "hung child cancellation took " << elapsed.count() << "ms\n";
        return false;
    }
    return true;
}

bool testCallbackRelease(const std::filesystem::path &root,
                         const std::filesystem::path &configPath) {
    auto fakeSherpa = makeFakeSherpa(
        root, "fake-sherpa-release",
        "printf '{\"text\": \"release result\"}\\n'\n");
    writeConfig(configPath, root, fakeSherpa);

    std::mutex mutex;
    std::condition_variable ready;
    bool completed = false;
    std::unique_ptr<ainput::FireRedAsrProvider> provider =
        std::make_unique<ainput::FireRedAsrProvider>();
    provider->setResultCallback([&](const std::string &, bool) {
        provider.reset();
        std::lock_guard<std::mutex> lock(mutex);
        completed = true;
        ready.notify_one();
    });
    provider->transcribe({}, (root / "release.wav").string());

    std::unique_lock<std::mutex> lock(mutex);
    if (!ready.wait_for(lock, std::chrono::seconds(3), [&] { return completed; })) {
        std::cerr << "callback-triggered provider release did not complete\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    namespace fs = std::filesystem;

    const char *tmp = std::getenv("MESON_TEST_TMPDIR");
    fs::path root = (tmp && *tmp) ? tmp : "/tmp/ainput-provider-lifetime";
    fs::path configDir = root / ".config/ainput";
    fs::create_directories(configDir);

    setenv("HOME", root.c_str(), 1);
    auto configPath = configDir / "fire_red.json";
    return testNormalCompletion(root, configPath) &&
                   testHungChildCancellation(root, configPath) &&
                   testCallbackRelease(root, configPath)
               ? 0
               : 1;
}
