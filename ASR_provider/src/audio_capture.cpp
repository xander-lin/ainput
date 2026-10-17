#include "audio_capture.h"
#include "config_schema.h"
#include "ainput_config.h"
#include "diagnostic_log.h"

#include <pulse/mainloop.h>
#include <pulse/error.h>
#include <speex/speex_preprocess.h>
#include <soxr.h>
#include <unistd.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <ebur128.h>
#include <cstdlib>

namespace ainput {

namespace {
std::atomic<uint64_t> nextCaptureId{0};
std::mutex deepFilterMutex;
std::mutex pulseCaptureMutex;
std::mutex audioProcessingMutex;
std::condition_variable audioProcessingReady;
uint64_t nextProcessingTicket = 0;
uint64_t nextProcessingId = 0;

class ProcessingTurn {
public:
    explicit ProcessingTurn(uint64_t id) : lock_(audioProcessingMutex) {
        audioProcessingReady.wait(lock_, [id] { return id == nextProcessingId; });
    }

    ~ProcessingTurn() {
        ++nextProcessingId;
        lock_.unlock();
        audioProcessingReady.notify_all();
    }

private:
    std::unique_lock<std::mutex> lock_;
};
}

double AudioCapture::lufsTarget_ = -16.0;
int AudioCapture::speexLevel_ = -15;
double AudioCapture::crestThreshold_ = 2.4;

static std::string jsonGetString(const std::string &json, const std::string &key) {
    std::string q = "\"" + key + "\"";
    auto pos = json.find(q);
    if (pos == std::string::npos) return "";
    pos += q.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t'))
        pos++;
    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        pos++;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }

    if (json[pos] == 't' || json[pos] == 'f') {
        return json.substr(pos, json.find_first_of(",}\n\r \t", pos) - pos);
    }

    return "";
}

static void loadAudioConfig(std::string &denoiseMethod) {
    auto val = jsonGetString(readConfigSection("config.json", "audio"), "denoise");
    if (val == "deepfilter") denoiseMethod = "deepfilter";
    else if (val == "speexdsp") denoiseMethod = "speexdsp";
    else if (val == "true") denoiseMethod = "speexdsp";
}

AudioCapture::AudioCapture() {
    loadAudioConfig(denoiseMethod_);
    if (!denoiseMethod_.empty())
        fprintf(stderr, "Ainput Capture: denoise=%s\n", denoiseMethod_.c_str());
    diagnosticLog().event("audio", "capture_created", {
        {"denoiser", denoiseMethod_.empty() ? "none" : denoiseMethod_}
    });

    static bool configLoaded = false;
    if (!configLoaded) {
        auto cfg = readConfigSection("config.json", "audio");
        if (!cfg.empty()) {
            reportConfigIssues("capture", "config.json [audio]",
                               validateConfigJson(cfg, audioConfigSchema()));
            auto t = jsonDouble(cfg, "lufs_target", lufsTarget_);
            if (t > -100.0 && t < 0.0) lufsTarget_ = t;
            speexLevel_ = jsonInt(cfg, "speex_level", speexLevel_);
            crestThreshold_ = jsonDouble(cfg, "crest_threshold", crestThreshold_);
            fprintf(stderr, "Ainput Capture: config.json [audio] parsed: crest_threshold=%.2f lufs_target=%.1f speex_level=%d\n",
                    crestThreshold_, lufsTarget_, speexLevel_);
        } else {
            fprintf(stderr, "Ainput Capture: config.json [audio] absent, using defaults\n");
        }
        configLoaded = true;
    }
}

AudioCapture::~AudioCapture() {
    stop();
    wait();
}

void AudioCapture::processSamples(std::vector<int16_t> &samples, const std::string &denoiser) {
    auto t0 = std::chrono::steady_clock::now();

    double loudness = normalizeSamples(samples);
    bool isBlank = !hasVoice(samples);

    bool wantDenoise = !denoiser.empty() && denoiser != "none";
    if (!isBlank && wantDenoise) applyDenoise(samples, denoiser);

    trimSilence(samples);

    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "Ainput Pipeline [summary] loudness=%.1f isBlank=%d denoiser=%s time=%ldms samples=%zu\n",
            loudness, (int)isBlank, denoiser.c_str(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            samples.size());
}

