#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include "asr_provider.h"

namespace ainput {

class ZipformerAsrProvider : public IAsrProvider {
public:
    ZipformerAsrProvider();
    ~ZipformerAsrProvider() override;

    void transcribe(std::vector<int16_t>, const std::string &wavPath) override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    static void runTranscribe(const std::string &wav, const std::string &dir,
                              std::string sherpaBin, int numThreads,
                              int timeoutSec,
                              std::shared_ptr<std::atomic_bool> cancel,
                              AsrResultCallback onR, AsrErrorCallback onE,
                              uint64_t diagnosticId);

    std::string modelDir_;
    std::string sherpaBin_ = "~/.local/share/ainput/sherpa-onnx/bin/sherpa-onnx";
    int timeoutSec_ = 120;
    std::shared_ptr<std::atomic_bool> cancel_ =
        std::make_shared<std::atomic_bool>(false);
    std::thread worker_;
};

class ZipformerAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "zipformer"; }
    std::string name() const override { return "Zipformer (sherpa-onnx)"; }
    // "Zipformer · <model dir basename from zipformer.json> (local)"
    std::string displayName() const override;
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace ainput
