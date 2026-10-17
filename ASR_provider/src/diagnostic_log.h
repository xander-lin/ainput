#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef AINPUT_DIAGNOSTICS_ENABLED
#define AINPUT_DIAGNOSTICS_ENABLED 0
#endif

namespace ainput {

using DiagnosticFields = std::vector<std::pair<std::string, std::string>>;

class IDiagnosticLog {
public:
    virtual ~IDiagnosticLog() = default;

    virtual void event(std::string_view component, std::string_view name,
                       const DiagnosticFields &fields = {}) noexcept = 0;
};

inline constexpr uint64_t kMaxDiagnosticLogBytes = 1ULL << 30;

class FileDiagnosticLog final : public IDiagnosticLog {
public:
    explicit FileDiagnosticLog(std::string path,
                               uint64_t maxBytes = kMaxDiagnosticLogBytes);
    ~FileDiagnosticLog() override;

    void event(std::string_view component, std::string_view name,
               const DiagnosticFields &fields = {}) noexcept override;

    bool stopped() const noexcept;

private:
    std::string path_;
    uint64_t maxBytes_;
    int fd_ = -1;
    mutable std::mutex mutex_;
    uint64_t bytesWritten_ = 0;
    bool stopped_ = false;
};

class MemoryDiagnosticLog final : public IDiagnosticLog {
public:
    void event(std::string_view component, std::string_view name,
               const DiagnosticFields &fields = {}) noexcept override;

    std::vector<std::string> lines() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

class NullDiagnosticLog final : public IDiagnosticLog {
public:
    void event(std::string_view, std::string_view,
               const DiagnosticFields & = {}) noexcept override {}
};

std::string hashDiagnosticValue(std::string_view value);
std::unique_ptr<IDiagnosticLog> makeDiagnosticLog();
IDiagnosticLog &diagnosticLog();

} // namespace ainput
