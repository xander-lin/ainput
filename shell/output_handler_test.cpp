#include "output_handler.h"

#include <fcitx/inputpanel.h>
#include <fcitx-utils/eventloopinterface.h>

#include <cstdlib>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

class TestContext : public fcitx::InputContext {
public:
    explicit TestContext(fcitx::Instance &instance)
        : InputContext(instance.inputContextManager()), eventLoop_(instance.eventLoop()) {
        created();
    }
    ~TestContext() override { destroy(); }

    const char *frontend() const override { return "ainput-test"; }
    std::vector<std::string> commits;
    /// Panel text at the moment of each commit, i.e. before the delivery
    /// code reacts to it.
    std::vector<std::string> panelAtCommit;
    std::function<void()> onCommit;
    bool exitOnCommit = true;

    std::string panelText() const { return inputPanel().auxUp().toString(); }

protected:
    void commitStringImpl(const std::string &text) override {
        commits.push_back(text);
        panelAtCommit.push_back(panelText());
        if (onCommit) onCommit();
        if (exitOnCommit) eventLoop_.exit();
    }
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
    void updatePreeditImpl() override {}

private:
    fcitx::EventLoop &eventLoop_;
};

void check(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "output-handler-test assertion failed at line %d\n", line);
        std::abort();
    }
}

#define check(condition) check(condition, __LINE__)

using Texts = std::vector<std::string>;

} // namespace

int main() {
    char name[] = "ainput-output-test";
    char disable[] = "--disable=all";
    char *args[] = {name, disable, nullptr};
    fcitx::Instance instance(2, args);
    // Focus only after initialize(): the input-context property factory is
    // registered there (found.md #35).
    instance.initialize();

    // One input context where an older dictation fails while a newer one is
    // recording, and one where a dictation fails with nothing else in flight.
    TestContext duringRecording(instance);
    TestContext idle(instance);
    TestContext queued(instance);
    TestContext starting(instance);
    TestContext quickSuccess(instance);
    TestContext quickError(instance);
    TestContext blocked(instance);

    std::vector<std::string> notifications;
    ainput::OutputHandler output(&instance, [&](const std::string &error) {
        notifications.push_back(error);
    });
    std::vector<size_t> pendingAtCommit;
    duringRecording.onCommit = [&] { pendingAtCommit.push_back(output.pendingCount()); };
    std::string idlePanelAfterDrain;
    std::unique_ptr<fcitx::EventSource> observePanel;
    idle.onCommit = [&] {
        observePanel = instance.eventLoop().addPostEvent(
            [&](fcitx::EventSource *) {
                idlePanelAfterDrain = idle.panelText();
                return false;
            });
    };

    duringRecording.focusIn();
    check(instance.mostRecentInputContext() == &duringRecording);
    output.rememberTarget(1);
    output.rememberTarget(2);
    duringRecording.focusOut();
    idle.focusIn();
    check(instance.mostRecentInputContext() == &idle);
    output.rememberTarget(5);
    output.rememberTarget(6);
    idle.focusOut();
    queued.focusIn();
    output.rememberTarget(7);
    output.rememberTarget(8);
    queued.focusOut();
    starting.focusIn();
    output.rememberTarget(9);
    output.rememberTarget(10);
    output.rememberTarget(11);
    starting.focusOut();
    quickSuccess.focusIn();
    output.rememberTarget(12);
    quickSuccess.focusOut();
    quickError.focusIn();
    output.rememberTarget(14);
    quickError.focusOut();
    blocked.focusIn();
    output.rememberTarget(20);
    output.rememberTarget(21);

    // Dictation 2 owns the panel when dictation 1 fails. Results 6 and 8
    // finish before 5 and 7, so each pair is delivered in a single FIFO batch.
    output.showStatus(2, "listening...");
    output.fail(1, "first failed");
    output.submit(6, "sixth");
    output.fail(5, "fifth failed");
    output.fail(8, "eighth failed");
    output.fail(7, "seventh failed");
    output.fail(9, "ninth failed");
    output.showStatus(11, "listening next...");
    output.fail(10, "tenth failed");
    output.showStatus(12, "first running");
    output.showStatus(14, "first error-case running");
    output.showStatus(21, "blocked result running");
    // Then the recording finishes. Everything is drained in one callback.
    output.submit(2, "second");
    duringRecording.exitOnCommit = false;
    idle.exitOnCommit = false;
    quickSuccess.exitOnCommit = false;
    quickError.exitOnCommit = false;
    blocked.exitOnCommit = false;
    auto quickJobs = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 20'000, 1'000,
        [&](fcitx::EventSourceTime *, uint64_t) {
            quickError.focusOut();
            quickSuccess.focusIn();
            output.rememberTarget(13);
            output.showStatus(13, "second running");
            output.submit(12, "first success");
            output.submit(13, "second success");
            quickSuccess.focusOut();
            quickError.focusIn();
            output.rememberTarget(15);
            output.showStatus(15, "second error-case running");
            output.submit(14, "first error-case success");
            output.fail(15, "second failed");
            output.submit(21, "buffered success");
            return false;
        });
    std::string firstQueuedPanel;
    std::string secondQueuedPanel;
    std::string startingPanel;
    std::string blockedPanel;
    auto checkFirst = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 40'000, 1'000,
        [&](fcitx::EventSourceTime *, uint64_t) {
            firstQueuedPanel = queued.panelText();
            return false;
        });
    auto checkSecond = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 180'000, 1'000,
        [&](fcitx::EventSourceTime *, uint64_t) {
            secondQueuedPanel = queued.panelText();
            startingPanel = starting.panelText();
            blockedPanel = blocked.panelText();
            return false;
        });
    auto releaseBlocked = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 220'000, 1'000,
        [&](fcitx::EventSourceTime *, uint64_t) {
            output.fail(20, "blocked earlier failure");
            return false;
        });
    auto stopTest = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 350'000, 1'000,
        [&](fcitx::EventSourceTime *, uint64_t) {
            instance.eventLoop().exit();
            return false;
        });
    instance.eventLoop().exec();

    // Both a displayed running status and one waiting for its turn prevent
    // another dictation's outcome from taking the panel.
    check(notifications == (Texts{"first failed", "tenth failed"}));

    // The live status survived until the text was committed, then the panel
    // was cleared.
    check(duringRecording.commits == Texts{"second"});
    check(duringRecording.panelAtCommit == Texts{"listening..."});
    check(duringRecording.panelText().empty());

    // With nothing else in flight, a failure simply goes to the panel.
    check(idle.commits == Texts{"sixth"});
    check(idle.panelAtCommit == Texts{"fifth failed"});
    check(idlePanelAfterDrain == "fifth failed");
    check(idle.panelText().empty());
    check(firstQueuedPanel == "seventh failed");
    check(secondQueuedPanel == "eighth failed");
    check(startingPanel == "listening next...");
    check(quickSuccess.panelText().empty());
    check(quickError.panelText() == "second failed");
    check(blockedPanel.empty());
    check(blocked.commits == Texts{"buffered success"});
    check(blocked.panelAtCommit == Texts{"blocked earlier failure"});

    // Only dictation 11 remains after the later test jobs complete.
    check(pendingAtCommit == std::vector<size_t>{5});
    check(output.pendingCount() == 1);
}
