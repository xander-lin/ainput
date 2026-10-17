// fcitx5 核心头文件
#include <fcitx/addonfactory.h>    // AddonFactory 基类
#include <fcitx/addoninstance.h>   // AddonInstance 基类
#include <fcitx/addonmanager.h>    // AddonManager, 用于获取 fcitx 实例
#include <fcitx/instance.h>        // Instance, fcitx 服务器实例
#include <fcitx/event.h>           // KeyEvent
#include <fcitx/inputcontext.h>    // InputContext
#include <fcitx/inputpanel.h>       // InputPanel, setClientPreedit
#include <fcitx/inputcontextmanager.h>  // findByUUID
#include <fcitx-utils/i18n.h>             // _() translation macro
#include <fcitx-utils/event.h>     // EventLoop, addTimeEvent
#include <fcitx-utils/eventloopinterface.h> // now()
#include <fcitx-utils/key.h>       // Key
#include <fcitx-utils/keysym.h>    // FcitxKey_Caps_Lock 等键值常量
#include <fcitx-utils/log.h>       // 日志宏 FCITX_INFO/FCITX_DEBUG 等

// uinput: 内核级常驻虚键设备, 兼容所有 Wayland compositor
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <spawn.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <deque>
#include <fstream>
#include <optional>
#include <string_view>
#include <cerrno>

// Ainput ASR provider 接口
#include "asr_provider.h"
#include "mock_provider.h"         // 确保 Mock 后端被链接并自动注册
#include "doubao_provider.h"      // 确保豆包后端被链接并自动注册
#include "qwen_provider.h"        // 确保千问后端被链接并自动注册
#include "audio_capture.h"
#include "config_schema.h"
#include "diagnostic_log.h"
#include "output_handler.h"
#include "ainput_config.h"

// notifications addon 公共 API (跨 addon 调用, 仅用于显示切换信息)
#include <fcitx-module/notifications/notifications_public.h>

static std::string expandPath(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

static std::atomic<uint64_t> nextRecognitionId{1};

static std::string diagnosticHash(std::string_view value) {
    return ainput::hashDiagnosticValue(value).substr(0, 16);
}

// AinputAddon — Ainput 语音输入插件的 addon 主体
// 继承 AddonInstance, fcitx5 加载 addon 时实例化此类
class AinputAddon : public fcitx::AddonInstance {
    struct CallbackGate {
        std::mutex mutex;
        AinputAddon *owner = nullptr;
    };

    template<typename Callback>
    static void withOwner(const std::shared_ptr<CallbackGate> &gate,
                          Callback &&callback) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        if (gate->owner) callback(*gate->owner);
    }

public:
    AinputAddon(fcitx::Instance *instance) : instance_(instance) {
        callbackGate_->owner = this;
        reloadConfig();

        auto vjson = ainput::readConfigFile("config.json");
        if (!vjson.empty()) {
            ainput::reportConfigIssues("ainput", "config.json",
                                       ainput::validateConfigFileJson(vjson));
            std::string ui = ainput::qjsonRawValue(vjson, "ui");
            if (!ui.empty()) {
                activationUsec_ = (uint64_t)ainput::jsonInt(ui, "activation_msec", 300) * 1000;
                notificationTimeout_ = ainput::jsonInt(ui, "notification_timeout", 2000);
                debounceCount_ = ainput::jsonInt(ui, "debounce_count", 2);
            }
        }

        FCITX_INFO() << "Ainput addon loaded";
        ainput::diagnosticLog().event("adapter", "addon_loaded", {
            {"diagnostics", AINPUT_DIAGNOSTICS_ENABLED ? "enabled" : "disabled"}
        });

        // 创建常驻 uinput 虚键盘, 用于还原 CapsLock
        initUinput();

        outputHandler_ = std::make_unique<ainput::OutputHandler>(instance_);

        // 在 PreInputMethod 阶段监听键盘事件 (早于输入法引擎)
        keyWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextKeyEvent,
            fcitx::EventWatcherPhase::PreInputMethod,
            [this](fcitx::Event &event) {
                auto &keyEvent = static_cast<fcitx::KeyEvent &>(event);
                onKeyEvent(keyEvent);
            });
    }

    ~AinputAddon() override {
        ainput::diagnosticLog().event("adapter", "addon_shutdown_begin");
        {
            std::lock_guard<std::mutex> lock(callbackGate_->mutex);
            callbackGate_->owner = nullptr;
        }
        shuttingDown_ = true;
        if (audioCapture_) audioCapture_->stop();
        for (auto &capture : finishingCaptures_) capture->stop();
        if (audioCapture_) audioCapture_->wait();
        for (auto &capture : finishingCaptures_) capture->wait();
        audioCapture_.reset();
        finishingCaptures_.clear();
        asr_.reset();
        for (const auto &request : recognitionQueue_) {
            unlink(request.wavPath.c_str());
        }
        recognitionQueue_.clear();
        if (uinputFd_ >= 0) {
            ioctl(uinputFd_, UI_DEV_DESTROY);
            close(uinputFd_);
        }
        ainput::diagnosticLog().event("adapter", "addon_shutdown_end");
    }

