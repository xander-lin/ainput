#include "doubao_provider.h"
#include "config_schema.h"
#include "config_templates.h"
#include "ainput_config.h"
#include "diagnostic_log.h"

#include <curl/curl.h>
#include <unistd.h>
#include <sys/random.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <thread>
#include <fstream>
#include <memory>

namespace ainput {

// Query poll cadence; retired from config with the other tuning knobs.
constexpr int kDoubaoPollIntervalMsec = 800;

static bool waitCancelable(const std::shared_ptr<std::atomic_bool> &cancel,
                           int milliseconds) {
    for (int waited = 0; waited < milliseconds; waited += 50) {
        if (cancel->load()) return false;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(50, milliseconds - waited)));
    }
    return !cancel->load();
}

static std::string base64Encode(const uint8_t *data, size_t len) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += T[(v >> 18) & 0x3F];
        out += T[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? T[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? T[v & 0x3F] : '=';
    }
    return out;
}

static std::string jsonGetString(const std::string &json, const std::string &key) {
    std::string q = "\"" + key + "\"";
    auto pos = json.find(q);
    if (pos == std::string::npos) return "";
    pos += q.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t'))
        pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static std::string generateUuid() {
    uint8_t r[16];
    if (getrandom(r, sizeof(r), 0) != (ssize_t)sizeof(r)) {
        for (size_t i = 0; i < sizeof(r); i++)
            r[i] = (uint8_t)(rand() ^ (time(nullptr) * (i + 1)));
    }
    r[6] = (r[6] & 0x0F) | 0x40;
    r[8] = (r[8] & 0x3F) | 0x80;
    char buf[37];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7],
             r[8],r[9],r[10],r[11],r[12],r[13],r[14],r[15]);
    return buf;
}

static size_t writeCb(void *ptr, size_t size, size_t nmemb, std::string *out) {
    out->append((const char *)ptr, size * nmemb);
    return size * nmemb;
}

static size_t headerCb(void *ptr, size_t size, size_t nmemb, std::string *out) {
    out->append((const char *)ptr, size * nmemb);
    return size * nmemb;
}

static std::string getHeader(const std::string &headers, const std::string &name) {
    std::string pattern = "\n" + name + ":";
    auto pos = headers.find(pattern);
    if (pos == std::string::npos) {
        if (headers.size() >= name.size() + 1 &&
            headers.substr(0, name.size()) == name && headers[name.size()] == ':') {
            pos = 0;
        } else {
            return "";
        }
    } else {
        pos++;
    }
    pos += name.size() + 1;
    while (pos < headers.size() && headers[pos] == ' ') pos++;
    std::string value;
    while (pos < headers.size() && headers[pos] != '\r' && headers[pos] != '\n') {
        if (headers[pos] != ' ') value += headers[pos];
        pos++;
    }
    return value;
}

static bool jsonBoolValue(const std::string &json, const std::string &key,
                          bool def) {
    std::string q = "\"" + key + "\"";
    auto pos = json.find(q);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos + q.size());
    if (pos == std::string::npos) return def;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) pos++;
    auto end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}' &&
           json[end] != ']') {
        end++;
    }
    std::string val = json.substr(pos, end - pos);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t' ||
                            val.back() == '\n' || val.back() == '\r')) {
        val.pop_back();
    }
    return val == "true";
}

AsrErrorCategory classifyDoubaoHttp(long httpCode) {
    if (httpCode == 429 || httpCode >= 500) return AsrErrorCategory::ServiceUnavailable;
    if (httpCode == 401 || httpCode == 403) return AsrErrorCategory::AuthRejected;
    return AsrErrorCategory::InvalidRequest;
}

// ByteDance auc status codes are 8 digits; 4* are request errors,
// 5* server errors.
AsrErrorCategory classifyDoubaoStatus(const std::string &statusCode) {
    if (statusCode.rfind("401", 0) == 0 || statusCode.rfind("403", 0) == 0)
        return AsrErrorCategory::AuthRejected;
    if (statusCode.rfind("5", 0) == 0 || statusCode.rfind("429", 0) == 0)
        return AsrErrorCategory::ServiceUnavailable;
    return AsrErrorCategory::InvalidRequest;
}

DoubaoAsrProvider::DoubaoAsrProvider() {
    state_ = std::make_shared<WorkerState>();
    worker_ = std::thread([this, state = state_] { workerLoop(state); });
}

