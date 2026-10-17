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

OutputHandler::OutputHandler(fcitx::Instance *instance) : instance_(instance) {
    if (pipe(wakePipe_) != 0) {
        throw std::runtime_error("Ainput: OutputHandler pipe() failed");
    }
    fcntl(wakePipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(wakePipe_[1], F_SETFL, O_NONBLOCK);

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
    if (wakePipe_[0] >= 0) close(wakePipe_[0]);
    if (wakePipe_[1] >= 0) close(wakePipe_[1]);
}

void OutputHandler::rememberTarget(uint64_t recognitionId) {
    // Event loop thread only.
    fcitx::ICUUID uuid = {};
    if (auto *ic = instance_->mostRecentInputContext()) {
        uuid = ic->uuid();
    } else {
        FCITX_INFO() << "Ainput: starting with no input context; the result "
                        "will go to whatever is focused when it arrives";
    }
    targets_[recognitionId] = uuid;
    if (targets_.size() > 32) {
        // Recordings are removed on delivery; this only guards against a lost
        // completion. Drop the oldest entries.
        targets_.erase(targets_.begin());
    }
}

void OutputHandler::submit(uint64_t recognitionId, const std::string &text) {
    Pending pending;
    pending.text = text;
    pending.isStatus = false;
    pending.recognitionId = recognitionId;
    enqueue(std::move(pending));
}

void OutputHandler::showStatus(uint64_t recognitionId, const std::string &text) {
    Pending pending;
    pending.text = text;
    pending.isStatus = true;
    pending.recognitionId = recognitionId;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending.sequence = ++latestStatusSequence_;
    }
    enqueue(std::move(pending));
}

void OutputHandler::showStage(uint64_t recognitionId, Stage stage) {
    showStatus(recognitionId, stageText(stage));
}

void OutputHandler::showGlobalStatus(const std::string &text) {
    Pending pending;
    pending.text = text;
    pending.isStatus = true;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending.sequence = ++latestStatusSequence_;
    }
    enqueue(std::move(pending));
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

fcitx::ICUUID OutputHandler::resolve(const Pending &pending) {
    auto it = targets_.find(pending.recognitionId);
    if (it != targets_.end()) {
        return it->second;
    }
    if (auto *ic = instance_->mostRecentInputContext()) {
        return ic->uuid();
    }
    return {};
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
        if (pending.isStatus) {
            if (pending.sequence < latestStatusSequence_) {
                if (pending.completion) pending.completion();
                continue;
            }
            showStatusNow(resolve(pending), pending.text, pending.sequence);
            if (pending.completion) pending.completion();
        } else {
            commitPending(std::move(pending));
        }
    }
}

void OutputHandler::showStatusNow(const fcitx::ICUUID &uuid,
                                  const std::string &text, uint64_t sequence) {
    auto *ic = instance_->inputContextManager().findByUUID(uuid);
    if (!ic) {
        if (!text.empty()) {
            FCITX_INFO() << "Ainput [status] no input context for \"" << text
                         << "\"; not shown";
        }
        return;
    }
    (void)sequence;
    ic->inputPanel().setAuxUp(fcitx::Text(text));
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void OutputHandler::commitPending(Pending pending) {
    // The result is final: drop the recording's target and clear the status
    // line. Leaving "processing audio..." in the panel after the text landed
    // is a silent stall from the user's point of view.
    targets_.erase(pending.recognitionId);

    auto *ic = instance_->inputContextManager().findByUUID(resolve(pending));
    if (!ic) {
        FCITX_INFO() << "Ainput [commit] no focused ic, drop text_len="
                     << pending.text.size();
        if (pending.completion) pending.completion();
        return;
    }

    FCITX_INFO() << "Ainput [commit] ic=" << ic << " program=" << ic->program()
                 << " text_len=" << pending.text.size();
    ic->inputPanel().setAuxUp(fcitx::Text(""));
    if (!pending.text.empty()) {
        ic->commitString(pending.text);
    }
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    if (pending.completion) pending.completion();
}

} // namespace ainput
