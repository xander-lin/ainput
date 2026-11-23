#include "output_handler.h"

#include <fcitx/inputpanel.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/text.h>
#include <fcitx-utils/log.h>

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
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

void OutputHandler::noResult(uint64_t recognitionId, const std::string &text) {
    if (recognitionId == 0) {
        showGlobalStatus(text);
    } else {
        enqueue({text, Kind::NoResult, recognitionId});
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
    for (const auto &uuid : targets_.discard()) showStatusNow(uuid, "");
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
                showStatusNow(ic->uuid(), pending.text);
            }
            continue;
        }
        const auto *target = targets_.find(id);
        if (!target) continue; // Cancelled or already delivered.
        if (pending.kind == Kind::Status) {
            if (!target->outcome && !targets_.hasNewer(id, target->context)) {
                showStatusNow(target->context, pending.text);
            }
            continue;
        }
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
        // A later commit can clear the panel in this same event-loop callback.
        reportError_(pending.text);
        if (!targets_.hasNewer(entry.id, entry.context)) showStatusNow(entry.context, pending.text);
        FCITX_INFO() << "Ainput: id=" << entry.id << " stage=error_reported";
        return;
    }
    if (pending.kind == Kind::NoResult) {
        // Not a failure, so no notification: the panel carries it, and a
        // newer dictation in the same context legitimately takes the panel
        // over. The journal still has the reason.
        if (!targets_.hasNewer(entry.id, entry.context)) showStatusNow(entry.context, pending.text);
        FCITX_INFO() << "Ainput: id=" << entry.id << " stage=no_result";
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
    if (!targets_.hasNewer(entry.id, ic->uuid())) showStatusNow(ic->uuid(), "");
    FCITX_INFO() << "Ainput: id=" << entry.id << " stage=committed text_len=" << pending.text.size();
}

} // namespace ainput