DoubaoAsrProvider::~DoubaoAsrProvider() {
    diagnosticLog().event("provider", "provider_shutdown", {
        {"provider", "doubao"},
        {"recognition_id", std::to_string(diagnosticId_)}
    });
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopping = true;
        if (state_->activeCancel) state_->activeCancel->store(true);
        for (const auto &task : state_->tasks) unlink(task.wavPath.c_str());
        state_->tasks.clear();
    }
    state_->ready.notify_one();
    joinAsrWorker(worker_);
}

void DoubaoAsrProvider::setConfig(const std::string &key, const std::string &value) {
    if (key == "api_key") apiKeyOverride_ = value;
    else if (key == "resource_id") resourceIdOverride_ = value;
}

DoubaoSettings DoubaoAsrProvider::resolveSettings() {
    DoubaoSettings s;
    std::string fileKey;
    std::string json = readConfigFile("doubao.json");
    if (!json.empty()) {
        auto issues = validateConfigJson(json, doubaoConfigSchema());
        reportConfigIssues("doubao", "doubao.json", issues);
        for (const auto &issue : issues) {
            if (issue.fatal) {
                s.configError = issue.message;
                break;
            }
        }
        if (s.configError.empty()) {
            fileKey = qjsonStringValue(json, "api_key");
            s.resourceId = jsonGetString(json, "resource_id");
            std::string v = jsonGetString(json, "model_name");
            if (!v.empty()) s.modelName = v;
            s.enableItn = jsonBoolValue(json, "enable_itn", s.enableItn);
            s.enablePunc = jsonBoolValue(json, "enable_punc", s.enablePunc);
            s.timeout = jsonInt(json, "timeout_sec", (int)s.timeout);
        }
    }
    if (fileKey == kApiKeyPlaceholder) fileKey.clear();  // seeded, not yet filled
    if (!resourceIdOverride_.empty()) s.resourceId = resourceIdOverride_;
    s.apiKey = apiKeyOverride_.empty() ? fileKey : apiKeyOverride_;
    return s;
}

void DoubaoAsrProvider::transcribe(std::vector<int16_t> samples, const std::string &wavPath) {
    Task task{std::move(samples), wavPath,
              std::make_shared<std::atomic_bool>(false), onResult_, onError_,
              diagnosticId_};
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stopping) {
            unlink(wavPath.c_str());
            return;
        }
        state_->tasks.push_back(std::move(task));
    }
    state_->ready.notify_one();
}

void DoubaoAsrProvider::workerLoop(const std::shared_ptr<WorkerState> &state) {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->ready.wait(lock, [&] {
                return state->stopping || !state->tasks.empty();
            });
            if (state->stopping) return;
            task = std::move(state->tasks.front());
            state->tasks.pop_front();
            state->activeCancel = task.cancel;
        }
        diagnosticLog().event("provider", "request_worker_begin", {
            {"provider", "doubao"},
            {"recognition_id", std::to_string(task.diagnosticId)},
            {"wav_hash", hashDiagnosticValue(task.wavPath).substr(0, 16)}
        });
        // Config is resolved on the worker thread so per-request file reads
        // never block the UI thread, and config members are never touched
        // from the UI thread (no cross-thread mutation).
        DoubaoSettings settings = resolveSettings();
        processRecording(std::move(task.samples), task.wavPath,
                         settings,
                         std::move(task.cancel), std::move(task.onResult),
                         std::move(task.onError), task.diagnosticId);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->activeCancel.reset();
            if (state->stopping) return;
        }
    }
}

