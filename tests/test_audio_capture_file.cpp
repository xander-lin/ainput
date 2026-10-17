#include "audio_capture.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string tempWavPath() {
    const char *tmp = std::getenv("MESON_TEST_TMPDIR");
    if (!tmp || !*tmp) {
        tmp = "/tmp";
    }
    return std::string(tmp) + "/ainput-audio-capture-file.wav";
}

uint32_t readLe32(const std::vector<unsigned char> &bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

} // namespace

int main() {
    constexpr int sampleRate = 16000;
    constexpr double frequency = 440.0;
    std::vector<int16_t> samples(sampleRate / 2);

    for (size_t i = 0; i < samples.size(); ++i) {
        double phase = 2.0 * M_PI * frequency * static_cast<double>(i) / sampleRate;
        samples[i] = static_cast<int16_t>(std::sin(phase) * 12000.0);
    }

    ainput::AudioCapture::processSamples(samples, "none");

    if (samples.empty()) {
        std::cerr << "audio pipeline removed all generated samples\n";
        return 1;
    }

    auto path = tempWavPath();
    ainput::AudioCapture::writeWav(samples, path);

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "failed to open generated WAV: " << path << "\n";
        return 1;
    }

    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    std::remove(path.c_str());

    if (bytes.size() < 44) {
        std::cerr << "generated WAV is too small\n";
        return 1;
    }
    if (std::string(reinterpret_cast<const char *>(bytes.data()), 4) != "RIFF" ||
        std::string(reinterpret_cast<const char *>(bytes.data() + 8), 4) != "WAVE" ||
        std::string(reinterpret_cast<const char *>(bytes.data() + 36), 4) != "data") {
        std::cerr << "generated WAV header is invalid\n";
        return 1;
    }

    uint32_t dataSize = readLe32(bytes, 40);
    if (dataSize != (bytes.size() - 44) || dataSize == 0 || (dataSize % 2) != 0) {
        std::cerr << "generated WAV data size is invalid\n";
        return 1;
    }

    return 0;
}
