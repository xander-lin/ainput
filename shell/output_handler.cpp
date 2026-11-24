#include "output_handler.h"

#include <fcitx/inputpanel.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/text.h>
#include <fcitx-utils/log.h>

#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <stdexcept>

namespace ainput {

namespace {
/// Status text shown for a stage. Kept here because the shell owns every
/// user-visible string.
const char *stageText(Stage stage) {
    switch (stage) {
    case Stage::Recognizing:
        return "Ainput: recognizing...";
    }
    return "Ainput: ...";
}
} // namespace

OutputHandler::OutputHandler(fcitx::Instance *instance, ReportError reportError)
    : instance_(instance), reportError_(std::move(reportError)) {
    if (pipe2(wakePipe_, O_NONBLOCK | O_CLOEXEC) != 0) {
        throw std::runtime_error("Ainput: OutputHandler pipe() failed");
    }

    wakeWatcher_ = instance_->eventLoop().addIOEvent(
        wakePipe_[0], fcitx::IOEventFlag::In,
        [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) -> bool {
            drainAndCommit();
            return true;
        });
    if (!wakeWatcher_) {
        close(wakePipe_[0]);
        close(wakePipe_[1]);
        wakePipe_[0] = wakePipe_[1] = -1;
        throw std::runtime_error("Ainput: failed to create output event source");
    }

    panelTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + kMinimumPanelUsec, 1'000,
        [this](fcitx::EventSourceTime *, uint64_t) {
            processPanelQueue();
            return true;
        });
    if (!panelTimer_) {
        wakeWatcher_.reset();
        close(wakePipe_[0]);
        close(wakePipe_[1]);
        wakePipe_[0] = wakePipe_[1] = -1;
        throw std::runtime_error("Ainput: failed to create panel timer");
    }
    panelTimer_->setEnabled(false);
}

OutputHandler::~OutputHandler() {
    wakeWatcher_.reset();
    if (wakePipe_[0] >= 0) close(wakePipe_[0]);
    if (wakePipe_[1] >= 0) close(wakePipe_[1]);
}

void OutputHandler::rememberTarget(uint64_t recognitionId) {
    // Event loop thread only.
    fcitx::ICUUID uuid = {};
    if (auto *ic = instance_->mostRecentInputContext()) {
        uuid = ic->uuid();
    }
    targets_.remember(recognitionId, uuid);
}

void OutputHandler::submit(uint64_t recognitionId, const std::string &text) {
    enqueue({text, Kind::Text, recognitionId});
}

void OutputHandler::fail(uint64_t recognitionId, const std::string &text) {
    if (recognitionId == 0) {
        showGlobalStatus(text);
    } else {
        enqueue({text, Kind::Error, recognitionId});
    }
}

void OutputHandler::showStatus(uint64_t recognitionId, const std::string &text) {
    enqueue({text, Kind::Status, recognitionId});
}

void OutputHandler::showStage(uint64_t recognitionId, Stage stage) {
    showStatus(recognitionId, stageText(stage));
}

void OutputHandler::showGlobalStatus(const std::string &text) {
    showStatus(0, text);
}

void OutputHandler::wake() {
    char c = 1;
    while (write(wakePipe_[1], &c, 1) < 0 && errno == EINTR) {}
}

void OutputHandler::enqueue(Pending pending) {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_.push_back(std::move(pending));
    }
    wake();
}

void OutputHandler::discardAll() {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_.clear();
    }
    targets_.discard();
    for (const auto &[uuid, state] : panels_) showStatusNow(uuid, "");
    panels_.clear();
    if (panelTimer_) panelTimer_->setEnabled(false);
}

void OutputHandler::drainAndCommit() {
    char buf[64];
    while (read(wakePipe_[0], buf, sizeof(buf)) > 0) {}

    std::vector<Pending> batch;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        batch.swap(pending_);
    }

    for (auto &pending : batch) {
        const auto id = pending.recognitionId;
        if (id == 0) {
            if (auto *ic = instance_->mostRecentInputContext()) {
                requestPanel(ic->uuid(),
                             {pending.text, PanelKind::Preemptible, 0});
            }
            continue;
        }
        const auto *target = targets_.find(id);
        if (!target) continue; // Cancelled or already delivered.
        if (pending.kind == Kind::Status) {
            if (!target->outcome && !targets_.hasNewer(id, target->context)) {
                requestPanel(target->context,
                             {pending.text, PanelKind::Running, id});
            }
            continue;
        }
        const auto context = target->context;
        retireRunning(context, id);
        for (const auto &entry : targets_.finish(id, std::move(pending))) commitPending(entry);
    }
}

