#include "audio_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <fstream>

static std::vector<int16_t> readWav(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return {}; }

    char riff[5] = {};
    f.read(riff, 4);
    if (strncmp(riff, "RIFF", 4) != 0) { fprintf(stderr, "Not RIFF\n"); return {}; }

    f.seekg(40, std::ios::beg);
    uint32_t dataSize = 0;
    f.read(reinterpret_cast<char *>(&dataSize), 4);

    f.seekg(44, std::ios::beg);
    size_t sampleCount = dataSize / 2;
    std::vector<int16_t> samples(sampleCount);
    f.read(reinterpret_cast<char *>(samples.data()), dataSize);
    return samples;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.wav> <output.wav> [none|speexdsp|deepfilter]\n", argv[0]);
        return 1;
    }

    auto samples = readWav(argv[1]);
    if (samples.empty()) return 1;

    std::string denoiser = (argc > 3) ? argv[3] : "none";
    fprintf(stderr, "Processing %s -> %s (denoiser=%s, %zu samples)\n",
            argv[1], argv[2], denoiser.c_str(), samples.size());

    auto t0 = std::chrono::steady_clock::now();
    ainput::AudioCapture::processSamples(samples, denoiser);
    ainput::AudioCapture::writeWav(samples, argv[2]);
    auto t1 = std::chrono::steady_clock::now();

    fprintf(stderr, "Done: %s (%zu samples, %ldms)\n",
            argv[2], samples.size(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return 0;
}
