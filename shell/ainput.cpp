// Ainput — fcitx5 voice input addon.
//
// This file is the C++ shell: everything that touches fcitx5 types lives here.
// Audio capture, DSP, ASR protocols, configuration files and the uinput device
// are implemented in the Rust core (rust/), reached through the small C ABI in
// ainput_core.h.

#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx-config/enum.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>

#include <fcitx-utils/event.h>
#include <fcitx-utils/eventloopinterface.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpaths.h>

#include <fcitx-module/notifications/notifications_public.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ainput_core.h"
#include "output_handler.h"

using Clock = std::chrono::steady_clock;

namespace fcitx {
// Explicit specialization: the macro version adds an overload of
// configTypeNameHelper, which two-phase lookup cannot find for a fundamental
// type. Specializing the class template instead always works.
template <>
struct OptionTypeName<double> {
    static std::string get() { return "Double"; }
};

struct DoubleMarshaller {
    void marshall(RawConfig &config, double value) const {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%g", value);
        config = buffer;
    }

    bool unmarshall(double &value, const RawConfig &config, bool) const {
        const std::string text = config.value();
        if (text.empty()) {
            return false;
        }
        try {
            std::size_t consumed = 0;
            const double parsed = std::stod(text, &consumed);
            if (consumed != text.size() || !std::isfinite(parsed)) {
                return false;
            }
            value = parsed;
            return true;
        } catch (const std::exception &) {
            return false;
        }
    }
};
} // namespace fcitx

namespace {

/// A recording that never sees a release event is force-stopped after this
/// long, so a dropped key event cannot leave the addon stuck in "listening".
constexpr auto kMaxRecordingTime = std::chrono::seconds(120);
/// Idle time after which a stale key debounce counter is cleared, so one lost
/// synthetic event cannot swallow every later CapsLock press.
constexpr auto kDebounceIdleReset = std::chrono::seconds(10);
/// CapsLock events ignored after the restore tap: one press + one release.
/// Not a user setting — changing it only makes sense when a compositor
/// duplicates the synthetic event, and that is better handled in code.
constexpr int kRestoreDebounceEvents = 2;


FCITX_CONFIG_ENUM(AinputDenoise, none, speexdsp, deepfilter);

FCITX_CONFIGURATION(
    AinputConfig,
    fcitx::Option<std::string> provider{
        this, "Provider", "ASR backend (qwen, doubao, zipformer, fire_red, mock)",
        "qwen"};
    fcitx::Option<int, fcitx::IntConstrain> activationMsec{
        this, "ActivationMsec", "How long CapsLock must be held (ms)", 300,
        fcitx::IntConstrain(50, 5000)};
    fcitx::Option<int, fcitx::IntConstrain> notificationTimeout{
        this, "NotificationTimeout",
        "How long the Ctrl+CapsLock switch notification stays on screen (ms)",
        2000, fcitx::IntConstrain(200, 20000)};
    // PulseAudio hands us audio in fragments of this size. Larger fragments
    // buffer more audio (safer against clipping the last syllable on devices
    // with bursty delivery) but add latency before processing starts.
    // There is no auto-detection: it is a plain setting so a device that
    // clips or stutters can be fixed by hand.
    fcitx::Option<int, fcitx::IntConstrain> bufferMs{
        this, "BufferMs",
        "Microphone buffer in ms. Raise if the end of a phrase is cut off, "
        "lower for quicker response",
        100, fcitx::IntConstrain(20, 1000)};
    fcitx::Option<AinputDenoise> denoise{
        this, "Denoise", "Noise reduction method", AinputDenoise::none};
    fcitx::Option<double, fcitx::NoConstrain<double>, fcitx::DoubleMarshaller> lufsTarget{
        this, "LufsTarget", "Loudness normalization target (LUFS)", -16.0};
    fcitx::Option<int, fcitx::IntConstrain> speexLevel{
        this, "SpeexLevel", "speexdsp suppression strength (dB)", -15,
        fcitx::IntConstrain(-100, 0)};
    fcitx::Option<double, fcitx::NoConstrain<double>, fcitx::DoubleMarshaller> crestThreshold{
        this, "CrestThreshold", "Speech gate; 0 disables", 2.4};

    // ---- credentials and model ids ----
    // These override the values in ~/.config/ainput/<provider>.json. An empty
    // field means "use the file", so an existing setup keeps working and the
    // GUI is the obvious place for a new one.
    fcitx::Option<std::string> qwenApiKey{
        this, "QwenApiKey", "Qwen (DashScope) API key", ""};
    fcitx::Option<std::string> qwenModel{
        this, "QwenModel",
        "Qwen model id (empty = qwen-audio-3.1-asr-flash)", ""};
    fcitx::Option<std::string> doubaoApiKey{
        this, "DoubaoApiKey", "Doubao (Volcengine) API key", ""};
    fcitx::Option<std::string> doubaoModel{
        this, "DoubaoModel", "Doubao model name (empty = bigmodel)", ""};
);

int32_t denoiseToInt(AinputDenoise value) {
    switch (value) {
    case AinputDenoise::speexdsp:
        return AINPUT_DENOISE_SPEEXDSP;
    case AinputDenoise::deepfilter:
        return AINPUT_DENOISE_DEEPFILTER;
    case AinputDenoise::none:
    default:
        return AINPUT_DENOISE_NONE;
    }
}

} // namespace

