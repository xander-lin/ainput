#include "audio_capture.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

int main() {
    using Clock = std::chrono::steady_clock;

    // AudioCapture::start() reads the global config (~/.config/ainput), which
    // would seed the template into the real HOME. Isolate HOME like the other
    // tests (see found.md #5 / #10) so a test run never touches user files.
    namespace fs = std::filesystem;
    const char *tmp = std::getenv("MESON_TEST_TMPDIR");
    fs::path root = (tmp && *tmp) ? tmp : "/tmp/ainput-audio-capture-stop";
    fs::create_directories(root / ".config/ainput");
    setenv("HOME", root.c_str(), 1);

    ainput::AudioCapture capture;
    ainput::AudioCapture secondCapture;
    capture.start();
    secondCapture.start();
    if (capture.wavPath() == secondCapture.wavPath()) {
        std::cerr << "concurrent captures reused a WAV path\n";
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    auto start = Clock::now();
    capture.stop();
    secondCapture.stop();
    auto stopElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start);
    if (stopElapsed > std::chrono::milliseconds(50)) {
        std::cerr << "AudioCapture::stop blocked for " << stopElapsed.count()
                  << "ms\n";
        return 1;
    }

    capture.wait();
    secondCapture.wait();
    auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start);
    if (totalElapsed > std::chrono::seconds(2)) {
        std::cerr << "AudioCapture shutdown took " << totalElapsed.count()
                  << "ms\n";
        return 1;
    }
    return 0;
}
