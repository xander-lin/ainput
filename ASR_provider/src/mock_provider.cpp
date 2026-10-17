#include "mock_provider.h"

namespace ainput {

static bool _mockRegistered = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<MockAsrProviderFactory>());
    return true;
}();

void MockAsrProvider::transcribe(std::vector<int16_t>, const std::string &) {
    if (onResult_) onResult_("hello world", true);
}

} // namespace ainput