void AudioCapture::start() {
    if (recordThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.clear();
    }
    stopRequested_ = false;
    finished_ = false;
    captureId_ = nextCaptureId.fetch_add(1);
    wavPath_ = "/tmp/ainput_cap_" + std::to_string(getpid()) + "_" +
               std::to_string(captureId_) + ".wav";
    diagnosticLog().event("audio", "capture_start", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)},
        {"wav_hash", hashDiagnosticValue(wavPath_).substr(0, 16)}
    });
    if (onState_) onState_(true);
    recordThread_ = std::thread(&AudioCapture::recordLoop, this);
}

void AudioCapture::stop() {
    stopRequested_ = true;
    diagnosticLog().event("audio", "capture_stop", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)}
    });
    std::lock_guard<std::mutex> lock(pulseMutex_);
    if (pulseMainloop_) pa_mainloop_wakeup(pulseMainloop_);
}

void AudioCapture::wait() {
    if (recordThread_.joinable()) recordThread_.join();
}

void AudioCapture::contextStateCallback(pa_context *, void *userdata) {
    auto *self = static_cast<AudioCapture *>(userdata);
    std::lock_guard<std::mutex> lock(self->pulseMutex_);
    if (self->pulseMainloop_) pa_mainloop_wakeup(self->pulseMainloop_);
}

void AudioCapture::streamStateCallback(pa_stream *, void *userdata) {
    auto *self = static_cast<AudioCapture *>(userdata);
    std::lock_guard<std::mutex> lock(self->pulseMutex_);
    if (self->pulseMainloop_) pa_mainloop_wakeup(self->pulseMainloop_);
}

void AudioCapture::streamReadCallback(pa_stream *stream, size_t, void *userdata) {
    auto *self = static_cast<AudioCapture *>(userdata);
    while (true) {
        const void *data = nullptr;
        size_t bytes = 0;
        if (pa_stream_peek(stream, &data, &bytes) < 0 || bytes == 0) break;
        if (data) {
            const auto *samples = static_cast<const int16_t *>(data);
            self->samples_.insert(self->samples_.end(), samples,
                                  samples + bytes / sizeof(int16_t));
        }
        if (pa_stream_drop(stream) < 0) break;
    }
}