class AinputAddon : public fcitx::AddonInstance {
public:
    explicit AinputAddon(fcitx::Instance *instance) : instance_(instance) {
        reloadConfig();

        AinputCallbacks callbacks{};
        callbacks.user = this;
        callbacks.on_text = &AinputAddon::onTextThunk;
        callbacks.on_error = &AinputAddon::onErrorThunk;
        engine_ = ainput_engine_new(&callbacks);

        output_ = std::make_unique<ainput::OutputHandler>(instance_);

        migrateLegacyConfig();
        pushSettings();
        warnIfUinputUnavailable();

        keyWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextKeyEvent,
            fcitx::EventWatcherPhase::PreInputMethod,
            [this](fcitx::Event &event) {
                onKeyEvent(static_cast<fcitx::KeyEvent &>(event));
            });

        watchdog_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 1000000, 0,
            [this](fcitx::EventSourceTime *, uint64_t) {
                checkStaleState();
                return true;
            });

        FCITX_INFO() << "Ainput loaded (Rust core)";
    }

    ~AinputAddon() override {
        if (watchdog_) {
            watchdog_.reset();
        }
        keyWatcher_.reset();
        if (engine_) {
            ainput_engine_shutdown(engine_);
            ainput_engine_free(engine_);
            engine_ = nullptr;
        }
        output_.reset();
    }

    // ---- fcitx5 configuration interface ----

    const fcitx::Configuration *getConfig() const override { return &config_; }

    void setConfig(const fcitx::RawConfig &rawConfig) override {
        config_.load(rawConfig, true);
        fcitx::safeSaveAsIni(config_, fcitx::StandardPathsType::PkgConfig, "conf/ainput.conf");
        pushSettings();
    }

    void reloadConfig() override {
        fcitx::readAsIni(config_, fcitx::StandardPathsType::PkgConfig, "conf/ainput.conf");
    }

    void save() override {
        fcitx::safeSaveAsIni(config_, fcitx::StandardPathsType::PkgConfig, "conf/ainput.conf");
    }

