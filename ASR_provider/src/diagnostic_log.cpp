#include "diagnostic_log.h"
#include "sha256.h"

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ainput {

namespace {

std::string jsonEscape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (unsigned char c : value) {
        switch (c) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                escaped += buf;
            } else {
                escaped += static_cast<char>(c);
            }
        }
    }
    return escaped;
}

std::string nowString() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const auto time = system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);

    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
    char result[40];
    snprintf(result, sizeof(result), "%s.%03lldZ", date,
             static_cast<long long>(millis.count()));
    return result;
}

std::string threadTag() {
    return std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

std::string makeLine(std::string_view component, std::string_view name,
                     const DiagnosticFields &fields) {
    std::ostringstream out;
    out << "{\"ts\":\"" << jsonEscape(nowString())
        << "\",\"pid\":\"" << getpid()
        << "\",\"tid\":\"" << threadTag()
        << "\",\"component\":\"" << jsonEscape(component)
        << "\",\"event\":\"" << jsonEscape(name) << "\"";
    for (const auto &[key, value] : fields) {
        out << ",\"" << jsonEscape(key) << "\":\""
            << jsonEscape(value) << "\"";
    }
    out << "}\n";
    return out.str();
}

#if AINPUT_DIAGNOSTICS_ENABLED
std::string defaultPath() {
    if (const char *overridePath = getenv("AINPUT_DIAGNOSTIC_LOG");
        overridePath && *overridePath) {
        return overridePath;
    }
    const char *home = getenv("HOME");
    if (home && *home)
        return std::string(home) + "/.local/share/ainput/diagnostic.log";
    return "/tmp/ainput-diagnostic.log";
}
#endif

uint64_t existingSize(const std::string &path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

} // namespace

std::string hashDiagnosticValue(std::string_view value) {
    return sha256::hashHex(value);
}

FileDiagnosticLog::FileDiagnosticLog(std::string path, uint64_t maxBytes)
    : path_(std::move(path)), maxBytes_(maxBytes),
      bytesWritten_(existingSize(path_)) {
    if (bytesWritten_ >= maxBytes_) {
        stopped_ = true;
        return;
    }
    const auto parent = std::filesystem::path(path_).parent_path();
    std::error_code error;
    if (!parent.empty())
        std::filesystem::create_directories(parent, error);
    fd_ = ::open(path_.c_str(),
                 O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                 S_IRUSR | S_IWUSR);
    if (fd_ < 0) stopped_ = true;
}

FileDiagnosticLog::~FileDiagnosticLog() {
    if (fd_ >= 0) ::close(fd_);
}

void FileDiagnosticLog::event(std::string_view component, std::string_view name,
                              const DiagnosticFields &fields) noexcept {
    if (fd_ < 0) return;
    try {
        const auto line = makeLine(component, name, fields);
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        bytesWritten_ = std::max(bytesWritten_, existingSize(path_));
        if (bytesWritten_ >= maxBytes_ ||
            line.size() > maxBytes_ - bytesWritten_) {
            stopped_ = true;
            return;
        }

        const size_t written = ::write(fd_, line.data(), line.size());
        if (written != line.size()) {
            stopped_ = true;
            return;
        }
        bytesWritten_ += line.size();
    } catch (...) {
        // Diagnostics must never affect recognition or the host event loop.
    }
}

bool FileDiagnosticLog::stopped() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
}

void MemoryDiagnosticLog::event(std::string_view component, std::string_view name,
                                const DiagnosticFields &fields) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(makeLine(component, name, fields));
    } catch (...) {
    }
}

std::vector<std::string> MemoryDiagnosticLog::lines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
}

std::unique_ptr<IDiagnosticLog> makeDiagnosticLog() {
#if AINPUT_DIAGNOSTICS_ENABLED
    return std::make_unique<FileDiagnosticLog>(defaultPath());
#else
    return std::make_unique<NullDiagnosticLog>();
#endif
}

IDiagnosticLog &diagnosticLog() {
    static std::unique_ptr<IDiagnosticLog> logger = makeDiagnosticLog();
    return *logger;
}

} // namespace ainput