void AudioCapture::recordLoop() {
    std::unique_lock<std::mutex> pulseSessionLock(pulseCaptureMutex);
    const uint64_t processingTicket = nextProcessingTicket++;
    diagnosticLog().event("audio", "capture_processing_ticket_assigned", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)},
        {"ticket", std::to_string(processingTicket)}
    });
    const pa_sample_spec ss{PA_SAMPLE_S16LE, 16000, 1};
    auto t0 = std::chrono::steady_clock::now();
    pa_mainloop *mainloop = pa_mainloop_new();
    pa_context *context = nullptr;
    pa_stream *stream = nullptr;
    bool readFailed = false;
    int error = PA_OK;
    int nReads = 0;

    if (!mainloop) {
        readFailed = true;
    } else {
        {
            std::lock_guard<std::mutex> lock(pulseMutex_);
            pulseMainloop_ = mainloop;
        }
        context = pa_context_new(pa_mainloop_get_api(mainloop), "ainput-cap");
        if (!context) {
            readFailed = true;
        } else {
            pa_context_set_state_callback(context, contextStateCallback, this);
            if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
                readFailed = true;
            }
        }
    }

    while (!readFailed && !stopRequested_) {
        auto state = pa_context_get_state(context);
        if (state == PA_CONTEXT_READY) break;
        if (!PA_CONTEXT_IS_GOOD(state) || pa_mainloop_iterate(mainloop, 1, nullptr) < 0) {
            readFailed = true;
        }
    }

    if (!readFailed && !stopRequested_) {
        stream = pa_stream_new(context, "voice", &ss, nullptr);
        if (!stream) {
            readFailed = true;
        } else {
            pa_stream_set_state_callback(stream, streamStateCallback, this);
            pa_stream_set_read_callback(stream, streamReadCallback, this);
            pa_buffer_attr attr{};
            attr.maxlength = static_cast<uint32_t>(-1);
            attr.fragsize = 3200; // 100 ms at 16 kHz mono S16.
            if (pa_stream_connect_record(stream, nullptr, &attr,
                                         PA_STREAM_ADJUST_LATENCY) < 0) {
                readFailed = true;
            }
        }
    }

    while (!readFailed && !stopRequested_) {
        auto state = pa_stream_get_state(stream);
        if (state == PA_STREAM_READY) break;
        if (!PA_STREAM_IS_GOOD(state) || pa_mainloop_iterate(mainloop, 1, nullptr) < 0) {
            readFailed = true;
        }
    }

    auto tPaOpen = std::chrono::steady_clock::now();
    bool stopping = stopRequested_;
    auto stopDeadline = std::chrono::steady_clock::time_point::max();
    while (!readFailed && stream) {
        if (stopRequested_ && !stopping) stopping = true;
        if (stopping && stopDeadline == std::chrono::steady_clock::time_point::max()) {
            stopDeadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(120);
        }
        if (stopping && std::chrono::steady_clock::now() >= stopDeadline) break;

        int dispatched = pa_mainloop_iterate(mainloop, stopping ? 0 : 1, nullptr);
        if (dispatched < 0 || !PA_STREAM_IS_GOOD(pa_stream_get_state(stream))) {
            readFailed = true;
            break;
        }
        if (dispatched > 0) nReads += dispatched;
        if (stopping) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto tRecordEnd = std::chrono::steady_clock::now();

    if (stream) {
        pa_stream_set_read_callback(stream, nullptr, nullptr);
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
    }
    if (context) {
        error = pa_context_errno(context);
        pa_context_disconnect(context);
        pa_context_unref(context);
    }
    {
        std::lock_guard<std::mutex> lock(pulseMutex_);
        pulseMainloop_ = nullptr;
    }
    if (mainloop) pa_mainloop_free(mainloop);
    pulseSessionLock.unlock();
    diagnosticLog().event("audio", "capture_processing_wait", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)},
        {"ticket", std::to_string(processingTicket)}
    });
    ProcessingTurn processingTurn(processingTicket);
    diagnosticLog().event("audio", "capture_processing_begin", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)},
        {"ticket", std::to_string(processingTicket)}
    });
    auto tPaClose = std::chrono::steady_clock::now();

    if (onState_) onState_(false);

    fprintf(stderr, "Ainput Capture [timer] pa_open=%ldms record=%ldms pa_close=%ldms events=%d samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tPaOpen - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tRecordEnd - tPaOpen).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tPaClose - tRecordEnd).count(),
            nReads, samples_.size());

    if (readFailed) {
        diagnosticLog().event("audio", "capture_read_failed", {
            {"recognition_id", std::to_string(diagnosticId_)},
            {"capture_id", std::to_string(captureId_)},
            {"error_code", std::to_string(error)}
        });
        fprintf(stderr, "Ainput Capture: read error: %s\n", pa_strerror(error));
        if (onStatusText_) onStatusText_("Ainput: microphone read failed");
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.clear();
        unlink(wavPath_.c_str());
        finished_ = true;
        return;
    }

    std::vector<int16_t> batch;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        batch.swap(samples_);
    }

    if (!batch.empty()) {
        // Step 1: ebur128 loudness normalization
        double loudness = normalizeSamples(batch);

        // Step 2: VAD check (but don't trim yet — denoiser needs leading noise)
        bool isBlank = !hasVoice(batch);
        bool wantDenoise = !denoiseMethod_.empty();

        // Step 3: denoise (with full audio for noise profile learning)
        if (!isBlank && wantDenoise) applyDenoise(batch, denoiseMethod_);

        // Step 4: trim silence (now that denoiser has processed the noise)
        trimSilence(batch);

        // ASR providers consume the WAV; blank or unobserved captures do not
        // need a temporary file.
        if (!isBlank && onRecorded_) {
            writeWav(batch, wavPath_);
            diagnosticLog().event("audio", "capture_recorded", {
                {"recognition_id", std::to_string(diagnosticId_)},
                {"capture_id", std::to_string(captureId_)},
                {"sample_count", std::to_string(batch.size())},
                {"wav_hash", hashDiagnosticValue(wavPath_).substr(0, 16)},
                {"voice", "true"}
            });
            onRecorded_(batch, wavPath_);
        } else if (isBlank) {
            diagnosticLog().event("audio", "capture_no_speech", {
                {"recognition_id", std::to_string(diagnosticId_)},
                {"capture_id", std::to_string(captureId_)},
                {"sample_count", std::to_string(batch.size())},
                {"voice", "false"}
            });
            unlink(wavPath_.c_str());
            if (onStatusText_) onStatusText_("Ainput: no speech detected");
        } else {
            unlink(wavPath_.c_str());
        }

        fprintf(stderr, "Ainput Capture [pipeline] loudness=%.1f isBlank=%d denoiser=%s samples=%zu\n",
                loudness, (int)isBlank, denoiseMethod_.c_str(), batch.size());

        {
            std::lock_guard<std::mutex> lk(sampleMutex_);
            samples_ = std::move(batch);
        }
    } else if (!readFailed && onStatusText_) {
        diagnosticLog().event("audio", "capture_no_audio", {
            {"recognition_id", std::to_string(diagnosticId_)},
            {"capture_id", std::to_string(captureId_)}
        });
        onStatusText_("Ainput: no audio captured");
    }
    size_t finalSampleCount = 0;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        finalSampleCount = samples_.size();
    }
    finished_ = true;
    diagnosticLog().event("audio", "capture_processing_end", {
        {"recognition_id", std::to_string(diagnosticId_)},
        {"capture_id", std::to_string(captureId_)},
        {"ticket", std::to_string(processingTicket)},
        {"sample_count", std::to_string(finalSampleCount)},
        {"elapsed_ms", std::to_string(std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count())}
    });
}