void DoubaoAsrProvider::processRecording(std::vector<int16_t> samples,
                                          const std::string &wavPath,
                                          const DoubaoSettings &settings,
                                          std::shared_ptr<std::atomic_bool> cancel,
                                          AsrResultCallback onR,
                                          AsrErrorCallback onE,
                                          uint64_t diagnosticId) {
    const std::string &apiKey = settings.apiKey;
    const std::string &resourceId = settings.resourceId;
    // Single whole-recognition budget (timeout_sec): submit + polling share
    // one deadline; per-request curl timeouts are capped slices of it.
    const int pollIntervalMsec = kDoubaoPollIntervalMsec;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(settings.timeout);
    auto remainingSec = [&deadline]() {
        auto left = std::chrono::duration_cast<std::chrono::seconds>(
                        deadline - std::chrono::steady_clock::now()).count();
        return left < 0 ? 0L : (long)left;
    };
    fprintf(stderr, "Ainput Doubao: recorded %zu samples to %s (model_name=%s)\n",
            samples.size(), wavPath.c_str(), settings.modelName.c_str());
    diagnosticLog().event("provider", "request_started", {
        {"provider", "doubao"}, {"recognition_id", std::to_string(diagnosticId)},
        {"model", settings.modelName},
        {"wav_hash", hashDiagnosticValue(wavPath).substr(0, 16)},
        {"sample_count", std::to_string(samples.size())}
    });
    struct Cleanup { std::string p; ~Cleanup() { unlink(p.c_str()); } } _wav{wavPath};

    if (!settings.configError.empty()) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "doubao"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "config_invalid"}
        });
        if (onE) onE("Doubao: doubao.json: " + settings.configError,
                     AsrErrorCategory::ConfigInvalid);
        return;
    }

    if (apiKey.empty() || resourceId.empty()) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "doubao"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "missing_credentials"}
        });
        if (onE) onE("Doubao: missing api_key or resource_id in ~/.config/ainput/doubao.json", AsrErrorCategory::ConfigMissing);
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
        std::ifstream wf(wavPath, std::ios::binary);
        if (!wf) {
            diagnosticLog().event("provider", "request_error", {
                {"provider", "doubao"}, {"recognition_id", std::to_string(diagnosticId)},
                {"reason", "wav_read_failed"}
            });
            if (onE) onE("Doubao: failed to read WAV", AsrErrorCategory::AudioData);
            return;
        }
        std::vector<uint8_t> wavData((std::istreambuf_iterator<char>(wf)),
                                      std::istreambuf_iterator<char>());
        if (wavData.empty()) {
            diagnosticLog().event("provider", "request_error", {
                {"provider", "doubao"}, {"recognition_id", std::to_string(diagnosticId)},
                {"reason", "empty_wav"}
            });
            if (onE) onE("Doubao: empty WAV file", AsrErrorCategory::AudioData);
            return;
        }
        std::string b64 = base64Encode(wavData.data(), wavData.size());
        auto tEncode = std::chrono::steady_clock::now();

        std::string taskId = generateUuid();

        {
            CURL *curl = getCurl();
            if (!curl) {
                diagnosticLog().event("provider", "request_error", {
                    {"provider", "doubao"}, {"recognition_id", std::to_string(diagnosticId)},
                    {"reason", "curl_init_failed"}, {"stage", "submit"}
                });
                if (onE) onE("Doubao: curl init failed", AsrErrorCategory::Runtime);
                return;
            }
            std::string submitBody =
                "{"
                "\"audio\":{"
                "\"format\":\"wav\","
                "\"data\":\"" + b64 + "\""
                "},"
                "\"request\":{"
                "\"model_name\":\"" + settings.modelName + "\","
                "\"enable_itn\":" + std::string(settings.enableItn ? "true" : "false") + ","
                "\"enable_punc\":" + std::string(settings.enablePunc ? "true" : "false") +
                "}"
                "}";

            curl_easy_reset(curl);
            std::string respBody, respHdr;
            struct curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers,
                ("X-Api-Key: " + apiKey).c_str());
            headers = curl_slist_append(headers,
                ("X-Api-Resource-Id: " + resourceId).c_str());
            headers = curl_slist_append(headers,
                ("X-Api-Request-Id: " + taskId).c_str());
            headers = curl_slist_append(headers, "X-Api-Sequence: -1");

            curl_easy_setopt(curl, CURLOPT_URL,
                "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, submitBody.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &respHdr);
            const long submitTimeout = std::max(1L, std::min(30L, remainingSec()));
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                             std::min(submitTimeout, 10L));
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, submitTimeout);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            CurlCancellationScope cancellation(curl, cancel);

            CURLcode res = curl_easy_perform(curl);
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_slist_free_all(headers);

            fprintf(stderr, "Ainput Doubao: submit HTTP %ld\n", httpCode);
            if (res != CURLE_OK) {
                if (cancel->load()) {
                    diagnosticLog().event("provider", "request_cancelled", {
                        {"provider", "doubao"},
                        {"recognition_id", std::to_string(diagnosticId)},
                        {"stage", "submit"}
                    });
                    return;
                }
                fprintf(stderr, "Ainput Doubao: submit failed, curl=%d (%s), response_bytes=%zu\n",
                        (int)res, curl_easy_strerror(res), respBody.size());
                evictCurlHandle();
                diagnosticLog().event("provider", "request_error", {
                    {"provider", "doubao"}, {"recognition_id", std::to_string(diagnosticId)},
                    {"reason", "submit_transport"}, {"handle_evicted", "true"}
                });
                if (onE) {
                    onE("Doubao: network request failed (" +
                        std::string(curl_easy_strerror(res)) + ")", AsrErrorCategory::Network);
                }
                return;
            }
            if (httpCode != 200) {
                if (onE) {
                    if (httpCode == 429 || httpCode >= 500) {
                        onE("Doubao: service unavailable (HTTP " +
                            std::to_string(httpCode) + ")",
                            AsrErrorCategory::ServiceUnavailable);
                    } else {
                        onE("Doubao: service request failed (HTTP " +
                            std::to_string(httpCode) + ")",
                            classifyDoubaoHttp(httpCode));
                    }
                }
                return;
            }

            std::string statusCode = getHeader(respHdr, "x-api-status-code");
            if (!statusCode.empty() && statusCode != "20000000") {
                fprintf(stderr, "Ainput Doubao: submit rejected status=%s\n", statusCode.c_str());
                if (onE) onE("Doubao: submit rejected (" + statusCode + ")",
                    classifyDoubaoStatus(statusCode));
                return;
            }
        }

        auto tSubmit = std::chrono::steady_clock::now();

        int consecutiveNetworkErrors = 0;
        CURLcode lastQueryError = CURLE_OK;
        long lastQueryHttpCode = 0;
        for (int pollCount = 1;; pollCount++) {
            if (remainingSec() <= 0) break;  // budget spent -> timeout below
            // Fixed cadence with fast first polls (300 ms, then 500 ms).
            int pollDelay = pollCount == 1   ? 300
                          : pollCount == 2   ? pollIntervalMsec - 300
                                             : pollIntervalMsec;
            if (!waitCancelable(cancel, pollDelay)) {
                diagnosticLog().event("provider", "request_cancelled", {
                    {"provider", "doubao"},
                    {"recognition_id", std::to_string(diagnosticId)},
                    {"stage", "poll_wait"}
                });
                return;
            }

            CURL *curl = getCurl();
            if (!curl) {
                diagnosticLog().event("provider", "request_error", {
                    {"provider", "doubao"}, {"recognition_id", std::to_string(diagnosticId)},
                    {"reason", "curl_init_failed"}, {"stage", "query"}
                });
                if (onE) onE("Doubao: curl init failed", AsrErrorCategory::Runtime);
                return;
            }
            curl_easy_reset(curl);
            std::string respBody, respHdr;

            struct curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers,
                ("X-Api-Key: " + apiKey).c_str());
            headers = curl_slist_append(headers,
                ("X-Api-Resource-Id: " + resourceId).c_str());
            headers = curl_slist_append(headers,
                ("X-Api-Request-Id: " + taskId).c_str());

            curl_easy_setopt(curl, CURLOPT_URL,
                "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &respHdr);
            const long queryTimeout = std::max(1L, std::min(15L, remainingSec()));
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                             std::min(queryTimeout, 10L));
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, queryTimeout);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            CurlCancellationScope cancellation(curl, cancel);

            CURLcode res = curl_easy_perform(curl);
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_slist_free_all(headers);

            if (cancel->load()) {
                diagnosticLog().event("provider", "request_cancelled", {
                    {"provider", "doubao"},
                    {"recognition_id", std::to_string(diagnosticId)},
                    {"stage", "query"}
                });
                return;
            }

            bool retryableHttp = httpCode == 429 || httpCode >= 500;
            if (res != CURLE_OK || retryableHttp) {
                lastQueryError = res;
                lastQueryHttpCode = httpCode;
                consecutiveNetworkErrors++;
                evictCurlHandle();
                fprintf(stderr,
                        "Ainput Doubao: query #%d HTTP %ld curl=%d (%s), retry %d/3\n",
                        pollCount, httpCode, (int)res, curl_easy_strerror(res),
                        consecutiveNetworkErrors);
                if (consecutiveNetworkErrors >= 3) {
                    diagnosticLog().event("provider", "request_error", {
                        {"provider", "doubao"},
                        {"recognition_id", std::to_string(diagnosticId)},
                        {"reason", "query_network"}, {"handle_evicted", "true"}
                    });
                    if (onE) {
                        if (lastQueryError != CURLE_OK) {
                            onE("Doubao: network query failed (" +
                                std::string(curl_easy_strerror(lastQueryError)) +
                                ")", AsrErrorCategory::Network);
                        } else {
                            onE("Doubao: service unavailable (HTTP " +
                                std::to_string(lastQueryHttpCode) + ")",
                                classifyDoubaoHttp(lastQueryHttpCode));
                        }
                    }
                    return;
                }
                continue;
            }
            if (httpCode != 200) {
                diagnosticLog().event("provider", "request_error", {
                    {"provider", "doubao"},
                    {"recognition_id", std::to_string(diagnosticId)},
                    {"reason", "query_http_status"},
                    {"http_code", std::to_string(httpCode)}
                });
                if (onE) onE("Doubao: service query failed (HTTP " +
                             std::to_string(httpCode) + ")",
                             classifyDoubaoHttp(httpCode));
                return;
            }
            consecutiveNetworkErrors = 0;

            std::string statusCode = getHeader(respHdr, "x-api-status-code");

            if (statusCode == "20000000") {
                auto tResult = std::chrono::steady_clock::now();
                std::string text = jsonGetString(respBody, "text");
                fprintf(stderr, "Ainput Doubao [timer] encode=%ldms submit=%ldms poll=%ldms poll_n=%d text_len=%zu\n",
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEncode - t0).count(),
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tSubmit - tEncode).count(),
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tResult - tSubmit).count(),
                        pollCount, text.size());
                diagnosticLog().event("provider", "request_result", {
                    {"provider", "doubao"},
                    {"recognition_id", std::to_string(diagnosticId)},
                    {"text_length", std::to_string(text.size())},
                    {"encode_ms", std::to_string(std::chrono::duration_cast<
                        std::chrono::milliseconds>(tEncode - t0).count())},
                    {"submit_ms", std::to_string(std::chrono::duration_cast<
                        std::chrono::milliseconds>(tSubmit - tEncode).count())},
                    {"poll_ms", std::to_string(std::chrono::duration_cast<
                        std::chrono::milliseconds>(tResult - tSubmit).count())},
                    {"poll_count", std::to_string(pollCount)}
                });
                if (onR) onR(text, true);
                return;
            }
            if (statusCode == "20000003") {
                auto tResult = std::chrono::steady_clock::now();
                fprintf(stderr, "Ainput Doubao [timer] encode=%ldms submit=%ldms poll=%ldms poll_n=%d (silence)\n",
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEncode - t0).count(),
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tSubmit - tEncode).count(),
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tResult - tSubmit).count(),
                        pollCount);
                diagnosticLog().event("provider", "request_error", {
                    {"provider", "doubao"},
                    {"recognition_id", std::to_string(diagnosticId)},
                    {"reason", "no_speech"}
                });
                if (onE) onE("Doubao: no speech recognized", AsrErrorCategory::NoSpeech);
                return;
            }
            if (statusCode != "20000001" && statusCode != "20000002") {
                diagnosticLog().event("provider", "request_error", {
                    {"provider", "doubao"},
                    {"recognition_id", std::to_string(diagnosticId)},
                    {"reason", "recognition_status"}
                });
                fprintf(stderr, "Ainput Doubao: query status=%s response_bytes=%zu\n",
                        statusCode.c_str(), respBody.size());
                if (onE) onE("Doubao: recognition failed (" + statusCode + ")",
                        classifyDoubaoStatus(statusCode));
                return;
            }
            fprintf(stderr, "Ainput Doubao: query #%d processing...\n", pollCount);
        }

        fprintf(stderr, "Ainput Doubao: query timeout\n");
        diagnosticLog().event("provider", "request_timeout", {
            {"provider", "doubao"},
            {"recognition_id", std::to_string(diagnosticId)}
        });
        if (onE) onE("Doubao: recognition timed out", AsrErrorCategory::Timeout);
}

std::unique_ptr<IAsrProvider> DoubaoAsrProviderFactory::create() {
    return std::make_unique<DoubaoAsrProvider>();
}

std::string DoubaoAsrProviderFactory::displayName() const {
    std::string model = "bigmodel";
    std::string warning;
    std::string json = readConfigFile("doubao.json");
    if (!json.empty()) {
        auto issues = validateConfigJson(json, doubaoConfigSchema());
        reportConfigIssues("doubao", "doubao.json", issues);
        warning = firstConfigIssue(issues);
        if (warning.empty()) {
            std::string v = jsonGetString(json, "model_name");
            if (!v.empty()) model = v;
        }
    }
    return warning.empty() ? "Doubao · " + model
                           : "Doubao · " + model + "\n⚠ doubao.json: " + warning;
}

static struct CurlInit {
    CurlInit() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlInit() { curl_global_cleanup(); }
} _curlInit;

static bool _doubaoReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<DoubaoAsrProviderFactory>());
    return true;
}();

} // namespace ainput
