#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include "asr_provider.h"

namespace ainput {

// Everything the Qwen request needs, resolved from qwen.json (+ setConfig
// overrides) on the worker thread right before each request. The config file
// is re-read per request so model switches need no restart or recompile.
// api_key is a plaintext field in qwen.json (file mode 0600 recommended).
struct QwenSettings {
    std::string apiKey;             // resolved key (override > file)
    std::string model;
    std::string endpoint;
    std::string languageHintsRaw;   // raw JSON array, e.g. ["zh","en"]; empty = auto
    std::string vocabularyRaw;      // raw JSON object {word: weight}; empty = none
    std::string vocabularyId;       // precompiled hotword list id; empty = none
    bool keepDialect = false;       // qwen-audio-3.1-asr-flash: keep dialect text
    bool speakerDiarization = false; // qwen-audio-3.1-asr-flash only
    long timeout = 60;              // whole-request curl timeout (timeout_sec)
    std::string configError;        // fatal qwen.json issue (syntax error)
};

class QwenAsrProvider : public IAsrProvider {
public:
    QwenAsrProvider();
    ~QwenAsrProvider() override;

    void transcribe(std::vector<int16_t> samples, const std::string &wavPath) override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    struct Task {
        std::vector<int16_t> samples;
        std::string wavPath;
        std::shared_ptr<std::atomic_bool> cancel;
        AsrResultCallback onResult;
        AsrErrorCallback onError;
        uint64_t diagnosticId;
    };
    struct WorkerState {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Task> tasks;
        std::shared_ptr<std::atomic_bool> activeCancel;
        bool stopping = false;
    };

    void workerLoop(const std::shared_ptr<WorkerState> &state);
    static void processRecording(std::vector<int16_t> samples,
                                 const std::string &wavPath,
                                 const QwenSettings &settings,
                                 std::shared_ptr<std::atomic_bool> cancel,
                                 AsrResultCallback onR, AsrErrorCallback onE,
                                 uint64_t diagnosticId);

    QwenSettings resolveSettings();

    std::string apiKeyOverride_;   // set via setConfig; wins over everything
    std::shared_ptr<WorkerState> state_;
    std::thread worker_;
};

class QwenAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "qwen"; }
    std::string name() const override { return "Qwen ASR (Alibaba DashScope)"; }
    // "Qwen · <model from qwen.json>"
    std::string displayName() const override;
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace ainput
