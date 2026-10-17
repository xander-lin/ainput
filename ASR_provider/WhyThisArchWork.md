# Why This Architecture Works

## Module Boundary

`ASR_provider` is responsible for audio capture, audio preprocessing, ASR backend abstraction, provider registration, and provider protocol implementations.

It is not responsible for fcitx5 keyboard events, input context lookup, notifications, or `commitString()`.

## Dependencies

This module may depend on PulseAudio, libebur128, speexdsp, soxr, libcurl, local ASR binaries, and per-user configuration under `~/.config/ainput/`.

It must not depend on fcitx5 adapter types, compositor IPC APIs, or UI state owned by the adapter.

## Data Flow

```text
AudioCapture::start()
  -> PulseAudio samples
  -> normalization / denoise / VAD trim
  -> WAV file
  -> RecordedCallback(samples, wavPath)
  -> IAsrProvider::transcribe(samples, wavPath)
  -> onResult(text, isFinal) or onError(error)
```

## Why It Works

Recording and recognition are separated at a stable data boundary: `std::vector<int16_t>` plus a WAV path. Local providers can use the WAV path for existing command-line tools, while cloud providers can upload or encode the same WAV data. The adapter does not need to understand ASR protocol details.

`AsrProviderRegistry` keeps provider discovery independent from the adapter. The Meson target uses `link_whole` so provider static registration is retained when linked into the addon or tests.

`asr_provider_dep` declares the module's external library requirements, so consumers do not need to duplicate internal ASR dependencies to link correctly.

Hardware buffer detection stores cached burst sizes by PulseAudio default source id in `~/.cache/ainput/pa_buffer.json` (state, not config). Devices with different burst behavior no longer share one `buffer_bytes` value, while the old single-value cache is migrated to the currently selected source on first use.

Runtime configuration reads only `~/.config/ainput/*.json`; defaults are compiled in, so a fresh install works with zero config files and upgrades never need pacnew reconciliation. The earlier `/etc/vinput` fallback layer was removed in the 2026-10 restructure: it added a copy-on-first-read step, pacnew noise, and a test-isolation footgun (packaged placeholder credentials leaking into test runs) for a site-defaults feature a personal IME addon never used.

## Failure Modes

- PulseAudio open/read can fail before samples exist.
- Hardware buffer detection can fall back to a default buffer size.
- PulseAudio source id lookup can fail; in that case detection uses a `default` cache bucket instead of mixing explicitly identified devices.
- Cloud providers can fail on missing API keys, HTTP errors, timeouts, or invalid responses.
- Local providers can fail on missing binaries, missing model files, or non-zero child process exits.
- Denoisers can fail because optional external binaries are missing or audio is too short/silent.

Failures are surfaced through callbacks or diagnostic logs without requiring adapter-owned state inside provider code.

## Replacement Cost

A provider can be replaced by preserving `IAsrProvider::transcribe()`, factory metadata, static registration, and callback semantics. Audio capture can be replaced by preserving `RecordedCallback(samples, wavPath)` and the public `AudioCapture` lifecycle.

## Verification

- `meson test -C build` verifies registry linkage, the file-only audio processing path, and per-device hardware buffer cache behavior.
- `./build/tools/test_audio_file input.wav output.wav none` verifies deterministic file processing with a supplied WAV.
- `./build/tools/test_audio_pipeline output.wav 3 none` verifies PulseAudio capture on a real device.
- Provider-specific cloud or model tests are manual because they require API keys, network, binaries, or model files.

## Independent Review

An independent explore sub-agent reviewed the project structure and identified missing automated tests, missing module architecture documents, stale ASR Provider API docs, and unclear ASR Meson dependency ownership. This document incorporates that review and remains subject to revision when the module boundary changes.
