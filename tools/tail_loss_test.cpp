// 录音尾部丢失诊断测试
// 使用和生产代码完全相同的 PulseAudio 参数和 64K 缓冲区
//
// 编译: g++ -std=c++17 -O2 tail_loss_test.cpp -o tail_loss_test -lpulse-simple -lpulse
//
// 用法:
//   ./tail_loss_test [秒数] [输出wav]   固定时长录音，检查尾部
//   ./tail_loss_test -i [输出wav]       交互模式
//   ./tail_loss_test -s [buf_bytes]     扫频测试
//   ./tail_loss_test -d                 检测硬件 burst 大小
//
// -b <bytes> 指定缓冲区大小（默认 64000）
// -s 不带参数时自动从 ~/.cache/ainput/pa_buffer.json 读取，否则用 64000
//
// 输出: 每次 pa_simple_read 的耗时、累计samples、期望时长 vs 实际时长

#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <signal.h>
#include <fstream>
#include <string>
#include <algorithm>
#include <sys/stat.h>

using Clock = std::chrono::steady_clock;

volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

static size_t gBufSize = 64000;  // 可被 -b 或配置文件覆盖

static size_t loadBufFromConfig() {
    const char *home = getenv("HOME");
    if (!home) return 0;
    std::string path = std::string(home) + "/.cache/ainput/pa_buffer.json";
    std::ifstream f(path);
    if (!f.is_open()) return 0;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    auto pos = content.find("\"buffer_bytes\"");
    if (pos == std::string::npos) return 0;
    pos = content.find(':', pos);
    if (pos == std::string::npos) return 0;
    while (++pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n')) {}
    if (pos >= content.size()) return 0;
    return (size_t)std::stoul(content.substr(pos));
}

static void writeWav(const char *path, const std::vector<int16_t> &samples) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return; }

    long dataSize = (long)samples.size() * 2;
    fwrite("RIFF", 1, 4, f);
    uint32_t chunkSize = 36 + (uint32_t)dataSize; fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t sub1 = 16, sr = 16000u;
    uint16_t fmt = 1, ch = 1, bps = 16, ba = 2;
    uint32_t br = 32000u;
    fwrite(&sub1, 4, 1, f); fwrite(&fmt, 2, 1, f);
    fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t ds = (uint32_t)dataSize; fwrite(&ds, 4, 1, f);
    fwrite(samples.data(), 2, samples.size(), f);
    fclose(f);
}

// 完全复刻生产代码的录音循环
// 返回: {耗时ms, samples}
static std::pair<double, size_t> recordWith64K(
    pa_simple *pa, std::atomic<bool> &stopFlag,
    std::vector<double> *outReadMs = nullptr)
{
    uint8_t buf[65536];  // 和生产代码一致的 64K 缓冲区
    std::vector<int16_t> samples;
    int nReads = 0;

    auto tStart = Clock::now();

    while (!stopFlag) {
        int error = 0;
        auto tBefore = Clock::now();
        if (pa_simple_read(pa, buf, sizeof(buf), &error) < 0) {
            fprintf(stderr, "  [read %d] error: %s\n", nReads, pa_strerror(error));
            break;
        }
        auto tAfter = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(tAfter - tBefore).count();
        if (outReadMs) outReadMs->push_back(ms);

        auto *p = reinterpret_cast<int16_t *>(buf);
        samples.insert(samples.end(), p, p + sizeof(buf) / 2);
        nReads++;

        printf("  [read %d] %.0f ms  (total samples: %zu, audio: %.3f s)\n",
               nReads, ms, samples.size(), samples.size() / 16000.0);
    }

    auto tEnd = Clock::now();
    double wallMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    return {wallMs, samples.size()};
}

