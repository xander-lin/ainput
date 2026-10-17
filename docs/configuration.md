# Configuration Reference

Ainput has two configuration surfaces, split by who owns the data.

| Surface | File | Edited with |
|---------|------|-------------|
| Shared settings (backend, key timing, audio pipeline) | `~/.config/fcitx5/conf/ainput.conf` | **fcitx5-configtool** (Addons → Ainput) or the file directly |
| Per-provider data (credentials, model ids, paths) | `~/.config/ainput/<provider>.json` | A text editor |

The split is deliberate: settings that affect the whole addon belong to fcitx5,
so they appear in its configuration UI and are written back the same way every
other addon writes its settings. Credentials and model paths stay in plain
files, where they can carry comments and be backed up or removed independently.

## Shared settings

Owned by fcitx5's configuration system. Edit them in
`fcitx5-configtool` → *Addons* → *Ainput*, or edit
`~/.config/fcitx5/conf/ainput.conf` while fcitx5 is **not** running (fcitx5
rewrites the file on exit).

| Option | Default | Meaning |
|--------|---------|---------|
| `Provider` | `qwen` | Backend: `qwen`, `doubao`, `zipformer`, `fire_red`, `mock`. `Ctrl+CapsLock` switching rewrites this |
| `ActivationMsec` | `300` | How long CapsLock must be held before recording starts (50–5000) |
| `NotificationTimeout` | `2000` | How long the `Ctrl+CapsLock` switch notification stays on screen (200–20000 ms) |
| `Denoise` | `none` | `none`, `speexdsp`, or `deepfilter` |
| `LufsTarget` | `-16.0` | Loudness normalization target in LUFS; values outside (-100, 0) fall back to the default |
| `SpeexLevel` | `-15` | speexdsp suppression strength in dB (-100–0) |
| `CrestThreshold` | `2.4` | Speech gate. Lower is more permissive; `0` disables the gate |

fcitx5 only writes options that differ from their default, so the file stays
short. Missing options mean "default".

### Credentials and model ids

Four settings exist so a fresh install can be configured entirely in
fcitx5-configtool:

| Option | Meaning |
|--------|---------|
| `QwenApiKey` | DashScope API key |
| `QwenModel` | Qwen model id |
| `DoubaoApiKey` | Volcengine app key |
| `DoubaoModel` | Doubao `request.model_name` |

**Empty means "not set here"** — the provider then falls back to
`~/.config/ainput/<provider>.json`, and finally to its built-in default. An
empty field never clears a key you already have in the file, so an existing
setup keeps working and the GUI is simply the more convenient place to put a
new one.

Precedence, highest first: fcitx5-configtool → provider JSON → built-in
default. The `Ctrl+CapsLock` notification shows the model that will actually
run, including a GUI override.

### Why there is no buffer setting

The microphone fragment size is fixed in code at 20 ms (640 bytes). It is not
exposed because there is nothing to tune and nothing to detect:

- PulseAudio delivers **exactly** the requested number of bytes per wakeup. A
  measurement on a real device requesting 5/10/20/50/100/200/400 ms returned
  chunk sizes identical to the request every time. The "device burst period"
  that an earlier auto-detection tried to find is not observable through the
  stream API, because the request *is* the delivery size.
- Audio still buffered but not yet forming a complete fragment is discarded
  when the stream closes, so the tail loss is bounded by the fragment size.
  Smaller is therefore strictly better, at the cost of more wakeups — and the
  capture loop already polls every 2 ms, so the wakeup count is irrelevant.
- The capture path drains for 150 ms after you release the key, which recovers
  the final partial fragment as long as the drain outlasts the fragment.

The old "64K works, 4K loses the tail" problem came from `pa_simple_read`,
which blocks until it has the full requested byte count. The current
implementation peeks and discards whatever is available, so that failure mode
no longer exists.

### Denoiser

