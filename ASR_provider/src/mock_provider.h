#pragma once

#include "asr_provider.h"

namespace ainput {

class MockAsrProvider : public IAsrProvider {
public:
    void transcribe(std::vector<int16_t> samples, const std::string &wavPath) override;
};

class MockAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "mock"; }
    std::string name() const override { return "Mock (test)"; }
    std::unique_ptr<IAsrProvider> create() override {
        return std::make_unique<MockAsrProvider>();
    }
};

} // namespace ainput