void OutputHandler::showStatusNow(const fcitx::ICUUID &uuid,
                                  const std::string &text) {
    auto *ic = instance_->inputContextManager().findByUUID(uuid);
    if (!ic) {
        if (!text.empty()) {
            FCITX_INFO() << "Ainput [status] no input context for \"" << text
                         << "\"; not shown";
        }
        return;
    }
    ic->inputPanel().setAuxUp(fcitx::Text(text));
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void OutputHandler::commitPending(const Results::Entry &entry) {
    const auto &pending = *entry.outcome;
    if (pending.kind == Kind::Error) {
        const bool notified = requestPanel(
            entry.context, {pending.text, PanelKind::Preemptible, entry.id});
        FCITX_INFO() << "Ainput: id=" << entry.id
                     << " stage=" << (notified ? "error_notified" : "error_queued");
        return;
    }
    auto *ic = instance_->inputContextManager().findByUUID(entry.context);
    if (!ic) {
        FCITX_INFO() << "Ainput: id=" << entry.id << " stage=original_context_gone";
        return;
    }

    if (!pending.text.empty()) {
        ic->commitString(pending.text);
    }
    if (!targets_.hasNewer(entry.id, ic->uuid())) {
        requestClear(ic->uuid(), entry.id);
    }
    FCITX_INFO() << "Ainput: id=" << entry.id << " stage=committed text_len=" << pending.text.size();
}

bool OutputHandler::requestPanel(const fcitx::ICUUID &uuid, PanelMessage message) {
    auto &state = panels_[uuid];

    // Only the latest undisplayed stage for a dictation is useful. Completion
    // removes its pending stages in retireRunning(), before FIFO delivery.
    if (message.kind == PanelKind::Running) {
        std::erase_if(state.pending, [&](const PanelAction &action) {
            return action.message && action.message->kind == PanelKind::Running &&
                   action.message->recognitionId == message.recognitionId;
        });
    }

    // A running status owns the panel. A terminal outcome from that same
    // dictation may replace its own status, but an outcome from another one
    // must use the system notification instead of hiding the live status.
    const bool runningDisplayed = state.current &&
        state.current->kind == PanelKind::Running &&
        state.current->recognitionId != message.recognitionId;
    const bool runningQueued = std::any_of(
        state.pending.begin(), state.pending.end(), [&](const PanelAction &action) {
            return action.message && action.message->kind == PanelKind::Running &&
                   action.message->recognitionId != message.recognitionId;
        });
    if (message.kind == PanelKind::Preemptible && (runningDisplayed || runningQueued)) {
        reportError_(message.text);
        return true;
    }

    PanelAction action{std::move(message), 0};
    if (action.message->kind == PanelKind::Running) {
        state.pending.push_front(std::move(action));
    } else {
        state.pending.push_back(std::move(action));
    }
    processPanelQueue();
    return false;
}

void OutputHandler::retireRunning(const fcitx::ICUUID &uuid, uint64_t recognitionId) {
    auto it = panels_.find(uuid);
    if (it == panels_.end()) return;

    auto &state = it->second;
    const auto removed = std::erase_if(state.pending, [&](const PanelAction &action) {
        return action.message && action.message->kind == PanelKind::Running &&
               action.message->recognitionId == recognitionId;
    });
    bool changed = removed != 0;
    if (state.current && state.current->kind == PanelKind::Running &&
        state.current->recognitionId == recognitionId) {
        // The displayed text still observes its dwell time, but no longer
        // blocks outcomes while this result waits for FIFO delivery.
        state.current->kind = PanelKind::Preemptible;
        state.pending.push_back({std::nullopt, recognitionId, true});
        changed = true;
    }
    if (changed) processPanelQueue();
}

void OutputHandler::requestClear(const fcitx::ICUUID &uuid, uint64_t recognitionId) {
    auto it = panels_.find(uuid);
    if (it == panels_.end()) return;

    auto &state = it->second;
    if (state.current && state.current->kind == PanelKind::Running &&
        state.current->recognitionId != recognitionId) {
        // A result from another dictation must not clear the live status.
        return;
    }
    state.pending.push_back({std::nullopt, recognitionId});
    processPanelQueue();
}

void OutputHandler::displayPanel(const fcitx::ICUUID &uuid, PanelState &state,
                                 PanelMessage message, uint64_t now) {
    showStatusNow(uuid, message.text);
    state.current = std::move(message);
    state.shownAt = now;
}

void OutputHandler::processPanelQueue() {
    uint64_t now = fcitx::now(CLOCK_MONOTONIC);
    for (auto it = panels_.begin(); it != panels_.end();) {
        const auto &uuid = it->first;
        auto &state = it->second;
        if (!instance_->inputContextManager().findByUUID(uuid)) {
            it = panels_.erase(it);
            continue;
        }
        while (!state.pending.empty()) {
            if (state.current && now - state.shownAt < kMinimumPanelUsec) break;

            PanelAction action = std::move(state.pending.front());
            state.pending.pop_front();
            if (!action.message) {
                if (!state.current ||
                    (action.ownerOnly && state.current->recognitionId != action.recognitionId) ||
                    (state.current->kind == PanelKind::Running &&
                     state.current->recognitionId != action.recognitionId)) {
                    continue;
                }
                state.current.reset();
                state.shownAt = now;
                showStatusNow(uuid, "");
                continue;
            }

            if (state.current && state.current->kind == PanelKind::Running &&
                action.message->kind == PanelKind::Preemptible &&
                state.current->recognitionId != action.message->recognitionId) {
                reportError_(action.message->text);
                FCITX_INFO() << "Ainput: id=" << action.message->recognitionId
                             << " stage=error_notified";
                continue;
            }

            displayPanel(uuid, state, std::move(*action.message), now);
            break;
        }
        now = fcitx::now(CLOCK_MONOTONIC);
        if (!state.current && state.pending.empty()) {
            it = panels_.erase(it);
        } else {
            ++it;
        }
    }

    const uint64_t delay = nextPanelDelay(now);
    if (delay == std::numeric_limits<uint64_t>::max()) {
        return;
    }
    armPanelTimer(delay);
}

void OutputHandler::armPanelTimer(uint64_t delayUsec) {
    if (!panelTimer_) return;
    panelTimer_->setTime(fcitx::now(CLOCK_MONOTONIC) + delayUsec);
    panelTimer_->setOneShot();
}

uint64_t OutputHandler::nextPanelDelay(uint64_t now) const {
    uint64_t next = std::numeric_limits<uint64_t>::max();
    for (const auto &[uuid, state] : panels_) {
        if (state.pending.empty()) continue;
        if (!state.current) return 0;
        const uint64_t elapsed = now - state.shownAt;
        next = std::min(next, elapsed >= kMinimumPanelUsec
                                  ? 0
                                  : kMinimumPanelUsec - elapsed);
    }
    return next;
}

} // namespace ainput
