#pragma once

#include <functional>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

namespace ainput {

using AsrResultCallback = std::function<void(const std::string &text, bool isFinal)>;

// Error categories reported alongside the detail message. Providers classify
// at the point of failure where the context is known; the adapter maps each
// category to an actionable status-panel string and only falls back to the
// raw detail for Unknown.
enum class AsrErrorCategory {
    ConfigMissing,      // api_key / resource_id not configured
    ConfigInvalid,      // config file failed schema/syntax validation
    AuthRejected,       // service rejected the key (401/403/InvalidApiKey)
    ModelNotFound,      // cloud model id unknown, or local model files missing
    LocalSetup,         // local engine misconfigured (binary missing, spawn/pipe)
    InvalidRequest,     // request rejected for other reasons (bad params, 4xx)
    Network,            // transport failure
    ServiceUnavailable, // 429/5xx or service rejected submit
    Timeout,            // recognition timed out
    NoSpeech,           // service detected silence
    EmptyResult,        // recognition finished but produced no text
    AudioData,          // recorded WAV unreadable/empty (internal pipeline)
    Runtime,            // unexpected local failure (curl init, waitpid...)
    Unknown,            // last resort; detail message is shown
};

using AsrErrorCallback = std::function<void(const std::string &error, AsrErrorCategory category)>;

inline void joinAsrWorker(std::thread &worker) {
    if (!worker.joinable()) return;
    if (worker.get_id() == std::this_thread::get_id()) {
        worker.detach();
    } else {
        worker.join();
    }
}

template<typename Task>
inline void startAsrWorker(std::thread &worker, Task &&task) {
    auto ready = std::make_shared<std::latch>(1);
    worker = std::thread(
        [ready, task = std::forward<Task>(task)]() mutable {
            ready->wait();
            task();
        });
    ready->count_down();
}

class IAsrProvider {
public:
    virtual ~IAsrProvider() = default;

    virtual void transcribe(std::vector<int16_t> samples, const std::string &wavPath) = 0;

    virtual void setConfig(const std::string &key, const std::string &value) { (void)key; (void)value; }
    virtual void setDiagnosticId(uint64_t id) { diagnosticId_ = id; }

    // Asynchronous providers invoke callbacks on their worker thread. Callbacks
    // must hand UI work to the host event loop.
    void setResultCallback(AsrResultCallback cb) { onResult_ = std::move(cb); }
    void setErrorCallback(AsrErrorCallback cb) { onError_ = std::move(cb); }

protected:
    AsrResultCallback onResult_;
    AsrErrorCallback onError_;
    uint64_t diagnosticId_ = 0;
};

class IAsrProviderFactory {
public:
    virtual ~IAsrProviderFactory() = default;
    virtual std::string id() const = 0;
    virtual std::string name() const = 0;
    // Detailed label shown in the switch notification (Ctrl+CapsLock ←/→).
    // Default is name(); factories whose model comes from a config file
    // override this to include the currently configured model ID so the
    // notification reflects what will actually run.
    virtual std::string displayName() const { return name(); }
    virtual std::unique_ptr<IAsrProvider> create() = 0;
};

class AsrProviderRegistry {
public:
    static AsrProviderRegistry &instance();

    void registerFactory(std::unique_ptr<IAsrProviderFactory> factory);
    std::vector<std::pair<std::string, std::string>> listFactories() const;
    std::unique_ptr<IAsrProvider> create(const std::string &id) const;

private:
    AsrProviderRegistry() = default;
    std::vector<std::unique_ptr<IAsrProviderFactory>> factories_;
};

} // namespace ainput