static void runFixedDuration(double seconds, const char *outPath) {
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto *pa = pa_simple_new(nullptr, "tail-test", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "PA error: %s\n", pa_strerror(error));
        return;
    }

    printf("=== Fixed duration: %.1f s ===\n", seconds);
    printf("Recording... (speak or make noise to verify audio is captured)\n");

    std::atomic<bool> stopFlag{false};
    std::vector<double> readMs;

    auto tStart = Clock::now();
    std::thread recThread([&]() {
        recordWith64K(pa, stopFlag, &readMs);
    });

    // 等待指定时间后设置停止标志（模拟 CapsLock 松键）
    std::this_thread::sleep_for(std::chrono::microseconds((long long)(seconds * 1e6)));
    auto tStop = Clock::now();
    stopFlag = true;

    recThread.join();
    auto tDone = Clock::now();

    // 从 timing 数据重建 samples 数
    // 实际上需要从 recordWith64K 返回值获取，修改一下
    // 重新设计...

    printf("Stop-to-done delay: %.0f ms\n",
           std::chrono::duration<double, std::milli>(tDone - tStop).count());
}

    // 扫频测试: 动态缓冲区
static void runSweep() {
    printf("=== Sweep test (buffer=%zu bytes, %.1f s) ===\n", gBufSize, gBufSize / 32000.0);

    struct Result {
        double targetSec;
        double wallSec;
        double audioSec;
        double stopToDoneMs;
        int nReads;
        double maxReadMs;
    };

    std::vector<Result> results;

    for (double dur : {0.25, 0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 8.0}) {
        printf("\n--- Testing %.1f s recording ---\n", dur);

        pa_sample_spec ss;
        ss.format = PA_SAMPLE_S16LE;
        ss.rate = 16000;
        ss.channels = 1;

        int error = 0;
        auto *pa = pa_simple_new(nullptr, "tail-test", PA_STREAM_RECORD,
                                 nullptr, "voice", &ss, nullptr, nullptr, &error);
        if (!pa) {
            fprintf(stderr, "PA error: %s\n", pa_strerror(error));
            continue;
        }

        std::vector<uint8_t> buf(gBufSize);
        std::vector<int16_t> samples;
        int nReads = 0;
        double maxReadMs = 0;
        std::atomic<bool> stopFlag{false};

        auto tStart = Clock::now();

        std::thread recThread([&]() {
            while (!stopFlag) {
                int err = 0;
                auto t0 = Clock::now();
                if (pa_simple_read(pa, buf.data(), buf.size(), &err) < 0) {
                    fprintf(stderr, "  read err\n");
                    break;
                }
                auto t1 = Clock::now();
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                if (ms > maxReadMs) maxReadMs = ms;

                auto *p = reinterpret_cast<int16_t *>(buf.data());
                samples.insert(samples.end(), p, p + buf.size() / 2);
                nReads++;
            }
        });

        std::this_thread::sleep_for(std::chrono::microseconds((long long)(dur * 1e6)));
        auto tStop = Clock::now();
        stopFlag = true;
        recThread.join();
        auto tDone = Clock::now();

        pa_simple_free(pa);

        double wallSec = std::chrono::duration<double>(tStop - tStart).count();
        double audioSec = (double)samples.size() / 16000.0;
        double lossSec = wallSec - audioSec;
        double stopToDoneMs = std::chrono::duration<double, std::milli>(tDone - tStop).count();

        printf("  wall=%.3f s  audio=%.3f s  loss=%.3f s (%.0f ms)  n_reads=%d  max_read=%.0f ms  stop→done=%.0f ms\n",
               wallSec, audioSec, lossSec, lossSec * 1000, nReads, maxReadMs, stopToDoneMs);

        results.push_back({dur, wallSec, audioSec, stopToDoneMs, nReads, maxReadMs});

        char path[256];
        snprintf(path, sizeof(path), "/tmp/tail_test_%.0fs.wav", dur);
        writeWav(path, samples);
        printf("  WAV saved: %s\n", path);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    printf("\n=== Summary ===\n");
    printf("%8s %8s %8s %8s %6s %8s\n", "target", "wall", "audio", "loss", "n_read", "stop→d");
    for (auto &r : results) {
        printf("%7.1fs %7.3fs %7.3fs %6.0fms %5d %6.0fms\n",
               r.targetSec, r.wallSec, r.audioSec,
               (r.wallSec - r.audioSec) * 1000, r.nReads, r.stopToDoneMs);
    }
}

// 交互模式
static void runInteractive(const char *outPath) {
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto *pa = pa_simple_new(nullptr, "tail-test", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "PA error: %s\n", pa_strerror(error));
        return;
    }

    signal(SIGINT, onSignal);
    printf("=== Interactive mode ===\n");
    printf("Press Ctrl+C to stop recording (simulates CapsLock release)\n");

    std::vector<uint8_t> buf(gBufSize);
    std::vector<int16_t> samples;
    std::vector<double> readMs;
    int nReads = 0;
    std::atomic<bool> stopFlag{false};

    auto tStart = Clock::now();

    std::thread recThread([&]() {
        while (!stopFlag) {
            int err = 0;
            auto t0 = Clock::now();
            if (pa_simple_read(pa, buf.data(), buf.size(), &err) < 0) {
                fprintf(stderr, "  read err: %s\n", pa_strerror(err));
                break;
            }
            auto t1 = Clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            readMs.push_back(ms);

            auto *p = reinterpret_cast<int16_t *>(buf.data());
            samples.insert(samples.end(), p, p + buf.size() / 2);
            nReads++;

            printf("  [read %d] %.0f ms  (total: %.2f s)\n",
                   nReads, ms, samples.size() / 16000.0);
        }
    });

    // 等 Ctrl+C
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto tStop = Clock::now();
    printf("\nStopping recording...\n");
    stopFlag = true;
    recThread.join();
    auto tDone = Clock::now();

    pa_simple_free(pa);

    double wallSec = std::chrono::duration<double>(tStop - tStart).count();
    double audioSec = (double)samples.size() / 16000.0;
    double stopToDone = std::chrono::duration<double, std::milli>(tDone - tStop).count();

    printf("\n=== Interactive results ===\n");
    printf("Wall time (before stop): %.3f s\n", wallSec);
    printf("Audio duration:          %.3f s (%zu samples)\n", audioSec, samples.size());
    printf("Difference:              %.3f s (%.0f ms)%s\n",
           wallSec - audioSec, (wallSec - audioSec) * 1000,
           (audioSec >= wallSec - 0.1) ? " ✓" : " ✗ LOSS DETECTED");
    printf("Stop→done delay:         %.0f ms\n", stopToDone);
    printf("N reads:                 %d\n", nReads);

    if (readMs.size() > 0) {
        double minMs = 1e9, maxMs = 0, sum = 0;
        for (auto d : readMs) {
            sum += d;
            if (d < minMs) minMs = d;
            if (d > maxMs) maxMs = d;
        }
        printf("Read times: min=%.0f max=%.0f avg=%.0f ms\n",
               minMs, maxMs, sum / readMs.size());
    }

    if (outPath) {
        writeWav(outPath, samples);
        printf("WAV saved: %s\n", outPath);
    }
}

