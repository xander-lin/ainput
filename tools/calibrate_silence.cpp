// 静音/背景噪声阀值标定工具 (含响度归一化，复现 processSamples 管线)
// 编译: c++ -std=c++20 calibrate_silence.cpp -o calibrate_silence -lpulse-simple -lpulse -lebur128
// 运行: ./calibrate_silence [录音秒数=5]
// 在安静环境中运行此程序，测量背景噪声的 peak 和 crest factor，给出阀值建议。
#include <pulse/simple.h>
#include <pulse/error.h>
#include <ebur128.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <cstdint>
#include <chrono>
#include <algorithm>

using Clock = std::chrono::steady_clock;

struct FrameStats {
    int32_t peak;
    double rms;
    double crest; // peak / rms
};

// 与 audio_capture.cpp:353-386 完全一致的响度归一化逻辑
static double normalizeSamples(std::vector<int16_t> &samples, double lufsTarget = -16.0) {
    ebur128_state *ebur = ebur128_init(1, 16000, EBUR128_MODE_I);
    ebur128_add_frames_short(ebur, samples.data(), samples.size());
    double loudness = 0.0;
    ebur128_loudness_global(ebur, &loudness);
    ebur128_destroy(&ebur);

    double gain;
    if (loudness < -70.0 || !std::isfinite(loudness)) {
        printf("  响度归一化: 音频过弱 (%.1f LUFS), 跳过归一化 (gain=1.0)\n\n", loudness);
        gain = 1.0;
    } else {
        gain = std::pow(10.0, (lufsTarget - loudness) / 20.0);
        printf("  响度归一化: loudness=%.1f LUFS, target=%.1f LUFS, gain=%.3f\n\n",
               loudness, lufsTarget, gain);
    }

    for (auto &s : samples) {
        double vd = static_cast<double>(s) * gain;
        if (vd > 32767.0) vd = 32767.0;
        else if (vd < -32768.0) vd = -32768.0;
        s = static_cast<int16_t>(static_cast<int32_t>(vd));
    }

    return loudness;
}

