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
    /// Runs inside commitStringImpl, so it observes the delivery queue as
    /// this commit leaves it.
    std::function<void()> onCommit;

protected:
    void commitStringImpl(const std::string &text) override {
        commits.push_back(text);
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

} // namespace

int main() {
    char name[] = "ainput-output-test";
    char disable[] = "--disable=all";
    char *args[] = {name, disable, nullptr};
    fcitx::Instance instance(2, args);
    instance.initialize();
    TestContext context(instance);
    context.focusIn();
    check(instance.mostRecentInputContext() == &context);

    std::vector<std::string> errors;
    ainput::OutputHandler output(&instance, [&](const std::string &error) {
        errors.push_back(error);
    });
    std::vector<size_t> pendingAtCommit;
    context.onCommit = [&] { pendingAtCommit.push_back(output.pendingCount()); };

    // Job 1 failed after job 2 had already finished, job 3 is still recording
    // and job 4 has nothing to transcribe. All of it is drained in one
    // event-loop callback.
    output.rememberTarget(1);
    output.rememberTarget(2);
    output.rememberTarget(3);
    output.rememberTarget(4);
    output.submit(2, "second");
    output.fail(1, "first failed");
    output.noResult(4, "no speech detected");
    output.submit(3, "third");
    instance.eventLoop().exec();

    // The failure is reported even though a later success is delivered in the
    // same callback. The no-result outcome is not a failure, so it is not.
    check(errors == std::vector<std::string>{"first failed"});
    check(context.commits == std::vector<std::string>{"second", "third"});
    // Jobs 3 and 4 hold their slots until 3, the older one, has an outcome.
    check(pendingAtCommit == std::vector<size_t>{2, 0});
    // Nothing to transcribe is shown in the panel instead.
    check(context.inputPanel().auxUp().toString() == "no speech detected");
    check(output.pendingCount() == 0);
}
