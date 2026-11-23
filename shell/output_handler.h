#pragma once

#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "delivery_queue.h"

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
/// A result is delivered only to the original input context. If that context
/// is gone, the drop is logged without recording the text.
///
/// The input panel is the primary channel and belongs to the newest
/// dictation of its input context: its live status ("listening...",
/// "recognizing...") must not be overwritten. An older dictation's terminal
/// outcome that arrives while a newer one is outstanding therefore goes to
/// ReportError instead of the panel.
class OutputHandler {
public:
    /// Called for a terminal outcome that cannot be shown in the input panel
    /// because a newer dictation in the same input context owns it. The addon
    /// routes it to the desktop notification.
    using ReportError = std::function<void(const std::string &)>;

    OutputHandler(fcitx::Instance *instance, ReportError reportError);
    ~OutputHandler();

    OutputHandler(const OutputHandler &) = delete;
    OutputHandler &operator=(const OutputHandler &) = delete;

    /// Remembers which input context started a recording. Event loop thread.
    void rememberTarget(uint64_t recognitionId);

    void submit(uint64_t recognitionId, const std::string &text);
    /// Terminal outcome without text: shown in the input panel, or reported
    /// through ReportError when a newer dictation owns the panel.
    void fail(uint64_t recognitionId, const std::string &text);
    void showStatus(uint64_t recognitionId, const std::string &text);
    void showStage(uint64_t recognitionId, Stage stage);

    /// Shows a message about the addon itself (no recording attached).
    void showGlobalStatus(const std::string &text);

    // Event-loop thread only. Includes completed results waiting for their turn.
    size_t pendingCount() const { return targets_.size(); }
    void discardAll();

private:
    fcitx::Instance *instance_;
    ReportError reportError_;

    int wakePipe_[2] = {-1, -1};
    std::unique_ptr<fcitx::EventSourceIO> wakeWatcher_;

    enum class Kind { Status, Text, Error };
    struct Pending {
        std::string text;
        Kind kind;
        uint64_t recognitionId;
    };
    std::mutex pendingMutex_;
    std::vector<Pending> pending_;

    using Results = DeliveryQueue<fcitx::ICUUID, Pending>;
    Results targets_;

    void enqueue(Pending pending);
    void drainAndCommit();
    void commitPending(const Results::Entry &entry);
    void showStatusNow(const fcitx::ICUUID &uuid, const std::string &text);
    void wake();
};

} // namespace ainput
