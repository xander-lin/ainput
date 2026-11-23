#include "output_handler.h"

#include <fcitx/inputpanel.h>

#include <cstdlib>
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

    std::string panelText() const { return inputPanel().auxUp().toString(); }

protected:
    void commitStringImpl(const std::string &text) override {
        commits.push_back(text);
        panelAtCommit.push_back(panelText());
        if (onCommit) onCommit();
        eventLoop_.exit();
    }
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
    void updatePreeditImpl() override {}

private:
    fcitx::EventLoop &eventLoop_;
};

void check(bool condition) {
    if (!condition) std::abort();
}

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

    std::vector<std::string> notifications;
    ainput::OutputHandler output(&instance, [&](const std::string &error) {
        notifications.push_back(error);
    });
    std::vector<size_t> pendingAtCommit;
    duringRecording.onCommit = [&] { pendingAtCommit.push_back(output.pendingCount()); };

    duringRecording.focusIn();
    check(instance.mostRecentInputContext() == &duringRecording);
    output.rememberTarget(1);
    output.rememberTarget(2);
    duringRecording.focusOut();
    idle.focusIn();
    check(instance.mostRecentInputContext() == &idle);
    output.rememberTarget(5);

    // Dictation 2 is recording and owns the panel of its input context when
    // dictation 1 fails. Dictation 5 fails with nothing else in flight.
    output.showStatus(2, "listening...");
    output.fail(1, "first failed");
    output.fail(5, "fifth failed");
    // Then the recording finishes. Everything is drained in one callback.
    output.submit(2, "second");
    instance.eventLoop().exec();

    // The failure that could not reach its panel went to the notification
    // channel; it is the only one that did.
    check(notifications == Texts{"first failed"});

    // The live status survived until the text was committed, then the panel
    // was cleared.
    check(duringRecording.commits == Texts{"second"});
    check(duringRecording.panelAtCommit == Texts{"listening..."});
    check(duringRecording.panelText().empty());

    // With nothing else in flight, a failure simply goes to the panel.
    check(idle.commits.empty());
    check(idle.panelText() == "fifth failed");

    // Dictation 2 held its slot until it was delivered.
    check(pendingAtCommit == std::vector<size_t>{0});
    check(output.pendingCount() == 0);
}