`Denoise` is `none` out of the box. `speexdsp` needs no extra software.
`deepfilter` requires an executable at
`~/.local/share/ainput/bin/deep-filter`; when it is missing the recording is
**not** silently processed undenoised — the status panel reports
`Ainput: local engine setup error: ...` and the recording is discarded.

## Provider files

Stored in `~/.config/ainput/`. Every file is created automatically on first run
with **all** fields present, their default values, and English comments, so
opening one shows everything that can be set. `//` line comments are supported
and are stripped before parsing.

Files are re-read before every recognition, so edits apply to the next
recording without restarting fcitx5. Local provider files
(`zipformer.json`, `fire_red.json`) are optional: they are only needed to
override a path or a timeout.

Keep credentials private:

```bash
chmod 600 ~/.config/ainput/qwen.json ~/.config/ainput/doubao.json
```

API keys are plaintext fields by design; there is no secret-store integration.

### `qwen.json`

| Key | Default | Meaning |
|-----|---------|---------|
| `api_key` | — (required) | DashScope key. The placeholder `PASTE_YOUR_KEY_HERE` counts as unset |
| `model` | `qwen-audio-3.1-asr-flash` | Model id. The request format is chosen from the model name; `qwen3-asr-flash` still works |
| `endpoint` | DashScope Beijing URL | Full request URL |
| `language_hints` | `[]` | Language codes, at most 4. Empty = auto-detect |
| `vocabulary` | `{}` | Instant hot words `{"term": weight}`, weight 1–5 (50 = super hot) |
| `vocabulary_id` | `""` | Precompiled hot-word list id |
| `keep_dialect` | `false` | Keep dialect wording (`qwen-audio-3.1-asr-flash` only) |
| `speaker_diarization` | `false` | Label speakers (`qwen-audio-3.1-asr-flash` only) |
| `timeout_sec` | `60` | Whole-request timeout |

### `doubao.json`

| Key | Default | Meaning |
|-----|---------|---------|
| `api_key` | — (required) | Volcengine app key |
| `resource_id` | `volc.seedasr.auc` | Product tier |
| `model_name` | `bigmodel` | Sent as `request.model_name` |
| `enable_itn` | `true` | Inverse text normalization |
| `enable_punc` | `true` | Punctuation restoration |
| `timeout_sec` | `90` | Whole-recognition budget: submit and polling share it |

### `zipformer.json` / `fire_red.json`

| Key | Default | Meaning |
|-----|---------|---------|
| `model_dir` | `~/.local/share/ainput/models/...` | Model directory |
| `bin_path` | `~/.local/share/ainput/sherpa-onnx/bin/...` | Engine binary |
| `timeout_sec` | `120` | Wall-clock timeout for one recognition |

## Validation

Every file is validated on read:

- JSON syntax errors are reported with a byte offset, and the file's values are
  not trusted (defaults apply).
- Unknown fields get a "did you mean" suggestion; type mismatches and missing
  required fields are named.
- Problems go to the fcitx5 log. The first problem in a provider file is also
  appended to that provider's `Ctrl+CapsLock` notification as a `⚠` line.

## Migration from 0.1

0.1 kept everything in `~/.config/ainput/config.json`. On first start, 0.2
reads that file, copies its values into fcitx5's configuration, and renames it
to `config.json.migrated` — nothing is deleted, and the values (including a
hand-tuned `crest_threshold: 0`) are carried over rather than reset.

After the rename the old file is no longer read. A startup message in the
fcitx5 log names the new location.

## Adding a setting

A new shared option goes in three places:

1. `AinputConfig` in `shell/ainput.cpp`
2. the `AinputSettings` struct in `shell/ainput_core.h`
3. `Settings` in `rust/src/settings.rs`

A new per-provider key goes in `rust/src/config.rs` (schema) and
`rust/src/templates.rs` (visible default), and `tests/templates_list_every_field`
fails until both are updated.
