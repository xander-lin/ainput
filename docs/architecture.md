# Architecture

The addon is split along the one boundary that actually costs something: fcitx5
speaks C++ with vtables and `std::string`, everything else is ordinary POSIX
and C libraries.

```text
┌──────────────────────────────────────────────────────────────┐
│ shell/  (C++, ~600 lines)                                     │
│   AddonInstance + AddonFactory          fcitx5 owns these     │
│   AinputConfig                          appears in configtool │
│   CapsLock event state machine          KeyEvent              │
│   OutputHandler                         self-pipe → commitString │
│   notifications                         INotifications        │
└──────────────────────────┬───────────────────────────────────┘
                           │ extern "C" — integers, C strings, POD only
┌──────────────────────────┴───────────────────────────────────┐
│ rust/  (Rust, ~3400 lines)                                    │
│   capture        PulseAudio, per-recording thread             │
│   dsp            ebur128, speexdsp, soxr, WAV                 │
│   asr            qwen, doubao, zipformer, fire_red, mock      │
│   engine         queue + single recognition worker            │
│   config         provider JSON, schema, templates, migration   │
│   uinput         virtual keyboard for CapsLock restore         │
│   diagnostic     bounded JSON-lines log                       │
└──────────────────────────────────────────────────────────────┘
```

`shell/ainput_core.h` is the whole contract. No fcitx5 type crosses it: the
shell passes `const char *`, integers and two POD structs, and receives text or
a categorised error through three function pointers.

## Why this split

A Rust-only addon is possible (the loader only needs one `extern "C"` symbol
returning a `fcitx::AddonFactory *`), but implementing `AddonInstance` from Rust
means reproducing vtable layout and the `std::string` ABI, which breaks
silently when fcitx5 changes. A C++-only addon means hand-rolling JSON parsing,
HTTP and audio DSP.

So: the ~600 lines that must speak C++ stay in C++, and everything with real
logic is Rust, where a failed operation is a `Result` rather than an ignored
return code.

## Data flow

```text
CapsLock press
  -> activation timer (ActivationMsec)
  -> on fire: engine.start() reserves a queue slot, spawns a capture thread,
     the shell records the current input-context UUID, panel shows "listening..."
  -> capture thread: PulseAudio → normalize → speech gate → denoise → trim → WAV

CapsLock release
  -> panel shows "processing audio...", engine.stop() signals the thread,
     CapsLock restored through uinput
  -> capture thread finishes preprocessing and enqueues the WAV
  -> one recognition worker picks it up in FIFO order
  -> provider runs (cloud: HTTP; local: one child process)
  -> on_text / on_error → OutputHandler → commitString in the fcitx loop
```

`Ctrl+CapsLock` enters switch mode without recording. `←/→` or `h/l` change the
backend, `↑/↓` or `k/j` change the denoiser. Both are written to
`~/.config/fcitx5/conf/ainput.conf` and apply to the next recording.

## Concurrency model

| Thread | Count | Work |
|--------|-------|------|
| fcitx5 main | 1 | key events, config, `commitString` |
| capture | one per recording | PulseAudio read loop, DSP, WAV write |
| recognition | one for the process | pulls from the queue, calls the provider |

At most three recordings are in flight, counting the one being recognized. The
counter is reserved **before** anything is created and released exactly once on
every path — success, provider error, capture error, or worker shutdown — so a
full queue can never become permanent.

Two watchdogs cover dropped key events: a recording older than 120 s is stopped,
and a debounce counter that saw no CapsLock traffic for 10 s is cleared.

## Provider contract

`rust/src/asr/mod.rs`:

```rust
pub trait Provider: Send {
    fn transcribe(&self, input: &Input<'_>) -> Result<String>;
}

pub struct Input<'a> {
    pub wav_path: &'a Path,
    pub cancel: &'a Arc<AtomicBool>,
}
```

- Providers receive a finished WAV path; they never open the microphone or
  touch fcitx5 types.
- `cancel` is checked cooperatively. A cancelled request must return an error,
  never silently produce nothing — the queue depends on exactly one outcome per
  recording.
- Errors carry a `Category` (`ConfigMissing`, `AuthRejected`, `ModelNotFound`,
  `LocalSetup`, `Network`, `ServiceUnavailable`, `Timeout`, `NoSpeech`,
  `EmptyResult`, `AudioData`, `Runtime`, `ConfigInvalid`, `InvalidRequest`,
  `Unknown`) which the shell turns into one actionable sentence.

### Backends

| ID | Transport | Needs |
|----|-----------|-------|
| `qwen` | HTTPS, base64 data URL, synchronous | API key |
| `doubao` | HTTPS, inline base64, submit + poll | API key, resource id |
| `zipformer` | child process, streaming transducer | `sherpa-onnx` + model |
| `fire_red` | child process, offline AED | `sherpa-onnx-offline` + model |
| `mock` | none | nothing |

Cloud requests are cancelled through libcurl's progress callback; local engines
by killing the child. Both are bounded by the provider's `timeout_sec`.

### Adding a backend

1. `rust/src/asr/<id>.rs` implementing `Provider`.
2. Add it to `DESCRIPTORS` and `create()` in `rust/src/asr/mod.rs`.
3. If it reads a config file: a schema in `rust/src/config.rs` and a full
   template in `rust/src/templates.rs` (`templates_list_every_field` enforces
   that every field is visible).
4. Document the required external pieces in `docs/development.md`.

## Failure handling

The rewrite exists partly because the C++ version swallowed failures. The rule
is now: a failure either propagates as an `Error` with a category, or it is
reported to the user. There is no third option.

| Failure | Behaviour |
|---------|-----------|
| Missing API key / model file | panel shows the exact file to edit |
| Denoise helper missing | recording is **discarded** with a setup error, not sent undenoised |
| Microphone unavailable | `Ainput: microphone ...` immediately, not an empty result |
| Input context gone before commit | result goes to the currently focused context, logged |
| `/dev/uinput` unavailable | one warning naming the udev rule; voice input still works |
| Queue full | `recognition queue full; try again`, newest recording discarded |
| Provider cancelled | returns `Timeout("cancelled")` so the queue advances |

## Verification

- `meson test -C build` runs the Rust unit tests (no hardware needed).
- `cargo test --lib -- --ignored` runs the end-to-end test that records from the
  real microphone and checks the callback fires.
- `tools/` is gone: diagnostics are a `--features diagnostics` build writing
  `~/.local/share/ainput/diagnostic.log`.
