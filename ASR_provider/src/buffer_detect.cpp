#include "buffer_detect.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <vector>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

using Clock = std::chrono::steady_clock;

namespace ainput {

struct BufferCacheEntry {
    std::string sourceId;
    size_t bufferBytes;
};

static std::string configPath() {
    const char *overridePath = getenv("AINPUT_PA_BUFFER_CONFIG");
    if (overridePath && *overridePath) return overridePath;

    // Auto-detected device cache, not user configuration — lives in the cache
    // dir so ~/.config/ainput holds only files a user is meant to edit.
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.cache/ainput";
    mkdir(dir.c_str(), 0755);
    return dir + "/pa_buffer.json";
}

static std::string jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

static size_t parseBufferBytesAt(const std::string &content, size_t pos) {
    pos = content.find("\"buffer_bytes\"", pos);
    if (pos == std::string::npos) return 0;
    pos = content.find(':', pos);
    if (pos == std::string::npos) return 0;
    while (++pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n')) {}
    if (pos >= content.size()) return 0;

    try {
        size_t val = (size_t)std::stoul(content.substr(pos));
        if (val < 4096 || val > 1048576) return 0;  // invalid range
        return val;
    } catch (...) {
        return 0;
    }
}

static std::string readTextFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return content;
}

static std::vector<BufferCacheEntry> parseDeviceEntries(const std::string &content) {
    std::vector<BufferCacheEntry> entries;
    auto devicesPos = content.find("\"devices\"");
    if (devicesPos == std::string::npos) return entries;

    auto pos = content.find('{', devicesPos);
    if (pos == std::string::npos) return entries;
    ++pos;

    while (pos < content.size()) {
        pos = content.find('"', pos);
        if (pos == std::string::npos) break;
        auto end = content.find('"', pos + 1);
        if (end == std::string::npos) break;

        std::string sourceId = content.substr(pos + 1, end - pos - 1);
        size_t value = parseBufferBytesAt(content, end);
        if (value > 0) entries.push_back({sourceId, value});

        pos = content.find('}', end);
        if (pos == std::string::npos) break;
        ++pos;
    }
    return entries;
}

static std::string defaultSourceId() {
    const char *overrideId = getenv("AINPUT_PA_SOURCE_ID");
    if (overrideId && *overrideId) return overrideId;

    FILE *pipe = popen("pactl get-default-source 2>/dev/null", "r");
    if (!pipe) return "default";

    char buf[256];
    std::string source;
    if (fgets(buf, sizeof(buf), pipe)) source = buf;
    pclose(pipe);

    while (!source.empty() && (source.back() == '\n' || source.back() == '\r')) {
        source.pop_back();
    }
    return source.empty() ? "default" : source;
}

static size_t loadFromConfig(const std::string &sourceId, bool *usedLegacy = nullptr) {
    if (usedLegacy) *usedLegacy = false;

    std::string content = readTextFile(configPath());
    if (content.empty()) return 0;

    for (const auto &entry : parseDeviceEntries(content)) {
        if (entry.sourceId == sourceId) return entry.bufferBytes;
    }

    // Migrate the old single-device cache only when no per-device section exists.
    if (content.find("\"devices\"") == std::string::npos) {
        size_t legacy = parseBufferBytesAt(content, 0);
        if (legacy > 0 && usedLegacy) *usedLegacy = true;
        return legacy;
    }
    return 0;
}

static void saveToConfig(size_t bytes, const std::string &sourceId) {
    std::map<std::string, size_t> devices;
    for (const auto &entry : parseDeviceEntries(readTextFile(configPath()))) {
        devices[entry.sourceId] = entry.bufferBytes;
    }
    devices[sourceId] = bytes;

    std::ofstream f(configPath());
    if (!f.is_open()) {
        fprintf(stderr, "Ainput: failed to write %s\n", configPath().c_str());
        return;
    }
    f << "{\n"
      << "  \"devices\": {\n";
    for (auto it = devices.begin(); it != devices.end(); ++it) {
        f << "    \"" << jsonEscape(it->first) << "\": {\"buffer_bytes\": " << it->second << "}";
        if (std::next(it) != devices.end()) f << ",";
        f << "\n";
    }
    f << "  }\n"
      << "}\n";
    f.close();
    fprintf(stderr, "Ainput: saved buffer=%zu for source=%s to %s\n",
            bytes, sourceId.c_str(), configPath().c_str());
}

size_t detectHardwareBurstBytes() {
    fprintf(stderr, "Ainput: detecting hardware buffer period...\n");

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto *pa = pa_simple_new(nullptr, "ainput-detect", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "Ainput: PA detection error: %s, using default 16384\n", pa_strerror(error));
        return 16384;
    }

