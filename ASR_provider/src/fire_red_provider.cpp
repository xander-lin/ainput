#include "fire_red_provider.h"
#include "config_schema.h"
#include "ainput_config.h"
#include "diagnostic_log.h"

#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <spawn.h>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

namespace ainput {

// Threading knob retired from config; the default works for both local models.
constexpr int kSherpaNumThreads = 30;

// Last non-empty line of captured child output, trimmed for error detail.
static std::string lastOutputLine(std::string output) {
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' ||
                               output.back() == ' ' || output.back() == '\t')) {
        output.pop_back();
    }
    auto pos = output.rfind('\n');
    std::string line = pos == std::string::npos ? output : output.substr(pos + 1);
    if (line.size() > 120) line = line.substr(0, 120);
    return line;
}

static std::string expandPath(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

FireRedAsrProvider::FireRedAsrProvider()
    : modelDir_("~/.local/share/ainput/models/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26") {
    auto cfg = readConfigFile("fire_red.json");
    if (!cfg.empty()) {
        reportConfigIssues("fire_red", "fire_red.json",
                           validateConfigJson(cfg, localModelConfigSchema()));
        auto d = jsonStr(cfg, "model_dir");
        if (!d.empty()) modelDir_ = d;
        timeoutSec_ = jsonInt(cfg, "timeout_sec", timeoutSec_);
        auto b = jsonStr(cfg, "bin_path");
        if (!b.empty()) sherpaBin_ = b;
    }
}

FireRedAsrProvider::~FireRedAsrProvider() {
    diagnosticLog().event("provider", "provider_shutdown", {
        {"provider", "fire_red"},
        {"recognition_id", std::to_string(diagnosticId_)}
    });
    cancel_->store(true);
    joinAsrWorker(worker_);
}

void FireRedAsrProvider::setConfig(const std::string &key,
                                    const std::string &value) {
    if (key == "model_dir") {
        modelDir_ = value;
    }
}

void FireRedAsrProvider::transcribe(std::vector<int16_t>, const std::string &wavPath) {
    diagnosticLog().event("provider", "request_started", {
        {"provider", "fire_red"},
        {"recognition_id", std::to_string(diagnosticId_)},
        {"wav_hash", hashDiagnosticValue(wavPath).substr(0, 16)}
    });
    cancel_->store(true);
    joinAsrWorker(worker_);
    cancel_ = std::make_shared<std::atomic_bool>(false);
    startAsrWorker(
        worker_,
        [wavPath, modelDir = expandPath(modelDir_),
         sherpaBin = expandPath(sherpaBin_), numThreads = kSherpaNumThreads,
          timeoutSec = timeoutSec_, cancel = cancel_, onR = onResult_,
          onE = onError_, diagnosticId = diagnosticId_]() mutable {
            runTranscribe(wavPath, modelDir, std::move(sherpaBin), numThreads,
                          timeoutSec, std::move(cancel), std::move(onR),
                          std::move(onE), diagnosticId);
        });
}

void FireRedAsrProvider::runTranscribe(const std::string &wav,
                                         const std::string &dir,
                                         std::string sherpaBin, int numThreads,
                                         int timeoutSec,
                                         std::shared_ptr<std::atomic_bool> cancel,
                                         AsrResultCallback onR,
                                         AsrErrorCallback onE,
                                         uint64_t diagnosticId) {
    auto t0 = std::chrono::steady_clock::now();
    struct Cleanup { std::string p; ~Cleanup() { unlink(p.c_str()); } } cleanup{wav};

        // Preflight: actionable setup errors before spawning the engine.
        if (access(sherpaBin.c_str(), X_OK) != 0) {
            diagnosticLog().event("provider", "request_error", {
                {"provider", "fire_red"}, {"recognition_id", std::to_string(diagnosticId)},
                {"reason", "binary_missing"}
            });
            if (onE) onE("FireRed: sherpa-onnx-offline binary not found at " + sherpaBin +
                         " (set bin_path in ~/.config/ainput/fire_red.json)",
                         AsrErrorCategory::LocalSetup);
            return;
        }
        for (const char *f : {"encoder.int8.onnx", "decoder.int8.onnx",
                              "tokens.txt"}) {
            std::string full = dir + "/" + f;
            if (!std::filesystem::exists(full)) {
                diagnosticLog().event("provider", "request_error", {
                    {"provider", "fire_red"}, {"recognition_id", std::to_string(diagnosticId)},
                    {"reason", "model_file_missing"}
                });
                if (onE) onE("FireRed: model file missing: " + full +
                             " (set model_dir in ~/.config/ainput/fire_red.json)",
                             AsrErrorCategory::ModelNotFound);
                return;
            }
        }

        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC) < 0) {
            if (onE) onE("FireRed: pipe failed", AsrErrorCategory::LocalSetup);
            return;
        }

        std::vector<std::string> args = {
            sherpaBin,
            "--fire-red-asr-encoder=" + dir + "/encoder.int8.onnx",
            "--fire-red-asr-decoder=" + dir + "/decoder.int8.onnx",
            "--tokens=" + dir + "/tokens.txt",
            "--num-threads=" + std::to_string(numThreads),
            wav
        };
        std::vector<const char*> argv;
        for (auto &a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);

        pid_t pid;
        int ret = posix_spawn(&pid, sherpaBin.c_str(), &actions, &attr,
                              (char *const *)argv.data(), ::environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[1]);

        if (ret != 0) {
            close(pipefd[0]);
            if (onE) onE("FireRed: spawn failed (" + std::string(strerror(ret)) + ")",
                         AsrErrorCategory::LocalSetup);
            return;
        }

        int flags = fcntl(pipefd[0], F_GETFL, 0);
        if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

        std::string output;
        char buf[4096];
        int status = 0;
        bool reaped = false;
        bool timedOut = false;
        auto deadline = t0 + std::chrono::seconds(std::max(timeoutSec, 1));

        while (!reaped) {
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                output.append(buf, (size_t)n);
            }

            pid_t waitResult = waitpid(pid, &status, WNOHANG);
            if (waitResult == pid) {
                reaped = true;
                break;
            }
            if (waitResult < 0 && errno != EINTR) break;

            timedOut = std::chrono::steady_clock::now() >= deadline;
            if (cancel->load() || timedOut) {
                diagnosticLog().event("provider", timedOut ? "request_timeout" : "request_cancelled", {
                    {"provider", "fire_red"},
                    {"recognition_id", std::to_string(diagnosticId)},
                    {"stage", "process"}
                });
                kill(-pid, SIGTERM);
                for (int i = 0; i < 20; i++) {
                    waitResult = waitpid(pid, &status, WNOHANG);
                    if (waitResult == pid) {
                        reaped = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                // The leader may exit while a descendant ignores SIGTERM.
                kill(-pid, SIGKILL);
                if (!reaped) {
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                }
                close(pipefd[0]);
                if (timedOut && onE) onE("FireRed: recognition timed out",
                                         AsrErrorCategory::Timeout);
                return;
            }

            pollfd pfd = {pipefd[0], POLLIN, 0};
            (void)poll(&pfd, 1, 100);
        }

        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            output.append(buf, (size_t)n);
        }
        close(pipefd[0]);

        if (!reaped) {
            fprintf(stderr, "Ainput FireRed: waitpid failed\n");
            unlink(wav.c_str());
            if (onE) onE("FireRed: recognition failed (waitpid)",
                         AsrErrorCategory::Runtime);
            return;
        }
        auto tRecv = std::chrono::steady_clock::now();

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            fprintf(stderr, "Ainput FireRed: child exit=%d\n", exitCode);
            unlink(wav.c_str());
            std::string tail = lastOutputLine(output);
            diagnosticLog().event("provider", "request_error", {
                {"provider", "fire_red"}, {"recognition_id", std::to_string(diagnosticId)},
                {"reason", "child_failed"}, {"exit_code", std::to_string(exitCode)}
            });
            if (onE) onE("FireRed: recognition failed (exit=" +
                         std::to_string(exitCode) +
                         (tail.empty() ? ")" : "): " + tail),
                         AsrErrorCategory::Runtime);
            return;
        }

        auto pos = output.rfind("\"text\"");
        std::string text;
        if (pos != std::string::npos) {
            pos += 9;
            auto end = output.find('"', pos);
            if (end != std::string::npos)
                text = output.substr(pos, end - pos);
        }

        auto tParse = std::chrono::steady_clock::now();
        fprintf(stderr, "Ainput FireRed [timer] exec_total=%ldms parse=%ldms text_len=%zu\n",
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tRecv - t0).count(),
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tParse - tRecv).count(),
                text.size());

        unlink(wav.c_str());
        if (onR && !text.empty()) {
            diagnosticLog().event("provider", "request_result", {
                {"provider", "fire_red"},
                {"recognition_id", std::to_string(diagnosticId)},
                {"text_length", std::to_string(text.size())},
                {"exec_ms", std::to_string(std::chrono::duration_cast<
                    std::chrono::milliseconds>(tRecv - t0).count())},
                {"parse_ms", std::to_string(std::chrono::duration_cast<
                    std::chrono::milliseconds>(tParse - tRecv).count())}
            });
            onR(text, true);
        } else if (onE) {
            diagnosticLog().event("provider", "request_error", {
                {"provider", "fire_red"},
                {"recognition_id", std::to_string(diagnosticId)},
                {"reason", "empty_result"}
            });
            onE("FireRed: empty result", AsrErrorCategory::EmptyResult);
        }
}

std::unique_ptr<IAsrProvider> FireRedAsrProviderFactory::create() {
    return std::make_unique<FireRedAsrProvider>();
}

std::string FireRedAsrProviderFactory::displayName() const {
    std::string dir = "~/.local/share/ainput/models/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26";
    std::string warning;
    std::string cfg = readConfigFile("fire_red.json");
    if (!cfg.empty()) {
        auto issues = validateConfigJson(cfg, localModelConfigSchema());
        reportConfigIssues("fire_red", "fire_red.json", issues);
        warning = firstConfigIssue(issues);
        auto d = jsonStr(cfg, "model_dir");
        if (!d.empty()) dir = d;
    }
    std::filesystem::path p(expandPath(dir));
    std::string base = "FireRed · " + p.filename().string() + " (local)";
    return warning.empty() ? base : base + "\n\u26a0 fire_red.json: " + warning;
}

static bool _frReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<FireRedAsrProviderFactory>());
    return true;
}();

} // namespace ainput
