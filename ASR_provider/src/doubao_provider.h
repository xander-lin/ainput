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

// Resolved from doubao.json (+ setConfig overrides) on the worker thread
// before each request. Re-read per request so model/feature switches need no
// restart. api_key is a plaintext field in doubao.json (file mode 0600
// recommended).
struct DoubaoSettings {
    std::string apiKey;    // resolved key (override > file)
    std::string resourceId;
    std::string modelName = "bigmodel";  // request.model_name
    bool enableItn = true;
    bool enablePunc = true;
    long timeout = 90;           // whole-recognition budget (timeout_sec):
                                // submit + polling share one deadline
    std::string configError;     // fatal doubao.json issue (syntax error)
};

class DoubaoAsrProvider : public IAsrProvider {
public:
    DoubaoAsrProvider();
    ~DoubaoAsrProvider() override;

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
                                 const DoubaoSettings &settings,
                                 std::shared_ptr<std::atomic_bool> cancel,
                                  AsrResultCallback onR, AsrErrorCallback onE,
                                  uint64_t diagnosticId);

    DoubaoSettings resolveSettings();

    std::string apiKeyOverride_;     // set via setConfig; wins over everything
    std::string resourceIdOverride_;
    std::shared_ptr<WorkerState> state_;
    std::thread worker_;
};

class DoubaoAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "doubao"; }
    std::string name() const override { return "Doubao (ByteDance)"; }
    // "Doubao · <model_name from doubao.json>"
    std::string displayName() const override;
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace ainput