int main(int argc, char **argv) {
    int duration = 5;
    if (argc > 1) duration = atoi(argv[1]);
    if (duration < 2) duration = 2;

    printf("=== 静音/背景噪声阀值标定 (含响度归一化) ===\n");
    printf("请确保当前处于安静环境，不要说话。\n");
    printf("录音时长: %d 秒\n\n", duration);

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto *pa = pa_simple_new(nullptr, "calibrate-silence", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "PulseAudio 错误: %s\n", pa_strerror(error));
        return 1;
    }

    printf("正在录音...");
    fflush(stdout);

    std::vector<int16_t> samples;
    uint8_t buf[4096];
    auto t0 = Clock::now();
    auto end = t0 + std::chrono::seconds(duration);

    while (Clock::now() < end) {
        if (pa_simple_read(pa, buf, sizeof(buf), &error) < 0) {
            fprintf(stderr, "\n读取错误: %s\n", pa_strerror(error));
            break;
        }
        auto *p = reinterpret_cast<int16_t *>(buf);
        samples.insert(samples.end(), p, p + sizeof(buf) / 2);
    }

    auto t1 = Clock::now();
    pa_simple_free(pa);

    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    double secs = (double)samples.size() / 16000.0;
    printf(" 完成 (%.1f 秒实际, %.1f 秒音频, %zu 采样点)\n",
           elapsed, secs, samples.size());

    if (samples.empty()) {
        fprintf(stderr, "没有录音数据\n");
        return 1;
    }

    // === 归一化前统计 ===
    printf("══════ 归一化前 ══════\n");
    {
        int32_t prePeak = 0;
        double preSumSq = 0;
        for (auto s : samples) {
            int32_t a = s >= 0 ? (int32_t)s : -(int32_t)s;
            if (a > prePeak) prePeak = a;
            preSumSq += (double)s * (double)s;
        }
        double preRms = std::sqrt(preSumSq / samples.size());
        printf("  peak=%d  RMS=%.1f  crest=%.1f\n",
               (int)prePeak, preRms, (double)prePeak / (preRms > 0 ? preRms : 1));
    }

    // === 响度归一化 (模拟 processSamples 第一步) ===
    normalizeSamples(samples);

    // === 归一化后统计 ===
    printf("══════ 归一化后整体统计 ══════\n");

    int32_t globalPeak = 0;
    double sumSq = 0;
    double sumAbs = 0;
    int16_t minVal = samples[0], maxVal = samples[0];
    for (auto s : samples) {
        int32_t a = s >= 0 ? (int32_t)s : -(int32_t)s;
        if (a > globalPeak) globalPeak = a;
        sumSq += (double)s * (double)s;
        sumAbs += a;
        if (s < minVal) minVal = s;
        if (s > maxVal) maxVal = s;
    }
    double globalRms = std::sqrt(sumSq / samples.size());
    double globalCrest = (double)globalPeak / (globalRms > 0 ? globalRms : 1);
    double avgAbs = sumAbs / samples.size();

    printf("  整体 peak:   %d     (采样值, 绝对值)\n", (int)globalPeak);
    printf("  整体 RMS:    %.1f   (采样值)\n", globalRms);
    printf("  整体 crest:  %.1f   (peak / RMS)\n", globalCrest);
    printf("  整体 avg_abs: %.1f   (平均绝对值)\n", avgAbs);
    printf("  采样范围:    [%d, %d]\n\n", (int)minVal, (int)maxVal);

    // === 分帧统计 (20ms frame, 320 samples @16kHz) ===
    printf("══════ 归一化后分帧统计 (20ms/帧, %d 采样点/帧) ══════\n", 320);

    constexpr int kFrameSize = 320;
    size_t frameCount = samples.size() / kFrameSize;
    std::vector<FrameStats> frames;
    frames.reserve(frameCount);

    for (size_t i = 0; i < frameCount; i++) {
        const int16_t *p = samples.data() + i * kFrameSize;
        int32_t peak = 0;
        double sqSum = 0;
        for (int j = 0; j < kFrameSize; j++) {
            int32_t a = p[j] >= 0 ? (int32_t)p[j] : -(int32_t)p[j];
            if (a > peak) peak = a;
            sqSum += (double)p[j] * (double)p[j];
        }
        double rms = std::sqrt(sqSum / kFrameSize);
        frames.push_back({peak, rms, (double)peak / (rms > 0 ? rms : 1)});
    }

    std::sort(frames.begin(), frames.end(),
              [](const FrameStats &a, const FrameStats &b) { return a.peak < b.peak; });

    printf("  帧数: %zu\n", frameCount);

    int32_t minPeak = frames.front().peak;
    int32_t maxPeak = frames.back().peak;
    int32_t p50Peak = frames[frameCount / 2].peak;
    int32_t p90Peak = frames[frameCount * 90 / 100].peak;
    int32_t p95Peak = frames[frameCount * 95 / 100].peak;
    int32_t p99Peak = frames[frameCount * 99 / 100].peak;
    double avgPeak = 0;
    for (auto &f : frames) avgPeak += f.peak;
    avgPeak /= frameCount;

    printf("  帧 peak 最小值:  %d\n", (int)minPeak);
    printf("  帧 peak 最大值:  %d\n", (int)maxPeak);
    printf("  帧 peak 平均值:  %.1f\n", avgPeak);
    printf("  帧 peak P50:     %d\n", (int)p50Peak);
    printf("  帧 peak P90:     %d\n", (int)p90Peak);
    printf("  帧 peak P95:     %d\n", (int)p95Peak);
    printf("  帧 peak P99:     %d\n", (int)p99Peak);

    std::sort(frames.begin(), frames.end(),
              [](const FrameStats &a, const FrameStats &b) { return a.crest < b.crest; });

    double minCrest = frames.front().crest;
    double maxCrest = frames.back().crest;
    double p50Crest = frames[frameCount / 2].crest;
    double p90Crest = frames[frameCount * 90 / 100].crest;
    double p95Crest = frames[frameCount * 95 / 100].crest;
    double p99Crest = frames[frameCount * 99 / 100].crest;
    double avgCrest = 0;
    for (auto &f : frames) avgCrest += f.crest;
    avgCrest /= frameCount;

    printf("\n  帧 crest 最小值:  %.2f\n", minCrest);
    printf("  帧 crest 最大值:  %.2f\n", maxCrest);
    printf("  帧 crest 平均值:  %.2f\n", avgCrest);
    printf("  帧 crest P50:     %.2f\n", p50Crest);
    printf("  帧 crest P90:     %.2f\n", p90Crest);
    printf("  帧 crest P95:     %.2f\n", p95Crest);
    printf("  帧 crest P99:     %.2f\n", p99Crest);

    // === 阀值建议 ===
    printf("\n══════ 阀值建议 ══════\n");
    printf("\n当前代码中的阀值:\n");
    printf("  crest < %.1f     → 判为静音 (配置项 crest_threshold, 默认 2.4)\n", 2.4);

    printf("\n根据本环境实测数据 (归一化后)，建议的安全阀值:\n");
    double safeCrest = p99Crest * 1.3;
    printf("  crest_threshold ≥ %.1f   (P99 * 1.3, 确保静音判定为静音)\n\n", safeCrest);

    printf("══════ 分析 ══════\n");
    printf("\n如果当前阀值在本环境不合适:\n");
    printf("  - 背景噪声被误判为语音 → 提高 crest_threshold\n");
    printf("  - 实际语音被误判为静音 → 降低 crest_threshold\n");
    printf("\n调整方法: 编辑 ~/.config/ainput/audio.json 中 crest_threshold 的值\n");

    return 0;
}