private:
    // ---- Rust callbacks (any thread) ----

    static void onTextThunk(void *user, uint64_t id, const char *text) {
        static_cast<AinputAddon *>(user)->onText(id, text);
    }
    static void onErrorThunk(void *user, uint64_t id, int32_t category,
                             const char *detail) {
        static_cast<AinputAddon *>(user)->onError(id, category, detail);
    }

    ainput::OutputTarget takeTarget(uint64_t id) {
        std::lock_guard<std::mutex> lock(targetMutex_);
        auto it = targets_.find(id);
        if (it != targets_.end()) {
            auto target = it->second;
            targets_.erase(it);
            return target;
        }
        // Queue-full and startup errors arrive without a recording id; deliver
        // them to whatever is focused now.
        return output_->captureCurrentUuid(id);
    }

    void onText(uint64_t id, const char *text) {
        auto target = takeTarget(id);
        output_->submit(target, text ? text : "");
    }

    void onError(uint64_t id, int32_t category, const char *detail) {
        const std::string message = statusForError(category, detail ? detail : "");
        auto target = takeTarget(id);
        output_->showStatus(target, message);
    }

    static std::string statusForError(int32_t category, const std::string &detail) {
        switch (category) {
        case AINPUT_ERR_CONFIG_MISSING:
            return "Ainput: " + detail;
        case AINPUT_ERR_CONFIG_INVALID:
            return "Ainput: " + detail;
        case AINPUT_ERR_AUTH_REJECTED:
            return "Ainput: API key rejected; check provider config";
        case AINPUT_ERR_MODEL_NOT_FOUND:
            return "Ainput: model not found; check provider config";
        case AINPUT_ERR_LOCAL_SETUP:
            return "Ainput: local engine setup error; " + detail;
        case AINPUT_ERR_NETWORK:
            return "Ainput: network error; try again";
        case AINPUT_ERR_SERVICE_UNAVAILABLE:
            return "Ainput: recognition service unavailable; try again";
        case AINPUT_ERR_TIMEOUT:
            return "Ainput: recognition timed out; try again";
        case AINPUT_ERR_NO_SPEECH:
        case AINPUT_ERR_EMPTY_RESULT:
            return "Ainput: no speech recognized";
        case AINPUT_ERR_AUDIO_DATA:
            return "Ainput: " + detail;
        case AINPUT_ERR_INVALID_REQUEST:
        case AINPUT_ERR_RUNTIME:
        case AINPUT_ERR_UNKNOWN:
        default: {
            std::string oneLine = detail;
            for (char &ch : oneLine) {
                if (ch == '\n' || ch == '\r' || ch == '\t') {
                    ch = ' ';
                }
            }
            if (oneLine.size() > 120) {
                oneLine = oneLine.substr(0, 120) + "...";
            }
            return "Ainput: " + oneLine;
        }
        }
    }

    // ---- configuration plumbing ----

    void pushSettings() {
        if (!engine_) {
            return;
        }
        const std::string provider = config_.provider.value();
        AinputSettings settings{};
        settings.provider = provider.c_str();
        settings.activation_msec = config_.activationMsec.value();
        settings.notification_timeout = config_.notificationTimeout.value();
        settings.buffer_ms = config_.bufferMs.value();
        settings.denoise = denoiseToInt(config_.denoise.value());
        settings.lufs_target = config_.lufsTarget.value();
        settings.speex_level = config_.speexLevel.value();
        settings.crest_threshold = config_.crestThreshold.value();
        settings.qwen_api_key = config_.qwenApiKey.value().c_str();
        settings.qwen_model = config_.qwenModel.value().c_str();
        settings.doubao_api_key = config_.doubaoApiKey.value().c_str();
        settings.doubao_model = config_.doubaoModel.value().c_str();
        ainput_engine_set_settings(engine_, &settings);
    }

    void migrateLegacyConfig() {
        AinputSettings legacy{};
        char report[512] = {0};
        const int result = ainput_migrate_legacy_config(&legacy, report, sizeof(report));
        if (result != 1) {
            if (result < 0) {
                FCITX_WARN() << "Ainput: legacy config migration failed: " << report;
            }
            return;
        }

        if (legacy.provider && *legacy.provider) {
            config_.provider.setValue(legacy.provider);
        }
        config_.activationMsec.setValue(legacy.activation_msec);
        config_.notificationTimeout.setValue(legacy.notification_timeout);
        switch (legacy.denoise) {
        case AINPUT_DENOISE_SPEEXDSP:
            config_.denoise.setValue(AinputDenoise::speexdsp);
            break;
        case AINPUT_DENOISE_DEEPFILTER:
            config_.denoise.setValue(AinputDenoise::deepfilter);
            break;
        default:
            config_.denoise.setValue(AinputDenoise::none);
            break;
        }
        config_.lufsTarget.setValue(legacy.lufs_target);
        config_.speexLevel.setValue(legacy.speex_level);
        config_.crestThreshold.setValue(legacy.crest_threshold);
        fcitx::safeSaveAsIni(config_, fcitx::StandardPathsType::PkgConfig, "conf/ainput.conf");

        // Rename rather than delete: the values are now in fcitx5's config,
        // and a leftover config.json would be migrated again on every start,
        // overwriting whatever the user changed in the config UI since.
        renameLegacyConfig();
        FCITX_WARN() << "Ainput: " << report;
        output_->showStatus(output_->captureCurrentUuid(0),
                            "Ainput: settings migrated to fcitx5 configuration");
    }

    void renameLegacyConfig() {
        const char *home = std::getenv("HOME");
        if (!home) {
            return;
        }
        namespace fs = std::filesystem;
        const fs::path dir = fs::path(home) / ".config/ainput";
        const fs::path from = dir / "config.json";
        std::error_code ec;
        if (!fs::exists(from, ec)) {
            return;
        }
        fs::path to = dir / "config.json.migrated";
        for (int suffix = 1; fs::exists(to, ec) && suffix < 100; ++suffix) {
            to = dir / ("config.json.migrated." + std::to_string(suffix));
        }
        fs::rename(from, to, ec);
        if (ec) {
            FCITX_WARN() << "Ainput: cannot rename " << from.string() << ": "
                         << ec.message()
                         << " (it will be migrated again next start)";
        } else {
            FCITX_INFO() << "Ainput: legacy config kept as " << to.string();
        }
    }

    void warnIfUinputUnavailable() {
        if (ainput_uinput_open()) {
            return;
        }
        char message[512] = {0};
        ainput_uinput_error(message, sizeof(message));
        FCITX_WARN() << "Ainput: " << message;
        output_->showStatus(output_->captureCurrentUuid(0),
                            "Ainput: CapsLock restore disabled (see log)");
    }

    // ---- providers ----

    struct ProviderEntry {
        std::string id;
        std::string label;
    };

    static std::vector<ProviderEntry> providers() {
        std::vector<ProviderEntry> list;
        const size_t count = ainput_provider_count();
        for (size_t index = 0; index < count; ++index) {
            char idBuffer[64] = {0};
            char labelBuffer[512] = {0};
            if (ainput_provider_id(index, idBuffer, sizeof(idBuffer)) < 0) {
                continue;
            }
            ainput_provider_display(index, labelBuffer, sizeof(labelBuffer));
            list.push_back({idBuffer, labelBuffer});
        }
        return list;
    }

    int providerIndex(const std::vector<ProviderEntry> &list) const {
        const auto current = config_.provider.value();
        for (size_t index = 0; index < list.size(); ++index) {
            if (list[index].id == current) {
                return static_cast<int>(index);
            }
        }
        return 0;
    }

    void persistProvider(const std::string &id) {
        config_.provider.setValue(id);
        fcitx::safeSaveAsIni(config_, fcitx::StandardPathsType::PkgConfig, "conf/ainput.conf");
        pushSettings();
    }

    void persistDenoise(AinputDenoise value) {
        config_.denoise.setValue(value);
        fcitx::safeSaveAsIni(config_, fcitx::StandardPathsType::PkgConfig, "conf/ainput.conf");
        pushSettings();
    }

    void notify(const std::string &message) {
        auto *notifications = instance_->addonManager().addon("notifications", true);
        if (!notifications) {
            return;
        }
        notifications->call<fcitx::INotifications::sendNotification>(
            "fcitx5-ainput", 0, "fcitx-ainput", "Ainput", message,
            std::vector<std::string>{}, config_.notificationTimeout.value(),
            nullptr, nullptr);
    }

    void switchProvider(int direction) {
        auto list = providers();
        if (list.empty()) {
            return;
        }
        int index = providerIndex(list);
        index = (index + direction + static_cast<int>(list.size())) %
                static_cast<int>(list.size());
        persistProvider(list[index].id);
        notify("ASR: " + list[index].label + " (" + std::to_string(index + 1) +
               "/" + std::to_string(list.size()) + ")");
    }

    void switchDenoise(int direction) {
        static const std::vector<AinputDenoise> order = {
            AinputDenoise::none, AinputDenoise::speexdsp,
            AinputDenoise::deepfilter};
        static const std::vector<std::string> names = {"none", "speexdsp",
                                                       "deepfilter"};
        const auto current = config_.denoise.value();
        auto it = std::find(order.begin(), order.end(), current);
        int index = it == order.end() ? 0 : static_cast<int>(it - order.begin());
        index = (index + direction + static_cast<int>(order.size())) %
                static_cast<int>(order.size());
        persistDenoise(order[index]);
        notify("Denoiser: " + names[index] + " (" + std::to_string(index + 1) +
               "/" + std::to_string(order.size()) + ")");
    }

    // ---- key handling ----

    void onKeyEvent(fcitx::KeyEvent &keyEvent) {
        const bool capsLock = keyEvent.key().sym() == FcitxKey_Caps_Lock;
        if (!capsLock && !active_ && !switchActive_ && !timer_) {
            return;
        }

        if (capsLock) {
            lastCapsEvent_ = Clock::now();
            if (revertDebounce_ > 0) {
                --revertDebounce_;
                return;
            }
        }

        if (keyEvent.isRelease()) {
            if (!capsLock) {
                return;
            }
            FCITX_DEBUG() << "Ainput: CapsLock release (active=" << active_
                          << " timer=" << (timer_ != nullptr) << ")";
            if (active_) {
                deactivate();
            } else if (switchActive_) {
                switchActive_ = false;
                revertDebounce_ = kRestoreDebounceEvents;
                ainput_uinput_restore_capslock();
            } else {
                timer_.reset();
            }
            keyEvent.filterAndAccept();
            return;
        }

        if (!capsLock) {
            if (switchActive_) {
                if (const int direction = horizontalDirection(keyEvent)) {
                    switchProvider(direction);
                    keyEvent.filterAndAccept();
                } else if (const int direction = verticalDirection(keyEvent)) {
                    switchDenoise(direction);
                    keyEvent.filterAndAccept();
                }
            }
            return;
        }

        if (timer_ || active_ || switchActive_) {
            return;
        }

        pressTime_ = Clock::now();
        FCITX_DEBUG() << "Ainput: CapsLock press";
        const bool ctrlHeld =
            (keyEvent.key().states().toInteger() &
             static_cast<uint32_t>(fcitx::KeyState::Ctrl)) != 0;

        if (ctrlHeld) {
            switchActive_ = true;
            auto list = providers();
            if (!list.empty()) {
                const int index = providerIndex(list);
                notify("ASR: " + list[index].label + " (" +
                       std::to_string(index + 1) + "/" +
                       std::to_string(list.size()) + ")");
            }
            keyEvent.filterAndAccept();
            return;
        }

        const auto delay = static_cast<uint64_t>(config_.activationMsec.value()) * 1000;
        timer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + delay, 0,
            [this](fcitx::EventSourceTime *, uint64_t) {
                activate();
                return false;
            });
        keyEvent.filterAndAccept();
    }

    static int horizontalDirection(const fcitx::KeyEvent &keyEvent) {
        switch (keyEvent.key().sym()) {
        case FcitxKey_Left:
        case FcitxKey_h:
            return -1;
        case FcitxKey_Right:
        case FcitxKey_l:
            return 1;
        default:
            return 0;
        }
    }

    static int verticalDirection(const fcitx::KeyEvent &keyEvent) {
        switch (keyEvent.key().sym()) {
        case FcitxKey_Up:
        case FcitxKey_k:
            return -1;
        case FcitxKey_Down:
        case FcitxKey_j:
            return 1;
        default:
            return 0;
        }
    }

    // ---- recording lifecycle ----

    void activate() {
        timer_.reset();
        if (!engine_) {
            return;
        }
        if (!instance_->mostRecentInputContext()) {
            FCITX_INFO() << "Ainput: no input context, ignoring activation";
            return;
        }

        activeId_ = ainput_engine_start(engine_);
        FCITX_INFO() << "Ainput: recording started, id=" << activeId_;
        if (activeId_ == 0) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            targets_[activeId_] = output_->captureCurrentUuid(activeId_);
        }
        active_ = true;
        activeSince_ = Clock::now();
        output_->showStatus(targetFor(activeId_), "Ainput: listening...");
    }

    ainput::OutputTarget targetFor(uint64_t id) {
        std::lock_guard<std::mutex> lock(targetMutex_);
        auto it = targets_.find(id);
        if (it != targets_.end()) {
            return it->second;
        }
        return output_->captureCurrentUuid(id);
    }

    void deactivate() {
        if (!active_) {
            return;
        }
        const auto id = activeId_;
        active_ = false;
        activeId_ = 0;
        output_->showStatus(targetFor(id), "Ainput: processing audio...");
        ainput_engine_stop(engine_, id);
        revertDebounce_ = kRestoreDebounceEvents;
    }

    /// Recovers from a dropped key event: a recording that outlived
    /// kMaxRecordingTime is stopped, and a debounce counter that has had no
    /// CapsLock traffic for kDebounceIdleReset is cleared.
    void checkStaleState() {
        const auto now = Clock::now();
        if (active_ && now - activeSince_ > kMaxRecordingTime) {
            FCITX_WARN() << "Ainput: recording exceeded the time limit, stopping";
            deactivate();
        }
        if (revertDebounce_ > 0 && now - lastCapsEvent_ > kDebounceIdleReset) {
            FCITX_WARN() << "Ainput: clearing a stale key debounce counter";
            revertDebounce_ = 0;
        }
    }

    fcitx::Instance *instance_;
    AinputConfig config_;
    AinputEngine *engine_ = nullptr;
    std::unique_ptr<ainput::OutputHandler> output_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> keyWatcher_;
    std::unique_ptr<fcitx::EventSourceTime> watchdog_;
    std::unique_ptr<fcitx::EventSourceTime> timer_;

    std::mutex targetMutex_;
    std::unordered_map<uint64_t, ainput::OutputTarget> targets_;

    bool active_ = false;
    bool switchActive_ = false;
    uint64_t activeId_ = 0;
    int revertDebounce_ = 0;
    Clock::time_point pressTime_{};
    Clock::time_point activeSince_{};
    Clock::time_point lastCapsEvent_{};
};

class AinputFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new AinputAddon(manager->instance());
    }
};

FCITX_ADDON_FACTORY(AinputFactory);
