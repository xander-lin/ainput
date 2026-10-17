#include "doubao_provider.h"
#include "qwen_provider.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

template<typename Provider>
bool testProvider(const std::filesystem::path &root, const std::string &name) {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<int> order;
    std::vector<std::thread::id> threads;
    std::vector<std::filesystem::path> paths;
    Provider provider;

    for (int i = 1; i <= 2; ++i) {
        auto path = root / (name + "-" + std::to_string(i) + ".wav");
        std::ofstream(path) << "placeholder";
        paths.push_back(path);
        provider.setErrorCallback([&, i](const std::string &, ainput::AsrErrorCategory) {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(i);
            threads.push_back(std::this_thread::get_id());
            ready.notify_one();
        });
        provider.transcribe({}, path.string());
    }

    std::unique_lock<std::mutex> lock(mutex);
    if (!ready.wait_for(lock, std::chrono::seconds(2),
                        [&] { return order.size() == 2; })) {
        std::cerr << name << " queue did not drain\n";
        return false;
    }
    lock.unlock();

    if (order != std::vector<int>({1, 2}) || threads[0] != threads[1]) {
        std::cerr << name << " did not use one FIFO worker\n";
        return false;
    }
    for (const auto &path : paths) {
        if (std::filesystem::exists(path)) {
            std::cerr << name << " left temporary WAV " << path << "\n";
            return false;
        }
    }
    return true;
}

template<typename Provider>
bool testQueuedCleanup(const std::filesystem::path &root,
                       const std::string &name) {
    std::vector<std::filesystem::path> paths;
    {
        Provider provider;
        for (int i = 1; i <= 8; ++i) {
            auto path = root / (name + "-cleanup-" + std::to_string(i) + ".wav");
            std::ofstream(path) << "placeholder";
            paths.push_back(path);
            provider.transcribe({}, path.string());
        }
    }
    for (const auto &path : paths) {
        if (std::filesystem::exists(path)) {
            std::cerr << name << " destructor left queued WAV " << path << "\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const char *tmp = std::getenv("MESON_TEST_TMPDIR");
    fs::path root = (tmp && *tmp) ? tmp : "/tmp/ainput-cloud-provider-queue";
    fs::create_directories(root / ".config/ainput");
    setenv("HOME", root.c_str(), 1);
    // Pin configs to this sandbox (the developer\'s real
    // ~/.config/ainput would otherwise carry a live api_key).
    {
        std::ofstream q(root / ".config/ainput/qwen.json");
        q << "{\"api_key\":\"\",\"endpoint\":\"http://127.0.0.1:1/x\"}\n";
        std::ofstream d(root / ".config/ainput/doubao.json");
        d << "{\"api_key\":\"\",\"resource_id\":\"\"}\n";
    }

    return testProvider<ainput::DoubaoAsrProvider>(root, "doubao") &&
                   testProvider<ainput::QwenAsrProvider>(root, "qwen") &&
                   testQueuedCleanup<ainput::DoubaoAsrProvider>(root, "doubao") &&
                   testQueuedCleanup<ainput::QwenAsrProvider>(root, "qwen")
               ? 0
               : 1;
}
