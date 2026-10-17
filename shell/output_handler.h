#pragma once

#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ainput {

/// Delivery stages, mirrored from the core's AINPUT_STAGE_*.
enum class Stage {
    Recognizing = 1,
};

/// Delivers text and status from arbitrary threads back into the fcitx5 event
/// loop through a self-pipe.
///
/// Threading: submit()/showStatus()/showStage() may be called from any thread
/// and only touch the pending queue. Every fcitx5 call — resolving the target
/// input context, touching the input panel, committing text — happens inside
/// drainAndCommit() on the event loop thread. Callers must not resolve input
/// contexts themselves: mostRecentInputContext() and the context manager are
/// not thread-safe.
///
/// A result whose original input context is gone falls back to the currently
/// focused context, and is logged rather than dropped silently: the user just
/// finished speaking and expects the text to appear somewhere they can see.
class OutputHandler {
public:
    explicit OutputHandler(fcitx::Instance *instance);
    ~OutputHandler();

    OutputHandler(const OutputHandler &) = delete;
    OutputHandler &operator=(const OutputHandler &) = delete;

    /// Remembers which input context started a recording. Event loop thread.
    void rememberTarget(uint64_t recognitionId);

    void submit(uint64_t recognitionId, const std::string &text);
    void showStatus(uint64_t recognitionId, const std::string &text);
    void showStage(uint64_t recognitionId, Stage stage);

    /// Shows a message about the addon itself (no recording attached).
    void showGlobalStatus(const std::string &text);

private:
    fcitx::Instance *instance_;

    int wakePipe_[2] = {-1, -1};
    std::unique_ptr<fcitx::EventSourceIO> wakeWatcher_;

    struct Pending {
        std::string text;
        bool isStatus = false;
        uint64_t recognitionId = 0;
        // Status messages from an older recording must not overwrite a newer
        // one; recorded when the message is enqueued.
        uint64_t sequence = 0;
        fcitx::ICUUID explicitUuid = {};
        std::function<void()> completion;
    };
    std::mutex pendingMutex_;
    std::vector<Pending> pending_;

    /// recognition id -> uuid captured when recording started. Event loop only.
    std::unordered_map<uint64_t, fcitx::ICUUID> targets_;
    uint64_t latestStatusSequence_ = 0;

    void enqueue(Pending pending);
    void drainAndCommit();
    void commitPending(Pending pending);
    void showStatusNow(const fcitx::ICUUID &uuid, const std::string &text,
                       uint64_t sequence);
    fcitx::ICUUID resolve(const Pending &pending);
    void wake();
};

} // namespace ainput