// 硬件中断周期检测 (和生产代码 buffer_detect.cpp 一致的算法)
static void runDetect() {
    printf("=== Hardware burst period detection ===\n");

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto *pa = pa_simple_new(nullptr, "tail-detect", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "PA error: %s\n", pa_strerror(error));
        return;
    }

    // 200 字节 = 100 samples @16kHz S16LE
    uint8_t buf[200];
    constexpr size_t kProbeSize = sizeof(buf);
    std::vector<double> readMs;

    printf("Recording 6s with %zu-byte probe (100 samples @16kHz)...\n", kProbeSize);
    auto tEnd = Clock::now() + std::chrono::seconds(6);

    while (Clock::now() < tEnd) {
        int err = 0;
        auto t0 = Clock::now();
        if (pa_simple_read(pa, buf, kProbeSize, &err) < 0) {
            fprintf(stderr, "  read error: %s\n", pa_strerror(err));
            break;
        }
        auto t1 = Clock::now();
        readMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    pa_simple_free(pa);

    printf("Collected %zu reads\n", readMs.size());
    if (readMs.size() < 3) {
        printf("Too few reads, fallback to 16384\n");
        return;
    }

    // 找相邻最大 gap
    std::vector<double> sorted = readMs;
    std::sort(sorted.begin(), sorted.end());

    size_t gapIdx = 0;
    double maxGap = 0.0;
    for (size_t i = 0; i + 1 < sorted.size(); i++) {
        double gap = sorted[i + 1] - sorted[i];
        if (gap > maxGap) { maxGap = gap; gapIdx = i; }
    }

    double slowVal = sorted[gapIdx + 1];
    double fastVal = sorted[gapIdx];
    bool hasBurst = (maxGap > 50.0) && (slowVal > fastVal * 3.0);

    printf("\n=== Timing analysis ===\n");
    printf("Sorted reads: min=%.1f ms, max=%.1f ms\n", sorted.front(), sorted.back());
    printf("Max gap: %.1f ms at index %zu/%zu (before=%.1f after=%.1f)\n",
           maxGap, gapIdx, sorted.size(), fastVal, slowVal);

    if (!hasBurst) {
        printf("Verdict: CONTINUOUS STREAMING (no interrupt cycle)\n");
        printf("Recommended buffer: 16384 bytes (0.5s)\n");
        return;
    }

    double thresholdMs = fastVal + maxGap * 0.3;
    double fastBound = thresholdMs * 0.3;
    printf("Burst pattern detected: fill=%.0fms, threshold=%.0fms\n", slowVal, thresholdMs);

    // 聚类算 burst 大小
    std::vector<size_t> burstSizes;
    size_t currentBurst = 0;
    bool inBurst = false;

    for (size_t i = 0; i < readMs.size(); i++) {
        if (readMs[i] > thresholdMs) {
            if (inBurst && currentBurst > 0) {
                burstSizes.push_back(currentBurst);
                currentBurst = 0;
            }
            inBurst = true;
            currentBurst = kProbeSize;
        } else if (readMs[i] < fastBound) {
            if (inBurst) {
                currentBurst += kProbeSize;
            } else {
                inBurst = true;
                currentBurst = kProbeSize;
            }
        }
    }
    if (inBurst && currentBurst > 0) {
        burstSizes.push_back(currentBurst);
    }

    size_t maxBurst = 0;
    printf("Detected bursts: ");
    for (size_t i = 0; i < burstSizes.size(); i++) {
        printf("%zu%s", burstSizes[i], i + 1 < burstSizes.size() ? ", " : "");
        if (burstSizes[i] > maxBurst) maxBurst = burstSizes[i];
    }
    printf("\n");

    size_t result = (maxBurst / 1000) * 1000;
    if (result < 4096) result = 4096;
    if (result > 1048576) result = 1048576;

    printf("\n=== Result ===\n");
    printf("Hardware burst: %zu bytes (%.1f s @16kHz)\n", maxBurst, maxBurst / 32000.0);
    printf("Recommended buffer: %zu bytes (%.3f s)\n", result, result / 32000.0);
    printf("Cached buffer file: ~/.cache/ainput/pa_buffer.json\n");
    printf("Set this device's \"buffer_bytes\" to %zu (nested per source id).\n", result);
}