void AudioCapture::applyDenoise(std::vector<int16_t> &samples, const std::string &method) {
    if (method.empty() || method == "none") return;

    if (method == "deepfilter") {
        dfDenoise(samples);
        return;
    }
    auto t0 = std::chrono::steady_clock::now();

    constexpr int kFrameSize = 320;
    SpeexPreprocessState *st = speex_preprocess_state_init(kFrameSize, 16000);
    int enable = 1;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_DENOISE, &enable);
    int level = speexLevel_;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &level);

    size_t frameCount = samples.size() / kFrameSize;
    for (size_t i = 0; i < frameCount; i++) {
        speex_preprocess_run(st, samples.data() + i * kFrameSize);
    }
    size_t remainder = samples.size() % kFrameSize;
    if (remainder > 0) {
        std::vector<int16_t> pad(samples.end() - remainder, samples.end());
        pad.resize(kFrameSize, 0);
        speex_preprocess_run(st, pad.data());
        std::copy(pad.begin(), pad.begin() + remainder, samples.end() - remainder);
    }

    speex_preprocess_state_destroy(st);

    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "Ainput Capture [timer] denoise=%ldms samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            samples.size());
}

void AudioCapture::dfDenoise(std::vector<int16_t> &samples) {
    std::lock_guard<std::mutex> lock(deepFilterMutex);
    auto t0 = std::chrono::steady_clock::now();

    // Lazy-start deep-filter daemon with --stay (model loaded once)
    static FILE *daemonPipe = nullptr;
    static bool daemonStartFailed = false;
    if (!daemonPipe && !daemonStartFailed) {
        std::string cmd = std::string(getenv("HOME") ? getenv("HOME") : "/tmp")
                          + "/.local/share/ainput/bin/deep-filter"
                          + " --stay -D -o /tmp 2>/dev/null";
        daemonPipe = popen(cmd.c_str(), "w");
        if (daemonPipe) {
            fprintf(stderr, "Ainput DF: daemon started\n");
            setvbuf(daemonPipe, nullptr, _IONBF, 0);
        } else {
            fprintf(stderr, "Ainput DF: daemon start failed, falling back to one-shot\n");
            daemonStartFailed = true;
        }
    }

    // Resample 16kHz → 48kHz
    size_t inLen = samples.size();
    double ratio = 48000.0 / 16000.0;
    size_t outLen48k = (size_t)(inLen * ratio) + 64;

    std::vector<float> fin(inLen);
    for (size_t i = 0; i < inLen; i++) fin[i] = samples[i] / 32768.0f;

    std::vector<float> fout(outLen48k);
    soxr_error_t err;
    soxr_t resampler = soxr_create(16000, 48000, 1, &err, nullptr, nullptr, nullptr);
    if (err) { fprintf(stderr, "Ainput DF: soxr create error\n"); return; }

    size_t consumed, generated;
    err = soxr_process(resampler, fin.data(), fin.size(), &consumed,
                       fout.data(), fout.size(), &generated);
    soxr_delete(resampler);
    if (err) { fprintf(stderr, "Ainput DF: soxr process error\n"); return; }

    std::vector<int16_t> samples48k(generated);
    for (size_t i = 0; i < generated; i++)
        samples48k[i] = (int16_t)std::clamp((int)(fout[i] * 32768.0f), -32768, 32767);

    // Write 48kHz WAV temp
    std::string tmp48k = "/tmp/ainput_df_" + std::to_string(getpid()) + "_48k.wav";
    writeWav(samples48k, tmp48k);

    if (daemonPipe) {
        // Feed path to daemon's stdin, then poll for file modification
        fprintf(daemonPipe, "%s\n", tmp48k.c_str());
        fflush(daemonPipe);
        auto before = std::filesystem::last_write_time(tmp48k);
        for (int i = 0; i < 100; i++) {
            usleep(2000);
            auto now = std::filesystem::last_write_time(tmp48k);
            if (now != before) break;
        }
    } else {
        // Fallback: one-shot mode
        std::string cmd = std::string(getenv("HOME") ? getenv("HOME") : "/tmp")
                          + "/.local/share/ainput/bin/deep-filter"
                          + " -D -o /tmp " + tmp48k + " 2>/dev/null";
        (void)!std::system(cmd.c_str());
    }

    // Read back 48kHz WAV
    {
        std::ifstream f(tmp48k, std::ios::binary);
        if (f) {
            f.seekg(0, std::ios::end);
            size_t size = f.tellg();
            if (size < 44) {
                fprintf(stderr, "Ainput DF: processed file too small (%zu bytes)\n", size);
                unlink(tmp48k.c_str());
                return;
            }
            f.seekg(44, std::ios::beg);
            size_t dataSize = size - 44;
            samples48k.resize(dataSize / 2);
            f.read(reinterpret_cast<char *>(samples48k.data()), dataSize);
        }
    }

    unlink(tmp48k.c_str());

    // Resample 48kHz → 16kHz
    std::vector<float> f48k(samples48k.size());
    for (size_t i = 0; i < samples48k.size(); i++) f48k[i] = samples48k[i] / 32768.0f;

    size_t outLen16k = (size_t)(f48k.size() / ratio) + 64;
    std::vector<float> f16k(outLen16k);

    resampler = soxr_create(48000, 16000, 1, &err, nullptr, nullptr, nullptr);
    if (!err) {
        err = soxr_process(resampler, f48k.data(), f48k.size(), &consumed,
                           f16k.data(), f16k.size(), &generated);
        soxr_delete(resampler);
    }
    if (err) { fprintf(stderr, "Ainput DF: soxr downsample error\n"); return; }

    samples.resize(generated);
    for (size_t i = 0; i < generated; i++)
        samples[i] = (int16_t)std::clamp((int)(f16k[i] * 32768.0f), -32768, 32767);

    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "Ainput Capture [timer] df_denoise=%ldms samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            samples.size());
}

