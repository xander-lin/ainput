# Architecture

Module boundaries, data flow, the provider contract, and how to add a backend.

## Module map

```text
fcitx5
  |
  v
adapter/AinputAddon
  |-- CapsLock / Ctrl+CapsLock key lifecycle
  |-- provider and denoiser selection, notifications
  |-- AudioCapture lifecycle and recognition queue
  |-- IAsrProvider lifecycle
  `-- OutputHandler lifecycle

ASR_provider/AudioCapture
  |-- PulseAudio capture
  |-- hardware buffer detection (cached per source)
  |-- ebur128 normalization
  |-- speech gate (crest factor + speexdsp VAD)
  |-- denoise (only when configured)
  |-- silence trim
  `-- samples + wavPath

ASR_provider/IAsrProvider
  |-- transcribe(samples, wavPath)
  `-- onResult(text, isFinal) / onError(error, category)

adapter/OutputHandler
  |-- self-pipe thread handoff
  `-- fcitx InputContext::commitString(text)
```

| Directory | Owns | Must not own |
|-----------|------|--------------|
| `adapter/` | fcitx5 integration, key handling, activation, notifications, CapsLock restore, text commit | ASR protocols, denoise algorithms, model command lines |
| `ASR_provider/` | audio capture + preprocessing, provider abstraction, provider registry, vendor protocols | fcitx5 input contexts, focus state, `commitString` |
| `tools/` | manual hardware diagnostics | anything the addon links |
| `tests/` | automated contracts that need no hardware | microphone / cloud / model dependencies |

`OutputHandler` is the only bridge from worker threads back into the fcitx
event loop: it writes to a self-pipe, the main loop drains it, and the commit
happens against the input-context UUID captured when recording started.

## Data flow

```text
CapsLock press
  -> activation timer starts (activation_msec, default 300)
  -> on activation: AudioCapture created, OutputHandler captures the current
     input-context UUID, panel shows "listening..."
  -> AudioCapture records and preprocesses

CapsLock release
  -> panel shows "processing audio..."
  -> AudioCapture::stop() signals the capture loop and returns immediately
  -> CapsLock state restored through uinput
  -> capture thread finishes preprocessing, writes the WAV
  -> recorded callback shows "recognizing..." and enqueues a request
  -> one recognition runs at a time (max 3 including the active one)
  -> provider created or reused; transcribe() runs on its worker
  -> onResult/onError
  -> OutputHandler::submit() -> commitString() in the fcitx loop
```

`Ctrl+CapsLock` enters switch mode without recording. `←/→` or `h/l` change
the ASR provider, `↑/↓` or `k/j` change the denoiser. Both selections are
written back to `config.json` and apply to the next recording.

## Provider contract

Defined in `ASR_provider/src/asr_provider.h`; that file is authoritative over
any snippet here.

```cpp
using AsrResultCallback = std::function<void(const std::string &text, bool isFinal)>;
using AsrErrorCallback =
    std::function<void(const std::string &error, AsrErrorCategory category)>;

class IAsrProvider {
public:
    virtual ~IAsrProvider() = default;
    virtual void transcribe(std::vector<int16_t> samples,
                            const std::string &wavPath) = 0;
    virtual void setConfig(const std::string &key, const std::string &value);
    virtual void setDiagnosticId(uint64_t id);
    void setResultCallback(AsrResultCallback cb);
    void setErrorCallback(AsrErrorCallback cb);
};

class IAsrProviderFactory {
public:
    virtual ~IAsrProviderFactory() = default;
    virtual std::string id() const = 0;
    virtual std::string name() const = 0;
    virtual std::string displayName() const { return name(); }
    virtual std::unique_ptr<IAsrProvider> create() = 0;
};
```

- Providers receive already captured audio. They never open the microphone,
  read keyboard events, or touch fcitx5 types.
- Callbacks fire on the provider's worker thread. UI work must be handed to
  the host event loop (the adapter does this through `OutputHandler`).
- `displayName()` is what the `Ctrl+CapsLock` notification shows. Factories
  whose model comes from a config file override it to include the current
  model and the first schema warning.
- `AsrErrorCategory` (ConfigMissing, ConfigInvalid, AuthRejected,
  ModelNotFound, LocalSetup, InvalidRequest, Network, ServiceUnavailable,
  Timeout, NoSpeech, EmptyResult, AudioData, Runtime, Unknown) lets the
  adapter print an actionable status. The raw detail is shown only for the
  unclassified bucket (ConfigInvalid, InvalidRequest, Runtime, Unknown).

### Registry

Factories self-register in a static initializer. `src/meson.build` builds the
module as a static library and exposes `asr_provider_dep` with `link_whole`,
so registration objects survive linking into the addon, tools, and tests.

```cpp
auto &reg = ainput::AsrProviderRegistry::instance();
auto list = reg.listFactories();          // (id, displayName) pairs
auto asr = reg.create("mock");
```

### Implementations

| ID | Name | Input | External dependency |
|----|------|-------|---------------------|
| `mock` | Mock (test) | ignored | none; returns `hello world` |
| `qwen` | Qwen ASR (Alibaba DashScope) | WAV path, base64 data URL | API key in `qwen.json` |
| `doubao` | Doubao AUC (ByteDance) | WAV path, base64 inline | API key + resource ID in `doubao.json` |
| `zipformer` | Local Zipformer (streaming) | WAV path | `sherpa-onnx` binary + model files |
| `fire_red` | Local FireRed (offline AED) | WAV path | `sherpa-onnx-offline` binary + model files |

Cloud providers own one serialized worker thread each; local providers spawn
one process per utterance and join it on cancellation.

### Adding a provider

1. Add `src/<id>_provider.{h,cpp}` under `ASR_provider/src/`.
2. Implement `transcribe()` and a factory with stable `id()`, `name()`,
   `create()`; override `displayName()` if the model is config-driven.
3. Register the factory in the `.cpp` with a static initializer.
4. Add the files to `src/meson.build`.
5. Add a test under `tests/` if the behavior can run without network, keys,
   models, or a microphone. Otherwise document manual verification.
6. Add the `id` to the `provider` enum in `config/config.json.example`,
   `docs/development.md`, and the schema if it takes a config file.

## Failure boundaries

| Area | Failure | Handling |
|------|---------|----------|
| AudioCapture | PulseAudio open/read failure | status message, no provider call if no samples |
| AudioCapture | no speech after gating | `Ainput: no speech detected`, WAV removed |
| AudioCapture | `deep-filter` missing | denoise falls back to the undenoised audio (stderr note only) |
| Provider | missing key / model / binary | `onError` with a category -> actionable panel text |
| OutputHandler | captured context is gone | logged and dropped; no fallback to the newest context |
| uinput | `/dev/uinput` not writable | CapsLock restore disabled; voice input still works |
| uinput | compositor ignores synthetic events | CapsLock restore ineffective on some compositors |

Failures are surfaced through callbacks, status text, or the diagnostic log.
Provider code never reaches into adapter state.

## Why this shape

Recording and recognition meet at one stable boundary: `std::vector<int16_t>`
plus a WAV path. Local providers can hand the path to a CLI, cloud providers
can encode the same bytes, and the adapter never learns the protocol. The
registry keeps discovery out of the adapter, and `link_whole` makes static
registration deterministic. Replacing a provider means preserving
`transcribe()` + factory metadata; replacing capture means preserving
`RecordedCallback(samples, wavPath)`.

## Verification

Automated coverage lives in `tests/` (see `docs/development.md`). Manual
checks that need real hardware: `tools/test_audio_file`,
`tools/test_audio_pipeline`, `tools/test_uinput_caps`.
