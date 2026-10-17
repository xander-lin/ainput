#include "audio_capture.h"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using Clock = std::chrono::steady_clock;

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