std::vector<int16_t> AudioCapture::takeSamples() {
    std::lock_guard<std::mutex> lk(sampleMutex_);
    return std::move(samples_);
}

double AudioCapture::normalizeSamples(std::vector<int16_t> &samples) {
    auto t0 = std::chrono::steady_clock::now();

    ebur128_state *ebur = ebur128_init(1, 16000, EBUR128_MODE_I);
    ebur128_add_frames_short(ebur, samples.data(), samples.size());
    double loudness = 0.0;
    ebur128_loudness_global(ebur, &loudness);
    ebur128_destroy(&ebur);

    double gain;
    if (loudness < -70.0 || !std::isfinite(loudness)) {
        fprintf(stderr, "Ainput Capture: audio too quiet (%.1f LUFS), skip norm\n", loudness);
        gain = 1.0;
    } else {
        gain = std::pow(10.0, (lufsTarget_ - loudness) / 20.0);
    }

    auto tEbur = std::chrono::steady_clock::now();

    for (auto &s : samples) {
        double vd = static_cast<double>(s) * gain;
        if (vd > 32767.0) vd = 32767.0;
        else if (vd < -32768.0) vd = -32768.0;
        s = static_cast<int16_t>(static_cast<int32_t>(vd));
    }

    auto tGain = std::chrono::steady_clock::now();
    fprintf(stderr, "Ainput Capture [timer] ebur128=%ldms gain=%ldms loudness=%.1f gain=%.2f\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEbur - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tGain - tEbur).count(),
            loudness, gain);

    return loudness;
}