int main(int argc, char **argv) {
    // Parse -b flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            gBufSize = (size_t)atol(argv[i + 1]);
            if (gBufSize < 256) gBufSize = 256;
        }
    }

    // If no -b given, try config file
    if (gBufSize == 64000) {
        auto cached = loadBufFromConfig();
        if (cached > 0) {
            gBufSize = cached;
            printf("Using buffer from config: %zu bytes (%.1f s)\n",
                   gBufSize, gBufSize / 32000.0);
        }
    }

    if (argc >= 2 && strcmp(argv[1], "-s") == 0) {
        runSweep();
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "-i") == 0) {
        const char *path = argc >= 3 ? argv[2] : "/tmp/tail_test_interactive.wav";
        runInteractive(path);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "-d") == 0) {
        runDetect();
        return 0;
    }

    // 固定时长模式
    double dur = argc >= 2 ? atof(argv[1]) : 5.0;
    const char *path = argc >= 3 ? argv[2] : "/tmp/tail_test_fixed.wav";

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto *pa = pa_simple_new(nullptr, "tail-test", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "PA error: %s\n", pa_strerror(error));
        return 1;
    }

    printf("=== Fixed duration: %.1f s (buffer=%zu bytes) ===\n", dur, gBufSize);
    printf("Recording... make noise to verify audio capture\n");

    std::vector<uint8_t> buf(gBufSize);
    std::vector<int16_t> samples;
    std::vector<double> readMs;
    int nReads = 0;
    std::atomic<bool> stopFlag{false};

    auto tStart = Clock::now();

    std::thread recThread([&]() {
        while (!stopFlag) {
            int err = 0;
            auto t0 = Clock::now();
            if (pa_simple_read(pa, buf.data(), buf.size(), &err) < 0) {
                fprintf(stderr, "  read err: %s\n", pa_strerror(err));
                break;
            }
            auto t1 = Clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            readMs.push_back(ms);

            auto *p = reinterpret_cast<int16_t *>(buf.data());
            samples.insert(samples.end(), p, p + buf.size() / 2);
            nReads++;
        }
    });

    // 等待指定时间
    std::this_thread::sleep_for(std::chrono::microseconds((long long)(dur * 1e6)));
    auto tStop = Clock::now();

    printf("\nStopping...\n");
    stopFlag = true;
    recThread.join();
    auto tDone = Clock::now();

    pa_simple_free(pa);

    double wallSec = std::chrono::duration<double>(tStop - tStart).count();
    double audioSec = (double)samples.size() / 16000.0;
    double stopToDone = std::chrono::duration<double, std::milli>(tDone - tStop).count();

    // 计算期望 samples
    size_t expectedSamples = (size_t)(wallSec * 16000);
    long long diffSamples = (long long)samples.size() - (long long)expectedSamples;

    printf("\n=== Results ===\n");
    printf("Wall time:       %.3f s\n", wallSec);
    printf("Expected samples: %zu\n", expectedSamples);
    printf("Actual samples:   %zu\n", samples.size());
    printf("Diff:             %lld samples (%.0f ms)\n", diffSamples, diffSamples / 16.0);
    printf("Audio duration:   %.3f s\n", audioSec);
    printf("Stop→done delay:  %.0f ms\n", stopToDone);
    printf("N reads:          %d\n", nReads);

    if (readMs.size() > 0) {
        double minMs = 1e9, maxMs = 0, sum = 0;
        for (auto d : readMs) {
            sum += d;
            if (d < minMs) minMs = d;
            if (d > maxMs) maxMs = d;
        }
        printf("Read times:       min=%.0f max=%.0f avg=%.0f ms\n",
               minMs, maxMs, sum / readMs.size());
    }

    // 判断
    if (audioSec >= wallSec - 0.05) {
        printf("Verdict: ✓ Audio COMPLETE (audio >= wall - 50ms)\n");
    } else if (audioSec >= wallSec - 0.5) {
        printf("Verdict: ~ Minor loss (%.0f ms) - likely acceptable\n", (wallSec - audioSec) * 1000);
    } else {
        printf("Verdict: ✗ LOSS DETECTED - missing %.3f s (%.0f ms) of audio!\n",
               wallSec - audioSec, (wallSec - audioSec) * 1000);
    }

    writeWav(path, samples);
    printf("WAV saved: %s\n", path);
    printf("\nCheck: ffprobe %s\n", path);

    // 扫频建议
    printf("\nRun sweep test: %s -s\n", argv[0]);

    return 0;
}
