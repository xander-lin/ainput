# Why This Architecture Works

## Module Boundary

`adapter` is responsible for fcitx5 addon integration, CapsLock event handling, activation/deactivation lifecycle, provider and denoiser switching, notifications, uinput CapsLock restore, and text commit.

It is not responsible for ASR API protocols, audio denoise algorithms, hardware buffer detection internals, or local model command-line details.

## Dependencies

This module may depend on fcitx5, fcitx5 notifications, Linux uinput, and `ASR_provider` public interfaces.

It must not require providers to know about fcitx5 input contexts or focus state.

## Data Flow

```text
fcitx5 KeyEvent
  -> AinputAddon activation lifecycle
  -> AudioCapture start/stop
  -> IAsrProvider::transcribe(samples, wavPath)
  -> provider callback
  -> OutputHandler::submit(text)
  -> InputContext::commitString(text)
```

## Why It Works

The addon keeps user interaction state in one place while delegating replaceable concerns to smaller modules. `AudioCapture` owns recording. `IAsrProvider` owns recognition. `OutputHandler` owns thread-safe result delivery and commit.

This prevents ASR providers from depending on fcitx5 and keeps result delivery isolated to the fcitx event loop.

## Failure Modes

- `/dev/uinput` can be unavailable, disabling CapsLock restore while leaving recognition flow intact.
- No input context can exist at activation time, so activation exits early.
- No ASR provider can be registered, so activation exits without recording.
- Provider creation can fail, so capture is not started.
- The captured input context can disappear before commit, so `OutputHandler` logs the missing context and drops the result.

## Replacement Cost

The adapter can replace an ASR provider without changing key handling if the provider preserves `IAsrProvider`. Output commit behavior can be changed inside `OutputHandler` without modifying provider implementations.

## Verification

- `meson test -C build` verifies provider registry linkage used by the adapter.
- `ninja -C build` verifies the fcitx5 addon links with ASR modules.
- Manual integration requires `sudo meson install -C build`, `fcitx5 -r`, and CapsLock testing in a real session.

## Independent Review

An independent explore sub-agent reviewed the module boundaries and recommended explicit `WhyThisArchWork.md` files for `adapter` and `ASR_provider`. This document incorporates that review and should be updated when adapter-owned inputs, outputs, dependencies, or failure modes change.