bool AudioCapture::hasVoice(const std::vector<int16_t> &samples) {
    if (samples.empty()) return false;

    // Crest factor: speech has high peaks relative to RMS, noise is flat
    int32_t peak = 0;
    double sumSq = 0;
    for (auto s : samples) {
        sumSq += (double)s * (double)s;
        int32_t a = s >= 0 ? (int32_t)s : -(int32_t)s;
        if (a > peak) peak = a;
    }

    double rms = std::sqrt(sumSq / samples.size());
    double crestFactor = (double)peak / (rms > 0 ? rms : 1);
    fprintf(stderr, "Ainput Capture: crestFactor=%.2f crestThreshold_=%.2f peak=%d rms=%.2f %s\n",
            crestFactor, crestThreshold_, (int)peak, rms,
            crestFactor < crestThreshold_ ? "SILENCE (crest below threshold)" : "HAS VOICE (crest passes)");
    if (crestFactor < crestThreshold_) return false;

    // Secondary: speexdsp VAD must also detect voice in majority of frames
    constexpr int kFrameSize = 320;
    constexpr double kMinVoiceRatio = 0.15;
    constexpr size_t kMinVoiceFrames = 5;

    SpeexPreprocessState *st = speex_preprocess_state_init(kFrameSize, 16000);
    int enable = 1;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_VAD, &enable);
    int probStart = 90;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_START, &probStart);
    int probContinue = 70;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &probContinue);

    size_t voiceCount = 0;
    size_t frameCount = samples.size() / kFrameSize;
    for (size_t i = 0; i < frameCount; i++) {
        int16_t frame[kFrameSize];
        memcpy(frame, samples.data() + i * kFrameSize, kFrameSize * sizeof(int16_t));
        if (speex_preprocess_run(st, frame))
            voiceCount++;
    }
    size_t remainder = samples.size() % kFrameSize;
    if (remainder > 0) {
        int16_t frame[kFrameSize] = {};
        memcpy(frame, samples.data() + frameCount * kFrameSize, remainder * sizeof(int16_t));
        if (speex_preprocess_run(st, frame))
            voiceCount++;
        frameCount++;
    }
    speex_preprocess_state_destroy(st);

    double ratio = frameCount > 0 ? (double)voiceCount / frameCount : 0;
    fprintf(stderr, "Ainput Capture: VAD crest=%.1f peak=%d voiceCount=%zu/%zu (%.1f%%)\n",
            crestFactor, (int)peak, voiceCount, frameCount, ratio * 100);

    if (voiceCount < kMinVoiceFrames) return false;
    if (ratio < kMinVoiceRatio) return false;

    return true;
}

