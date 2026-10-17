# ASR Provider Module

`ASR_provider` owns audio capture, audio preprocessing, ASR provider abstraction, provider registration, and provider implementations. It exposes one Meson dependency, `asr_provider_dep`, for the fcitx5 adapter, tools, and tests.

## Public Contract

- `src/asr_provider.h` defines `IAsrProvider`, `IAsrProviderFactory`, callbacks, and `AsrProviderRegistry`.
- `src/audio_capture.h` defines the recording and preprocessing boundary used by the adapter.
- Consumers should depend on `asr_provider_dep` instead of linking ASR internals one library at a time.
- Providers receive `std::vector<int16_t>` samples and a WAV path through `IAsrProvider::transcribe(samples, wavPath)`.
- Providers return text through `onResult(text, isFinal)` and failures through `onError(error)`.

## Current Source Groups

The source directory is intentionally still flat to avoid a high-risk include and Meson rewrite. Treat the files as these logical groups:

| Group | Files | Responsibility |
|-------|-------|----------------|
| Core | `src/asr_provider.h`, `src/asr_provider.cpp` | Provider interface, factory interface, registry, callback contract |
| Audio | `src/audio_capture.h`, `src/audio_capture.cpp`, `src/buffer_detect.h`, `src/buffer_detect.cpp` | PulseAudio capture, hardware buffer detection, normalization, denoise, VAD trim, WAV writing |
| Providers | `src/mock_provider.*`, `src/doubao_provider.*`, `src/qwen_provider.*`, `src/zipformer_provider.*`, `src/fire_red_provider.*` | Test, cloud, and local ASR implementations |
| Qwen JSON | `src/qwen_json.h` | Dependency-free JSON helpers for Qwen config reading and schema-tolerant response parsing (covered by `tests/test_qwen_json.cpp`) |
| Support | `src/ainput_config.h`, `src/ws_client.h`, `src/service_helper.h` | Config parsing, WebSocket client helpers, systemd/service helpers |
| Build | `src/meson.build` | Static library, `link_whole` registration retention, external dependency ownership |

## Dependency Ownership

`src/meson.build` owns the external libraries used by this module:

- `libpulse-simple` for recording and live audio diagnostics
- `libebur128` for loudness normalization
- `speexdsp` for denoise and voice activity decisions
- `soxr` for resampling support
- `libcurl` for cloud ASR providers

Downstream targets should not duplicate these dependencies unless they directly include and use the external library themselves.

## Adding A Provider

1. Add `src/<id>_provider.h` and `src/<id>_provider.cpp`.
2. Implement `IAsrProvider::transcribe(samples, wavPath)`.
3. Implement a factory with stable `id()`, `name()`, and `create()`.
4. Register the factory through a static initializer in the `.cpp` file.
5. Add the files to `src/meson.build`.
6. Add or update automated tests if the provider has behavior that can run without network, API keys, model files, or microphone hardware.
7. Document manual verification if the provider depends on external services or model files.

## Failure Boundaries

- Audio capture failures must remain in `AudioCapture` and surface through status/error logging or the recorded callback not firing.
- Provider failures must call `onError` where possible and must not touch fcitx5 input contexts.
- Desktop focus and text commit are adapter responsibilities and must not be added to provider code.
- Configuration is read from `~/.config/ainput/*.json` only (`config.json` plus one file per provider). Defaults live in code; files are optional and sparse, and there is no `/etc` fallback layer. Repository files under `config/*.example` are commented examples only.

## Verification

Default verification:

```bash
meson test -C build
```

Manual audio verification:

```bash
./build/tools/test_audio_file input.wav output.wav none
./build/tools/test_audio_pipeline output.wav 3 none
```

Cloud and local model providers require provider-specific manual tests because they depend on credentials, network, binaries, or model files.
