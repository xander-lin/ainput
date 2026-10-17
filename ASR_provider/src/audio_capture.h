#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include <functional>
#include <pulse/pulseaudio.h>

namespace ainput {

class AudioCapture {
public:
    using StateCallback = std::function<void(bool active)>;
    using StatusTextCallback = std::function<void(const std::string &)>;
    using RecordedCallback = std::function<void(const std::vector<int16_t>&, const std::string&)>;

    AudioCapture();
    ~AudioCapture();

    void start();
    void stop();
    void wait();

    bool recording() const { return recordThread_.joinable() && !stopRequested_; }
    bool finished() const { return finished_; }

    std::vector<int16_t> takeSamples();
    const std::string& wavPath() const { return wavPath_; }

    void setStateCallback(StateCallback cb) { onState_ = std::move(cb); }
    void setStatusTextCallback(StatusTextCallback cb) { onStatusText_ = std::move(cb); }
    void setRecordedCallback(RecordedCallback cb) { onRecorded_ = std::move(cb); }
    void setDiagnosticId(uint64_t id) { diagnosticId_ = id; }

    void setDenoiseMethod(const std::string &method) { denoiseMethod_ = method; }

    static void processSamples(std::vector<int16_t> &samples, const std::string &denoiser);
    static void writeWav(const std::vector<int16_t> &samples, const std::string &path);

private:
    void recordLoop();
    static void contextStateCallback(pa_context *context, void *userdata);
    static void streamStateCallback(pa_stream *stream, void *userdata);
    static void streamReadCallback(pa_stream *stream, size_t bytes, void *userdata);
    static void applyDenoise(std::vector<int16_t> &samples, const std::string &method);
    static void dfDenoise(std::vector<int16_t> &samples);
    static double normalizeSamples(std::vector<int16_t> &samples);
    static bool hasVoice(const std::vector<int16_t> &samples);
    static void trimSilence(std::vector<int16_t> &samples);

    std::thread recordThread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> finished_{true};
    std::mutex pulseMutex_;
    pa_mainloop *pulseMainloop_ = nullptr;
    std::mutex sampleMutex_;
    std::vector<int16_t> samples_;
    std::string wavPath_;
    uint64_t captureId_ = 0;
    uint64_t diagnosticId_ = 0;
    static double lufsTarget_;
    static int speexLevel_;
    static double crestThreshold_;
    std::string denoiseMethod_;

    StateCallback onState_;
    StatusTextCallback onStatusText_;
    RecordedCallback onRecorded_;
};

} // namespace ainput
