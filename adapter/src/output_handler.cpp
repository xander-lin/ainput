#include "output_handler.h"
#include "diagnostic_log.h"
#include "ainput_config.h"

#include <fcitx/inputpanel.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/text.h>
#include <fcitx-utils/log.h>

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace ainput {

namespace {

std::string diagnosticUuid(const fcitx::ICUUID &uuid) {
    return hashDiagnosticValue(std::string_view(
        reinterpret_cast<const char *>(uuid.data()), uuid.size())).substr(0, 16);
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

void OutputHandler::submit(const OutputTarget &target, const std::string &text,
                           std::function<void()> onCommitted) {
    enqueue(target, text, false, std::move(onCommitted));
}

void OutputHandler::showStatus(const OutputTarget &target,
                               const std::string &text,
                               std::function<void()> onShown) {
    enqueue(target, text, true, std::move(onShown));
}

OutputTarget OutputHandler::captureCurrentUuid(uint64_t recognitionId) {
    OutputTarget target;
    target.recognitionId = recognitionId;
    target.statusSequence = ++latestStatusSequence_;
    if (auto *ic = instance_->mostRecentInputContext()) {
        target.uuid = ic->uuid();
    }
    diagnosticLog().event("output", "target_captured", {
        {"recognition_id", std::to_string(target.recognitionId)},
        {"target_uuid", diagnosticUuid(target.uuid)},
        {"status_sequence", std::to_string(target.statusSequence)}
    });
    return target;
}

void OutputHandler::wake() {
    char c = 1;
    while (write(wakePipe_[1], &c, 1) < 0 && errno == EINTR) {}
}

void OutputHandler::enqueue(const OutputTarget &target, const std::string &text,
                            bool isStatus,
                            std::function<void()> completion) {
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        pending_.push_back({text, isStatus, target.uuid,
                            target.statusSequence, target.recognitionId,
                            std::move(completion)});
    }
    diagnosticLog().event("output", isStatus ? "status_enqueued" : "result_enqueued", {
        {"recognition_id", std::to_string(target.recognitionId)},
        {"target_uuid", diagnosticUuid(target.uuid)},
        {"text_length", std::to_string(text.size())}
    });
    wake();
}

void OutputHandler::drainAndCommit() {
    char buf[64];
    while (read(wakePipe_[0], buf, sizeof(buf)) > 0) {}

    std::vector<Pending> batch;
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        batch.swap(pending_);
    }

    // Status belongs in the input panel, never in the application's document.
    for (auto &p : batch) {
        if (p.isStatus) {
            if (p.statusSequence < latestStatusSequence_) {
                if (p.completion) p.completion();
                continue;
            }
            auto *ic = instance_->inputContextManager().findByUUID(p.targetUuid);
            if (ic) {
                ic->inputPanel().setAuxUp(fcitx::Text(p.text));
                ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
                diagnosticLog().event("output", "status_shown", {
                    {"recognition_id", std::to_string(p.recognitionId)},
                    {"target_uuid", diagnosticUuid(p.targetUuid)},
                    {"text_length", std::to_string(p.text.size())}
                });
            } else {
                diagnosticLog().event("output", "status_context_missing", {
                    {"recognition_id", std::to_string(p.recognitionId)},
                    {"target_uuid", diagnosticUuid(p.targetUuid)},
                    {"text_length", std::to_string(p.text.size())}
                });
            }
            if (p.completion) p.completion();
        } else {
            commitPending(std::move(p), "commit");
        }
    }
}

void OutputHandler::commitPending(Pending pending, const char *label) {
    auto *ic = instance_->inputContextManager().findByUUID(pending.targetUuid);
    if (!ic) {
        diagnosticLog().event("output", "commit_context_missing", {
            {"recognition_id", std::to_string(pending.recognitionId)},
            {"target_uuid", diagnosticUuid(pending.targetUuid)},
            {"text_length", std::to_string(pending.text.size())}
        });
        FCITX_INFO() << "Ainput [" << label << "] no focused ic, drop";
    } else {
        diagnosticLog().event("output", "commit_context_found", {
            {"recognition_id", std::to_string(pending.recognitionId)},
            {"target_uuid", diagnosticUuid(pending.targetUuid)},
            {"text_length", std::to_string(pending.text.size())}
        });
        FCITX_INFO() << "Ainput [" << label << "] ic=" << ic
                     << " program=" << ic->program()
                     << " text_len=" << pending.text.size();
        if (!pending.text.empty()) ic->commitString(pending.text);
    }
    if (pending.completion) pending.completion();
}

} // namespace ainput