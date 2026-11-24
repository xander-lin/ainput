#pragma once

#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
/// The input panel is the primary channel. A displayed running status
/// ("listening...", "recognizing...") is not preemptible; a terminal outcome
/// from another dictation goes to ReportError while it is displayed or waiting
/// for its turn. Other panel messages can replace one another, but each stays
/// visible for at least 100 ms.
class OutputHandler {
public:
    /// Called for an outcome that cannot be shown because a running status
    /// owns or is waiting for the panel. The addon routes it to a notification.
    using ReportError = std::function<void(const std::string &)>;

    OutputHandler(fcitx::Instance *instance, ReportError reportError);
    ~OutputHandler();

    OutputHandler(const OutputHandler &) = delete;
    OutputHandler &operator=(const OutputHandler &) = delete;

    /// Remembers which input context started a recording. Event loop thread.
    void rememberTarget(uint64_t recognitionId);

    void submit(uint64_t recognitionId, const std::string &text);
    /// Terminal outcome without text: shown in the input panel, or reported
    /// through ReportError when a running status owns the panel.
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
    enum class PanelKind { Running, Preemptible };
    struct Pending {
        std::string text;
        Kind kind;
        uint64_t recognitionId;
    };

    struct PanelMessage {
        std::string text;
        PanelKind kind;
        uint64_t recognitionId;
    };

    struct PanelAction {
        std::optional<PanelMessage> message;
        uint64_t recognitionId;
        bool ownerOnly = false;
    };

    struct PanelState {
        std::optional<PanelMessage> current;
        uint64_t shownAt = 0;
        std::deque<PanelAction> pending;
    };

    static constexpr uint64_t kMinimumPanelUsec = 100'000;
    std::mutex pendingMutex_;
    std::vector<Pending> pending_;

    using Results = DeliveryQueue<fcitx::ICUUID, Pending>;
    Results targets_;
    std::map<fcitx::ICUUID, PanelState> panels_;
    std::unique_ptr<fcitx::EventSourceTime> panelTimer_;

    void enqueue(Pending pending);
    void drainAndCommit();
    void commitPending(const Results::Entry &entry);
    void showStatusNow(const fcitx::ICUUID &uuid, const std::string &text);
    bool requestPanel(const fcitx::ICUUID &uuid, PanelMessage message);
    void retireRunning(const fcitx::ICUUID &uuid, uint64_t recognitionId);
    void requestClear(const fcitx::ICUUID &uuid, uint64_t recognitionId);
    void processPanelQueue();
    void armPanelTimer(uint64_t delayUsec);
    uint64_t nextPanelDelay(uint64_t now) const;
    void displayPanel(const fcitx::ICUUID &uuid, PanelState &state,
                      PanelMessage message, uint64_t now);
    void wake();
};

} // namespace ainput
