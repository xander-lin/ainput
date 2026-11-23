#include "output_handler.h"

#include <cstdlib>
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

protected:
    void commitStringImpl(const std::string &text) override {
        commits.push_back(text);
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
    output.rememberTarget(1);
    output.rememberTarget(2);
    output.rememberTarget(3);
    output.submit(2, "second");
    output.fail(1, "first failed");
    instance.eventLoop().exec();
    check(errors == std::vector<std::string>{"first failed"});
    check(context.commits == std::vector<std::string>{"second"});
    check(output.pendingCount() == 1);
}