void AudioCapture::trimSilence(std::vector<int16_t> &samples) {
    constexpr int kFrameSize = 320;
    constexpr size_t kPadFrames = 1;
    constexpr size_t kMinHeadSamples = 3200;

    SpeexPreprocessState *st = speex_preprocess_state_init(kFrameSize, 16000);
    int enable = 1;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_VAD, &enable);
    int probStart = 80;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_START, &probStart);
    int probContinue = 50;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &probContinue);

    size_t frameCount = samples.size() / kFrameSize;
    std::vector<bool> voiceFrames(frameCount, false);

    for (size_t i = 0; i < frameCount; i++) {
        int16_t frame[kFrameSize];
        memcpy(frame, samples.data() + i * kFrameSize, kFrameSize * sizeof(int16_t));
        if (speex_preprocess_run(st, frame))
            voiceFrames[i] = true;
    }

    speex_preprocess_state_destroy(st);

    size_t firstVoice = 0;
    while (firstVoice < frameCount && !voiceFrames[firstVoice]) firstVoice++;
    size_t lastVoice = frameCount;
    while (lastVoice > 0 && !voiceFrames[lastVoice - 1]) lastVoice--;

    if (firstVoice >= lastVoice) return;

    firstVoice = firstVoice > kPadFrames ? firstVoice - kPadFrames : 0;
    lastVoice = std::min(lastVoice + kPadFrames, frameCount);

    size_t trimStart = firstVoice * kFrameSize;
    size_t trimEnd = lastVoice * kFrameSize;

    if (trimStart < kMinHeadSamples) trimStart = kMinHeadSamples;
    if (trimStart > trimEnd) trimStart = trimEnd;

    size_t origSize = samples.size();
    if (trimStart >= origSize) return;

    if (trimStart > 0) {
        samples.erase(samples.begin(), samples.begin() + trimStart);
    }
    size_t shiftedEnd = trimEnd - trimStart;
    if (shiftedEnd < samples.size()) {
        samples.erase(samples.begin() + shiftedEnd, samples.end());
    }

    fprintf(stderr, "Ainput Capture: trimmed %zu leading + %zu trailing samples (orig=%zu now=%zu)\n",
            trimStart, origSize - trimEnd, origSize, samples.size());
}

void AudioCapture::writeWav(const std::vector<int16_t> &samples, const std::string &path) {
    auto t0 = std::chrono::steady_clock::now();

    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "Ainput Capture: cannot write WAV to %s\n", path.c_str());
        return;
    }

    long dataSize = (long)samples.size() * 2;
    fwrite("RIFF", 1, 4, f);
    uint32_t chunkSize = 36 + (uint32_t)dataSize;
    fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t sub1 = 16, sr = 16000, br = 32000;
    uint16_t fmtTag = 1, ch = 1, bps = 16, ba = 2;
    fwrite(&sub1, 4, 1, f); fwrite(&fmtTag, 2, 1, f);
    fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t ds = (uint32_t)dataSize;
    fwrite(&ds, 4, 1, f);
    fwrite(samples.data(), 2, samples.size(), f);
    fclose(f);

    auto tWav = std::chrono::steady_clock::now();
    fprintf(stderr, "Ainput Capture [timer] wav_write=%ldms\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tWav - t0).count());
}

} // namespace ainput