    // 200 字节 = 100 samples @16kHz S16LE, 精确 6.25ms 精度
    uint8_t buf[200];
    constexpr size_t kDetectBufSize = sizeof(buf);
    std::vector<double> readMs;

    auto tEnd = Clock::now() + std::chrono::seconds(6);
    fprintf(stderr, "Ainput: recording 6s for detection...\n");

    while (Clock::now() < tEnd) {
        int err = 0;
        auto t0 = Clock::now();
        if (pa_simple_read(pa, buf, kDetectBufSize, &err) < 0) {
            fprintf(stderr, "Ainput: detection read error: %s\n", pa_strerror(err));
            break;
        }
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        readMs.push_back(ms);
    }

    pa_simple_free(pa);

    if (readMs.size() < 3) {
        fprintf(stderr, "Ainput: too few reads (%zu), using default 16384\n", readMs.size());
        return 16384;
    }

    // 自适应判定中断周期:
    // 排序读耗时 → 找相邻最大 gap → 快/慢聚类天然分界
    // gap = sorted[i+1] - sorted[i], maxGap 把数据切成快慢两簇
    // maxGap < 50ms(或占比较小) → 连续流 → 默认 16384
    std::vector<double> sorted = readMs;
    std::sort(sorted.begin(), sorted.end());

    size_t gapIdx = 0;
    double maxGap = 0.0;
    for (size_t i = 0; i + 1 < sorted.size(); i++) {
        double gap = sorted[i + 1] - sorted[i];
        if (gap > maxGap) { maxGap = gap; gapIdx = i; }
    }

    // 连续流判定: gap 太小(全聚在一起) 或 gap 不是数量级跳变(仅时序抖动)
    double slowVal = sorted[gapIdx + 1];
    double fastVal = sorted[gapIdx];
    bool hasBurst = (maxGap > 50.0) && (slowVal > fastVal * 3.0);

    if (!hasBurst) {
        fprintf(stderr, "Ainput: continuous streaming (max_gap=%.0fms min=%.0f max=%.0f) → default 16384\n",
                maxGap, sorted.front(), sorted.back());
        return 16384;
    }

    double thresholdMs = fastVal + maxGap * 0.3;
    double fastBound = thresholdMs * 0.3;
    fprintf(stderr, "Ainput: burst pattern: fast=%.0fms slow=%.0fms gap=%.0fms threshold=%.0fms\n",
            fastVal, slowVal, maxGap, thresholdMs);

    // 分类: >threshold = slow (fill 期), <fastBound = fast (burst 期)
    // 每组 [slow + 后续 fast reads] = 一个硬件周期的完整数据量
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
            currentBurst = kDetectBufSize;
        } else if (readMs[i] < fastBound) {
            if (inBurst) {
                currentBurst += kDetectBufSize;
            } else {
                inBurst = true;
                currentBurst = kDetectBufSize;
            }
        }
    }
    if (inBurst && currentBurst > 0) {
        burstSizes.push_back(currentBurst);
    }

    if (burstSizes.empty()) {
        fprintf(stderr, "Ainput: no burst pattern, using default 16384\n");
        return 16384;
    }

    // 取第一个完整 burst（通常最早记录的 cycle 最准，后续可能受干扰）
    // 但考虑到时序抖动，取最大 burst 做保守估计
    size_t maxBurst = 0;
    fprintf(stderr, "Ainput: detected bursts: ");
    for (size_t i = 0; i < burstSizes.size(); i++) {
        fprintf(stderr, "%zu%s", burstSizes[i], i + 1 < burstSizes.size() ? ", " : "");
        if (burstSizes[i] > maxBurst) maxBurst = burstSizes[i];
    }
    fprintf(stderr, "\n");

    // 向下取整到 1000 的倍数（留安全边际，避免缓冲区略大于 burst）
    size_t result = (maxBurst / 1000) * 1000;
    if (result < 4096) result = 4096;
    if (result > 1048576) result = 1048576;

    fprintf(stderr, "Ainput: buffer set to %zu bytes (%.1f s @16kHz)\n",
            result, result / 32000.0);
    return result;
}

size_t loadOrDetectBufferBytes(std::function<void(const std::string &)> onStatus) {
    std::string sourceId = defaultSourceId();
    bool usedLegacy = false;
    size_t cached = loadFromConfig(sourceId, &usedLegacy);
    if (cached > 0) {
        if (usedLegacy) saveToConfig(cached, sourceId);
        fprintf(stderr, "Ainput: using cached buffer=%zu bytes (%.1f s) for source=%s\n",
                cached, cached / 32000.0, sourceId.c_str());
        return cached;
    }

    if (onStatus) onStatus("Detecting hardware buffer period...");
    size_t detected = detectHardwareBurstBytes();
    saveToConfig(detected, sourceId);
    if (onStatus) onStatus("");
    return detected;
}

} // namespace ainput