private:
    // config.json [ui] 节
    uint64_t activationUsec_ = 300 * 1000;  // activation_msec
    int notificationTimeout_ = 2000;         // notification_timeout
    int debounceCount_ = 2;                   // debounce_count

    // 当前选择的 ASR 后端（config.json 顶层 "provider"）
    std::string readProviderSelection() {
        return ainput::qjsonStringValue(ainput::readConfigFile("config.json"),
                                        "provider");
    }

    // config.json 原子写回：在原始文本上做最小替换以保留用户注释，
    // tmp + rename，文件权限 0600。
    void writeConfigJson(const std::string &json) {
        namespace fs = std::filesystem;
        std::error_code mk;
        fs::create_directories(ainput::configDir(), mk);
        std::string tmp = ainput::configPath("config.json.tmp");
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f) return;
            f << json;
        }
        chmod(tmp.c_str(), 0600);
        std::error_code ec;
        fs::rename(tmp, ainput::configPath("config.json"), ec);
        if (ec) unlink(tmp.c_str());
    }

    void persistProviderSelection(const std::string &id) {
        writeConfigJson(ainput::qjsonSetTopLevelString(
            ainput::readFileIfExists(ainput::configPath("config.json")),
            "provider", id));
    }

    void persistDenoiserSelection(const std::string &name) {
        writeConfigJson(ainput::qjsonSetSectionString(
            ainput::readFileIfExists(ainput::configPath("config.json")),
            "audio", "denoise", name));
    }

    // 创建常驻 uinput 虚拟键盘设备, 用于还原 CapsLock
    void initUinput() {
        uinputFd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (uinputFd_ < 0) {
            // CapsLock restore silently stops working without this, while the
            // rest of voice input keeps functioning — so fail loudly and point
            // at the shipped udev rule instead of looking like a broken key.
            FCITX_WARN() << "Ainput: cannot open /dev/uinput (errno=" << errno
                         << "); CapsLock restore disabled. Install/enable "
                            "70-ainput-uinput.rules and run "
                            "'udevadm control --reload && udevadm trigger "
                            "--subsystem-match=misc --sysname-match=uinput'.";
            ainput::diagnosticLog().event("adapter", "uinput_open_failed", {
                {"errno", std::to_string(errno)}
            });
            return;
        }
        ioctl(uinputFd_, UI_SET_EVBIT, EV_KEY);
        ioctl(uinputFd_, UI_SET_KEYBIT, KEY_CAPSLOCK);
        ioctl(uinputFd_, UI_SET_EVBIT, EV_LED);
        ioctl(uinputFd_, UI_SET_LEDBIT, LED_CAPSL);

        struct uinput_setup usetup = {};
        strcpy(usetup.name, "Ainput vkbd");
        usetup.id.bustype = BUS_VIRTUAL;
        ioctl(uinputFd_, UI_DEV_SETUP, &usetup);
        ioctl(uinputFd_, UI_DEV_CREATE);
        FCITX_INFO() << "Ainput uinput device created";
        ainput::diagnosticLog().event("adapter", "uinput_device_created");
    }

    // 还原 CapsLock — 通过 uinput 虚键发送 CapsLock (还原 LED, 会触发 IM 切换但马上恢复)
    void revertCapsLock() {
        if (uinputFd_ < 0) return;

        struct input_event ev = {};
        ev.type = EV_KEY;
        ev.code = KEY_CAPSLOCK;
        ev.value = 1;
        (void)!write(uinputFd_, &ev, sizeof(ev));
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        (void)!write(uinputFd_, &ev, sizeof(ev));

        ev = {};
        ev.type = EV_KEY;
        ev.code = KEY_CAPSLOCK;
        ev.value = 0;
        (void)!write(uinputFd_, &ev, sizeof(ev));
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        (void)!write(uinputFd_, &ev, sizeof(ev));
        FCITX_INFO() << "Ainput revert CapsLock via uinput";
    }

    fcitx::Instance *instance_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> keyWatcher_;
    int uinputFd_ = -1;
    int revertDebounce_ = 0;            // uinput CapsLock 反弹去抖计数

    // Output: encapsulates self-pipe and commit
    std::unique_ptr<ainput::OutputHandler> outputHandler_;

    // 运行时依赖: notifications addon (仅用于切换显示)
    FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());

    // 提示音: 用系统命令播放 WAV
    static void playSound(const std::string &name) {
        auto path = expandPath("~/.local/share/ainput/sounds/" + name + ".wav");
        if (access(path.c_str(), R_OK) != 0) return;

        // paplay 需要 PULSE_RUNTIME_PATH 环境变量
        const char *pulsePath = getenv("PULSE_RUNTIME_PATH");
        const char *xdgRuntime = getenv("XDG_RUNTIME_DIR");
        std::string paEnv;
        if (pulsePath) paEnv = std::string("PULSE_RUNTIME_PATH=") + pulsePath;
        else if (xdgRuntime) paEnv = std::string("PULSE_RUNTIME_PATH=") + xdgRuntime + "/pulse";

        const char *envp[2] = {paEnv.empty() ? nullptr : paEnv.c_str(), nullptr};
        pid_t pid;
        const char *argv[] = {"paplay", path.c_str(), nullptr};
        posix_spawn(&pid, "/usr/bin/paplay", nullptr, nullptr,
                    (char *const *)argv, envp[0] ? (char *const *)envp : nullptr);
    }

    // 状态
    bool active_ = false;               // 语音录音中
    bool switchActive_ = false;         // Ctrl+CapsLock 切换模式
    std::atomic_bool shuttingDown_{false};
    std::shared_ptr<CallbackGate> callbackGate_ =
        std::make_shared<CallbackGate>();

    // 性能计时
    std::chrono::steady_clock::time_point tPress_, tActivate_, tStop_;
    std::unique_ptr<fcitx::EventSourceTime> timer_;
    std::unique_ptr<ainput::IAsrProvider> asr_;
    std::string asrProviderId_;
    std::unique_ptr<ainput::AudioCapture> audioCapture_;
    std::vector<std::unique_ptr<ainput::AudioCapture>> finishingCaptures_;
    ainput::OutputTarget currentTarget_;
    fcitx::InputContext *currentIC_ = nullptr;
    fcitx::ICUUID currentUuid_ = {};  // 用于 deactivate 后仍能查找 IC
    std::string lastPreeditText_;       // deactivate 时 commit 用
    int providerIndex_ = 0;
    int denoiserIndex_ = 0;
    uint64_t currentRecognitionId_ = 0;

    struct RecognitionRequest {
        std::vector<int16_t> samples;
        std::string wavPath;
        std::string providerId;
        ainput::OutputTarget target;
        std::chrono::steady_clock::time_point pressTime;
        uint64_t recognitionId = 0;
    };
    static constexpr size_t kMaxPendingRecognitions = 3;
    std::deque<RecognitionRequest> recognitionQueue_;
    std::optional<RecognitionRequest> activeRecognition_;

    static const std::vector<std::string>& denoiserList() {
        static const std::vector<std::string> list = {"none", "speexdsp", "deepfilter"};
        return list;
    }

    // 从 keyEvent 提取切换方向, 0 表示非方向键
    static int switchDirection(const fcitx::KeyEvent &keyEvent) {
        switch (keyEvent.key().sym()) {
            case FcitxKey_Left:  case FcitxKey_h: return -1;
            case FcitxKey_Right: case FcitxKey_l: return  1;
            default: return 0;
        }
    }

    static int switchVertical(const fcitx::KeyEvent &keyEvent) {
        switch (keyEvent.key().sym()) {
            case FcitxKey_Up:   case FcitxKey_k: return -1;
            case FcitxKey_Down: case FcitxKey_j: return  1;
            default: return 0;
        }
    }

    // 仅切换 provider index + 通知 + 持久化, 不创建/启动 ASR 实例
    void doProviderSwitch(int direction) {
        auto list = ainput::AsrProviderRegistry::instance().listFactories();
        if (list.empty()) return;

        providerIndex_ = (providerIndex_ + direction + (int)list.size()) % (int)list.size();
        const auto &[nextId, nextName] = list[providerIndex_];

        auto total = (int)list.size();
        auto msg = nextName + " (" + std::to_string(providerIndex_ + 1)
                   + "/" + std::to_string(total) + ")";
        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-ainput", 0, "fcitx-ainput",
            "Ainput", msg,
            std::vector<std::string>{}, notificationTimeout_, nullptr, nullptr);

        FCITX_INFO() << "Ainput switch ASR provider: " << nextName;
        persistProviderSelection(nextId);
        playSound("switch");
    }

    // 切换降噪后端 + 通知 + 持久化
    void doDenoiserSwitch(int direction) {
        auto &list = denoiserList();
        denoiserIndex_ = (denoiserIndex_ + direction + (int)list.size()) % (int)list.size();
        const auto &name = list[denoiserIndex_];

        auto total = (int)list.size();
        auto msg = std::string("Denoiser: ") + name
                   + " (" + std::to_string(denoiserIndex_ + 1)
                   + "/" + std::to_string(total) + ")";
        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-ainput", 0, "fcitx-ainput",
            "Ainput", msg,
            std::vector<std::string>{}, notificationTimeout_, nullptr, nullptr);

        // 持久化到 config.json [audio]
        persistDenoiserSelection(name);

        FCITX_INFO() << "Ainput switch denoiser: " << name;
        playSound("switch");
    }

    // 键盘事件回调
    void onKeyEvent(fcitx::KeyEvent &keyEvent) {
        bool capsLock = (keyEvent.key().sym() == FcitxKey_Caps_Lock);

        // 放行 CapsLock; 切换模式或录音中放行所有键
        if (!capsLock && !active_ && !switchActive_ && !timer_) return;

        if (keyEvent.isRelease()) {
            if (capsLock) {
                ainput::diagnosticLog().event("input", "capslock_release", {
                    {"recognition_id", std::to_string(currentRecognitionId_)},
                    {"active", active_ ? "true" : "false"},
                    {"switch_active", switchActive_ ? "true" : "false"},
                    {"timer", timer_ ? "true" : "false"},
                    {"revert_debounce", std::to_string(revertDebounce_)}
                });
                if (revertDebounce_ > 0) {
                    revertDebounce_--;
                    return;
                }
                if (active_) {
                    onDeactivate();
                } else if (switchActive_) {
                    switchActive_ = false;
                    revertDebounce_ = debounceCount_;
                    playSound("deactivate");
                    revertCapsLock();
                } else {
                    timer_.reset();
                }
                keyEvent.filterAndAccept();
            }
            return;
        }

        // ---- 按下事件 ----
        if (capsLock) {
            ainput::diagnosticLog().event("input", "capslock_press", {
                {"recognition_id", std::to_string(currentRecognitionId_)},
                {"active", active_ ? "true" : "false"},
                {"switch_active", switchActive_ ? "true" : "false"},
                {"timer", timer_ ? "true" : "false"},
                {"revert_debounce", std::to_string(revertDebounce_)}
            });
            if (revertDebounce_ > 0) {
                revertDebounce_--;
                return;
            }
            if (timer_ || active_ || switchActive_) return;
            tPress_ = std::chrono::steady_clock::now();
            currentIC_ = keyEvent.inputContext();
            if (currentIC_) {
                currentUuid_ = currentIC_->uuid();
                FCITX_INFO() << "Ainput [press] ic=" << currentIC_
                             << " program=" << currentIC_->program()
                             << " frontend=" << currentIC_->frontendName();
            } else {
                FCITX_INFO() << "Ainput [press] no input context";
            }

            bool ctrlHeld = (keyEvent.key().states().toInteger() & (uint32_t)fcitx::KeyState::Ctrl) != 0;

            if (ctrlHeld) {
                // Ctrl+CapsLock: 进入切换模式 (不启用录音)
                switchActive_ = true;
                FCITX_INFO() << "Ainput switch mode active";

                auto list = ainput::AsrProviderRegistry::instance().listFactories();
                if (!list.empty()) {
                    auto &dnList = denoiserList();
                    int di = denoiserIndex_;
                    if (di < 0 || di >= (int)dnList.size()) di = 0;
                    auto msg = std::string("ASR: ") + list[providerIndex_].second
                               + " (" + std::to_string(providerIndex_ + 1)
                               + "/" + std::to_string((int)list.size()) + ")\n"
                               + "Denoiser: " + dnList[di]
                               + " (" + std::to_string(di + 1)
                               + "/" + std::to_string((int)dnList.size()) + ")";
                    notifications()->call<fcitx::INotifications::sendNotification>(
                        "fcitx5-ainput", 0, "fcitx-ainput",
                        "Ainput", msg,
                        std::vector<std::string>{}, notificationTimeout_, nullptr, nullptr);
                }
            } else {
                // 普通 CapsLock: 启动长按计时器
                timer_ = instance_->eventLoop().addTimeEvent(
                    CLOCK_MONOTONIC,
                    fcitx::now(CLOCK_MONOTONIC) + activationUsec_, 0,
                    [this](fcitx::EventSourceTime *, uint64_t) {
                        onActivate();
                        return false;
                    });
            }
            keyEvent.filterAndAccept();
            return;
        }

        // 切换模式下: 箭头/h/l/j/k 键切换 (可多次)
        if (switchActive_) {
            int dir = switchDirection(keyEvent);
            if (dir != 0) {
                doProviderSwitch(dir);
                keyEvent.filterAndAccept();
                return;
            }
            dir = switchVertical(keyEvent);
            if (dir != 0) {
                doDenoiserSwitch(dir);
                keyEvent.filterAndAccept();
                return;
            }
        }
    }

    // 按住达到配置的 activation_msec（默认 300ms）后触发
    void onActivate() {
        reapFinishedCaptures();
        timer_.reset();
        tActivate_ = std::chrono::steady_clock::now();
        auto pressMs = std::chrono::duration_cast<std::chrono::milliseconds>(tActivate_ - tPress_).count();
        FCITX_INFO() << "Ainput activated (press→activate=" << pressMs << "ms)";
        const auto recognitionId = nextRecognitionId.fetch_add(1);
        currentRecognitionId_ = recognitionId;

        // 重新捕获当前焦点窗口 (比 KeyEvent::inputContext 更可靠)
        auto *ic = instance_->mostRecentInputContext();
        if (ic) {
            currentIC_ = ic;
            currentUuid_ = ic->uuid();
            FCITX_INFO() << "Ainput [activate] ic=" << ic
                         << " program=" << ic->program()
                         << " frontend=" << ic->frontendName();
        } else {
            ainput::diagnosticLog().event("adapter", "activation_no_input_context", {
                {"recognition_id", std::to_string(recognitionId)}
            });
            FCITX_INFO() << "Ainput [activate] no input context";
            return;
        }

        // 捕获当前焦点窗口 (通过 OutputHandler 的桌面策略)
        if (outputHandler_) {
            currentTarget_ = outputHandler_->captureCurrentUuid(recognitionId);
            outputHandler_->showStatus(currentTarget_, "Ainput: listening...");
        }
        FCITX_INFO() << "Ainput [activate] captured window";
        ainput::diagnosticLog().event("adapter", "recognition_activated", {
            {"recognition_id", std::to_string(recognitionId)},
            {"provider_config", readProviderSelection()},
            {"press_to_activate_ms", std::to_string(pressMs)}
        });

        auto list = ainput::AsrProviderRegistry::instance().listFactories();
        if (list.empty()) {
            FCITX_INFO() << "Ainput: no ASR provider registered";
            return;
        }

        // 根据配置中的默认后端 ID 查找索引；全新安装（无 config.json）
        // 用文档默认 qwen，而不是注册顺序的首位。
        auto defaultId = readProviderSelection();
        if (defaultId.empty()) defaultId = "qwen";
        for (int i = 0; i < (int)list.size(); i++) {
            if (list[i].first == defaultId) {
                providerIndex_ = i;
                break;
            }
        }

        active_ = true;
        const auto providerId = list[providerIndex_].first;
        const auto target = currentTarget_;
        const auto pressTime = tPress_;
        auto callbackGate = callbackGate_;

        audioCapture_ = std::make_unique<ainput::AudioCapture>();
        audioCapture_->setDiagnosticId(recognitionId);
        {
            // 从 config.json [audio] 读取初始降噪方法，设置到 AudioCapture
            auto method = ainput::jsonStr(
                ainput::readConfigSection("config.json", "audio"), "denoise");
            if (!method.empty()) {
                auto &list = denoiserList();
                for (int i = 0; i < (int)list.size(); i++) {
                    if (list[i] == method) { denoiserIndex_ = i; break; }
                }
            }
        }
        audioCapture_->setRecordedCallback([callbackGate, providerId, target, pressTime,
                                             recognitionId](
                                               const std::vector<int16_t> &samples,
                                               const std::string &wav) {
            std::lock_guard<std::mutex> lock(callbackGate->mutex);
            auto *owner = callbackGate->owner;
            if (!owner) {
                ainput::diagnosticLog().event("adapter", "capture_callback_after_shutdown", {
                    {"recognition_id", std::to_string(recognitionId)},
                    {"wav_hash", diagnosticHash(wav)}
                });
                unlink(wav.c_str());
                return;
            }
            ainput::diagnosticLog().event("adapter", "capture_recorded_callback", {
                {"recognition_id", std::to_string(recognitionId)},
                {"sample_count", std::to_string(samples.size())},
                {"wav_hash", diagnosticHash(wav)}
            });
            auto request = std::make_shared<RecognitionRequest>(RecognitionRequest{
                samples, wav, providerId, target, pressTime, recognitionId});
            owner->outputHandler_->showStatus(
                target, "Ainput: recognizing...",
                [callbackGate, request = std::move(request)] {
                    withOwner(callbackGate, [&](AinputAddon &owner) {
                        owner.enqueueRecognition(std::move(*request));
                    });
                });
        });
        audioCapture_->setStateCallback([](bool active) {
            FCITX_INFO() << "Ainput ASR state: " << (active ? "on" : "off");
        });
        audioCapture_->setStatusTextCallback([callbackGate, target, recognitionId](const std::string &text) {
            ainput::diagnosticLog().event("adapter", "capture_status", {
                {"recognition_id", std::to_string(recognitionId)},
                {"status_hash", diagnosticHash(text)},
                {"status_length", std::to_string(text.size())}
            });
            withOwner(callbackGate, [&](AinputAddon &owner) {
                owner.outputHandler_->showStatus(target, text);
            });
        });

        playSound("activate");
        audioCapture_->start();
    }

    void enqueueRecognition(RecognitionRequest request) {
        ainput::diagnosticLog().event("adapter", "recognition_enqueue_attempt", {
            {"recognition_id", std::to_string(request.recognitionId)},
            {"provider", request.providerId},
            {"queue_size", std::to_string(recognitionQueue_.size())},
            {"active_id", activeRecognition_ ?
                std::to_string(activeRecognition_->recognitionId) : "0"}
        });
        if (shuttingDown_) {
            ainput::diagnosticLog().event("adapter", "recognition_dropped_shutdown", {
                {"recognition_id", std::to_string(request.recognitionId)}
            });
            unlink(request.wavPath.c_str());
            return;
        }
        if (recognitionQueue_.size() + (activeRecognition_ ? 1 : 0) >=
            kMaxPendingRecognitions) {
            unlink(request.wavPath.c_str());
            ainput::diagnosticLog().event("adapter", "recognition_dropped_queue_full", {
                {"recognition_id", std::to_string(request.recognitionId)},
                {"queue_size", std::to_string(recognitionQueue_.size())},
                {"active_id", activeRecognition_ ?
                    std::to_string(activeRecognition_->recognitionId) : "0"}
            });
            outputHandler_->showStatus(request.target,
                                       "Ainput: recognition queue full; try again");
            return;
        }
        recognitionQueue_.push_back(std::move(request));
        ainput::diagnosticLog().event("adapter", "recognition_enqueued", {
            {"recognition_id", std::to_string(recognitionQueue_.back().recognitionId)},
            {"queue_size", std::to_string(recognitionQueue_.size())}
        });
        dispatchNextRecognition();
    }

    bool ensureAsrProvider(const std::string &providerId) {
        if (asr_ && asrProviderId_ == providerId) return true;
        if (asr_) {
            ainput::diagnosticLog().event("adapter", "provider_replaced", {
                {"old_provider", asrProviderId_},
                {"new_provider", providerId},
                {"recognition_id", activeRecognition_ ?
                    std::to_string(activeRecognition_->recognitionId) : "0"}
            });
        }
        asr_.reset();
        asr_ = ainput::AsrProviderRegistry::instance().create(providerId);
        asrProviderId_ = asr_ ? providerId : std::string{};
        return static_cast<bool>(asr_);
    }

    void dispatchNextRecognition() {
        if (activeRecognition_ || recognitionQueue_.empty() || shuttingDown_) {
            ainput::diagnosticLog().event("adapter", "recognition_dispatch_blocked", {
                {"reason", activeRecognition_ ? "active" :
                           (recognitionQueue_.empty() ? "empty" : "shutting_down")},
                {"active_id", activeRecognition_ ?
                    std::to_string(activeRecognition_->recognitionId) : "0"},
                {"queue_size", std::to_string(recognitionQueue_.size())}
            });
            return;
        }
        activeRecognition_ = std::move(recognitionQueue_.front());
        recognitionQueue_.pop_front();
        ainput::diagnosticLog().event("adapter", "recognition_dispatch_begin", {
            {"recognition_id", std::to_string(activeRecognition_->recognitionId)},
            {"provider", activeRecognition_->providerId},
            {"wav_hash", diagnosticHash(activeRecognition_->wavPath)},
            {"queue_size", std::to_string(recognitionQueue_.size())}
        });

        if (!ensureAsrProvider(activeRecognition_->providerId)) {
            auto target = activeRecognition_->target;
            const auto recognitionId = activeRecognition_->recognitionId;
            unlink(activeRecognition_->wavPath.c_str());
            activeRecognition_.reset();
            ainput::diagnosticLog().event("adapter", "recognition_provider_unavailable", {
                {"recognition_id", std::to_string(recognitionId)}
            });
            outputHandler_->showStatus(target, "Ainput: ASR provider unavailable",
                                       [callbackGate = callbackGate_] {
                                           withOwner(callbackGate, [](AinputAddon &owner) {
                                               owner.dispatchNextRecognition();
                                           });
                                       });
            return;
        }

        auto target = activeRecognition_->target;
        auto tPress = activeRecognition_->pressTime;
        const auto recognitionId = activeRecognition_->recognitionId;
        const auto providerId = activeRecognition_->providerId;
        auto callbackGate = callbackGate_;
        asr_->setDiagnosticId(recognitionId);
        asr_->setResultCallback([callbackGate, target, tPress, recognitionId](const std::string &text, bool isFinal) {
            auto tResult = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tResult - tPress).count();
            fprintf(stderr, "Ainput [timer] press→result=%ldms\n", ms);
            FCITX_INFO() << "Ainput ASR result: text_len=" << text.size()
                         << " (final=" << isFinal << ")";
            ainput::diagnosticLog().event("adapter", "recognition_result_callback", {
                {"recognition_id", std::to_string(recognitionId)},
                {"is_final", isFinal ? "true" : "false"},
                {"text_length", std::to_string(text.size())},
                {"text_hash", diagnosticHash(text)}
            });
            withOwner(callbackGate, [&](AinputAddon &owner) {
                owner.outputHandler_->submit(target, text, [callbackGate, target] {
                    withOwner(callbackGate, [&](AinputAddon &owner) {
                        owner.outputHandler_->showStatus(target, "");
                        owner.finishRecognition();
                    });
                });
            });
        });
        asr_->setErrorCallback([callbackGate, target, recognitionId, providerId](
            const std::string &error,
            ainput::AsrErrorCategory category) {
            FCITX_INFO() << "Ainput ASR error: " << error
                         << " (category=" << (int)category << ")";
            ainput::diagnosticLog().event("adapter", "recognition_error_callback", {
                {"recognition_id", std::to_string(recognitionId)},
                {"category", std::to_string((int)category)},
                {"error_length", std::to_string(error.size())},
                {"error_hash", diagnosticHash(error)}
            });
            std::string status;
            switch (category) {
            case ainput::AsrErrorCategory::ConfigMissing:
                status = "Ainput: fill api_key in ~/.config/ainput/" +
                         providerId + ".json";
                break;
            case ainput::AsrErrorCategory::AuthRejected:
                status = "Ainput: API key rejected; check provider config";
                break;
            case ainput::AsrErrorCategory::ModelNotFound:
                status = "Ainput: model not found; check provider config";
                break;
            case ainput::AsrErrorCategory::LocalSetup:
                status = "Ainput: local engine setup error; check bin_path config";
                break;
            case ainput::AsrErrorCategory::Network:
                status = "Ainput: network error; try again";
                break;
            case ainput::AsrErrorCategory::ServiceUnavailable:
                status = "Ainput: recognition service unavailable; try again";
                break;
            case ainput::AsrErrorCategory::Timeout:
                status = "Ainput: recognition timed out; try again";
                break;
            case ainput::AsrErrorCategory::NoSpeech:
            case ainput::AsrErrorCategory::EmptyResult:
                status = "Ainput: no speech recognized";
                break;
            case ainput::AsrErrorCategory::AudioData:
                status = "Ainput: audio capture error";
                break;
            case ainput::AsrErrorCategory::ConfigInvalid:
            case ainput::AsrErrorCategory::InvalidRequest:
            case ainput::AsrErrorCategory::Runtime:
            case ainput::AsrErrorCategory::Unknown:
            default: {
                // Last resort: surface the provider's detail so nothing is a
                // dead-end "failed". Collapse to one trimmed line.
                std::string detail = error;
                for (char &c : detail) {
                    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                }
                if (detail.size() > 120) detail = detail.substr(0, 120) + "...";
                status = "Ainput: " + detail;
                break;
            }
            }
            withOwner(callbackGate, [&](AinputAddon &owner) {
                owner.outputHandler_->showStatus(target, status, [callbackGate] {
                    withOwner(callbackGate, [](AinputAddon &owner) {
                        owner.finishRecognition();
                    });
                });
            });
        });
        asr_->transcribe(std::move(activeRecognition_->samples),
                         activeRecognition_->wavPath);
        ainput::diagnosticLog().event("adapter", "recognition_provider_called", {
            {"recognition_id", std::to_string(recognitionId)},
            {"provider", activeRecognition_->providerId}
        });
    }

    void finishRecognition() {
        const auto recognitionId = activeRecognition_ ? activeRecognition_->recognitionId : 0;
        ainput::diagnosticLog().event("adapter", "recognition_finished", {
            {"recognition_id", std::to_string(recognitionId)},
            {"queue_size", std::to_string(recognitionQueue_.size())}
        });
        activeRecognition_.reset();
        dispatchNextRecognition();
    }

    void reapFinishedCaptures() {
        std::erase_if(finishingCaptures_, [](const auto &capture) {
            return capture->finished();
        });
    }

    // 松键后结束
    void onDeactivate() {
        active_ = false;
        tStop_ = std::chrono::steady_clock::now();
        auto recMs = std::chrono::duration_cast<std::chrono::milliseconds>(tStop_ - tActivate_).count();
        FCITX_INFO() << "Ainput deactivated (record=" << recMs << "ms)";
        ainput::diagnosticLog().event("adapter", "capture_stop_requested", {
            {"recognition_id", std::to_string(currentRecognitionId_)},
            {"record_ms", std::to_string(recMs)}
        });

        if (audioCapture_) {
            outputHandler_->showStatus(currentTarget_, "Ainput: processing audio...");
            audioCapture_->stop();
            finishingCaptures_.push_back(std::move(audioCapture_));
        }
        currentIC_ = nullptr;

        // commit 最后收到的 preedit 文本
        if (!lastPreeditText_.empty()) {
            auto *ic = instance_->inputContextManager().findByUUID(currentUuid_);
            if (ic) {
                ic->inputPanel().reset();
                ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
                ic->commitString(lastPreeditText_);
        FCITX_INFO() << "Ainput final commit: text_len=" << lastPreeditText_.size();
            }
            lastPreeditText_.clear();
        }

        playSound("deactivate");  // 结束音: 低音

        // 松键后还原 CapsLock (按下时硬件层已切换, 现在补一个假按键还原)
        revertDebounce_ = debounceCount_;
        revertCapsLock();
    }

};

// AinputFactory — Ainput 插件的工厂类
class AinputFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new AinputAddon(manager->instance());
    }
};

FCITX_ADDON_FACTORY(AinputFactory);
