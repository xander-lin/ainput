#pragma once

// C ABI between the C++ fcitx5 shell and the Rust core.
//
// Only plain data crosses this boundary: integers, C strings and POD structs.
// No fcitx5 or C++ type is ever visible to the Rust side.
//
// String ownership:
//   - Strings the Rust side passes to a callback are borrowed and only valid
//     for the duration of that callback. The shell must copy them.
//   - Strings returned through a caller-provided buffer are copied in and
//     truncated at a char boundary; the return value is the untruncated
//     length in bytes, or -1 on error.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AinputEngine AinputEngine;

typedef struct {
    void *user;
    // Recognition finished; text is final. Fires on a Rust thread.
    void (*on_text)(void *user, uint64_t recognition_id, const char *text);
    // Failure. category is one of AINPUT_ERR_*; detail is a human description.
    // Status text ("listening", "processing") stays in the shell; the core
    // only reports outcomes.
    void (*on_error)(void *user, uint64_t recognition_id, int32_t category,
                     const char *detail);
} AinputCallbacks;

typedef struct {
    const char *provider;         // e.g. "qwen"; NULL means default
    int32_t activation_msec;
    int32_t notification_timeout;
    int32_t buffer_ms;            // PulseAudio fragment size, in milliseconds
    int32_t denoise;              // AINPUT_DENOISE_*
    double lufs_target;
    int32_t speex_level;
    double crest_threshold;

    // Credentials and model ids set in fcitx5-configtool. NULL or empty means
    // "not set here": the provider then uses its JSON file.
    const char *qwen_api_key;
    const char *qwen_model;
    const char *doubao_api_key;
    const char *doubao_model;
} AinputSettings;

enum {
    AINPUT_DENOISE_NONE = 0,
    AINPUT_DENOISE_SPEEXDSP = 1,
    AINPUT_DENOISE_DEEPFILTER = 2,
};

enum {
    AINPUT_ERR_CONFIG_MISSING = 0,
    AINPUT_ERR_CONFIG_INVALID = 1,
    AINPUT_ERR_AUTH_REJECTED = 2,
    AINPUT_ERR_MODEL_NOT_FOUND = 3,
    AINPUT_ERR_LOCAL_SETUP = 4,
    AINPUT_ERR_INVALID_REQUEST = 5,
    AINPUT_ERR_NETWORK = 6,
    AINPUT_ERR_SERVICE_UNAVAILABLE = 7,
    AINPUT_ERR_TIMEOUT = 8,
    AINPUT_ERR_NO_SPEECH = 9,
    AINPUT_ERR_EMPTY_RESULT = 10,
    AINPUT_ERR_AUDIO_DATA = 11,
    AINPUT_ERR_RUNTIME = 12,
    AINPUT_ERR_UNKNOWN = 13,
};

enum {
    AINPUT_TEMPLATE_QWEN = 1,
    AINPUT_TEMPLATE_DOUBAO = 2,
    AINPUT_TEMPLATE_ZIPFORMER = 3,
    AINPUT_TEMPLATE_FIRE_RED = 4,
};

// Engine lifecycle. Callbacks may fire from any Rust thread.
AinputEngine *ainput_engine_new(const AinputCallbacks *callbacks);
void ainput_engine_free(AinputEngine *engine);
void ainput_engine_set_settings(AinputEngine *engine,
                                const AinputSettings *settings);

// Start recording; returns a recognition id, or 0 when the engine is not
// ready. stop() finalises that recording and queues recognition.
uint64_t ainput_engine_start(AinputEngine *engine);
void ainput_engine_stop(AinputEngine *engine, uint64_t recognition_id);
// Stops capture, drops the queue and joins workers. Engine must not be used
// afterwards except for ainput_engine_free().
void ainput_engine_shutdown(AinputEngine *engine);

size_t ainput_provider_count(void);
int32_t ainput_provider_id(size_t index, char *buf, size_t len);
int32_t ainput_provider_display(size_t index, char *buf, size_t len);

// Returns 1 on success, 0 when /dev/uinput could not be opened (the reason is
// available from ainput_uinput_error()).
int32_t ainput_uinput_open(void);
void ainput_uinput_restore_capslock(void);
void ainput_uinput_close(void);
int32_t ainput_uinput_error(char *buf, size_t len);

// Commented template for a provider config file (AINPUT_TEMPLATE_*).
int32_t ainput_config_template(int32_t which, char *buf, size_t len);

// Reads the pre-0.2 JSON layout and fills `out`. Returns 1 when a legacy file
// was found, 0 when there is nothing to migrate, -1 on read/parse failure.
// `report` receives a human-readable note describing what was migrated.
int32_t ainput_migrate_legacy_config(AinputSettings *out, char *report,
                                     size_t report_len);

void ainput_string_free(char *s);

#ifdef __cplusplus
}
#endif
