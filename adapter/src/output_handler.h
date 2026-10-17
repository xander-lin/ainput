#pragma once

#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ainput {

struct OutputTarget {
    fcitx::ICUUID uuid = {};
    uint64_t statusSequence = 0;
    uint64_t recognitionId = 0;
};

class OutputHandler {
public:
    explicit OutputHandler(fcitx::Instance *instance);
    ~OutputHandler();

    OutputHandler(const OutputHandler &) = delete;
    OutputHandler &operator=(const OutputHandler &) = delete;

    // Thread-safe: submit ASR result text for display
    void submit(const OutputTarget &target, const std::string &text,
                std::function<void()> onCommitted = {});

    // Thread-safe: show transient status text
    void showStatus(const OutputTarget &target, const std::string &text,
                    std::function<void()> onShown = {});

    // Capture the current input context uuid for result delivery
    OutputTarget captureCurrentUuid(uint64_t recognitionId = 0);

private:
    fcitx::Instance *instance_;

    int wakePipe_[2] = {-1, -1};
    std::unique_ptr<fcitx::EventSourceIO> wakeWatcher_;

    struct Pending {
        std::string text;
        bool isStatus = false;
        fcitx::ICUUID targetUuid = {};
        uint64_t statusSequence = 0;
        uint64_t recognitionId = 0;
        std::function<void()> completion;
    };
    std::mutex pendingMutex_;
    std::vector<Pending> pending_;

    uint64_t latestStatusSequence_ = 0;

    void enqueue(const OutputTarget &target, const std::string &text,
                 bool isStatus, std::function<void()> completion);
    void drainAndCommit();
    void commitPending(Pending pending, const char *label);
    void wake();
};

} // namespace ainput
