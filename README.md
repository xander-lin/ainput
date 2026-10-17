# fcitx5-ainput

**English** | [中文](README.zh-CN.md)

Push-to-talk voice input for [fcitx5](https://github.com/fcitx/fcitx5): hold
**CapsLock**, speak, release — the recognized text is inserted at the cursor.
Ainput (**a**udio **input**) is a small fcitx5 addon for Linux desktops. The
fcitx5-facing layer is C++; capture, audio processing and the ASR backends are
Rust.

## Features

- **CapsLock push-to-talk** — no extra hotkey daemon; the addon is loaded by
  fcitx5 itself.
- **Pluggable ASR backends** — two cloud APIs (Alibaba Qwen, ByteDance Doubao)
  and two local engines (sherpa-onnx Zipformer / FireRed) behind one interface.
  Switch with `Ctrl+CapsLock` at any time; the choice is persisted.
- **Consecutive speech** — start the next sentence while the previous one is
  still recognizing (up to 3 in flight, order preserved, each result goes to
  the window that was focused when that recording started).
- **Audio preprocessing** — loudness normalization, speech gate, optional
  denoise, silence trimming.
- **Actionable failures** — missing key, rejected key, model not found, local
  setup, network, timeout, no speech: the status panel tells you which, and
  never inserts the error into your document.
- **CapsLock restore** — a synthetic CapsLock event through `/dev/uinput` puts
  the lock state back, so dictation does not leave CapsLock on.

## Requirements

- Linux with fcitx5 (`fcitx5`, `libpulse`, `libebur128`, `libcurl`,
  `speexdsp`, `libsoxr`)
- To build from source: a C++20 compiler, Meson, Ninja and a Rust toolchain
- For local engines: the `sherpa-onnx` binaries and model files (optional,
  roughly 1.3 GB total)

## Install

Arch Linux (AUR):

```bash
paru -S fcitx5-ainput-git
```

From source:

```bash
meson setup build --prefix=/usr
ninja -C build
sudo meson install -C build
```

Either way, restart fcitx5 afterwards:

```bash
fcitx5 -r
```

The package also installs a udev rule for `/dev/uinput`, which is what allows
CapsLock restore. See [Troubleshooting](#troubleshooting).

## Usage

| Action | Keys |
|--------|------|
| Voice input | Hold **CapsLock** → speak → release |
| Switch ASR provider | **Ctrl+CapsLock**, then **← / →** (or **h / l**) |
| Switch denoiser | **Ctrl+CapsLock**, then **↑ / ↓** (or **k / j**) |

While recording, the input panel shows `listening...`; after release it moves
through `processing audio...` and `recognizing...`, then the line disappears and
either the text is inserted or an actionable error is shown.

## ASR backends

| Provider | Where it runs | Needs |
|----------|---------------|-------|
| Qwen | Alibaba DashScope (cloud) | API key |
| Doubao | ByteDance Volcengine (cloud) | API key + resource ID |
| Zipformer | local, streaming | `sherpa-onnx` + model (~490 MB download) |
| FireRed | local, offline AED | `sherpa-onnx-offline` + model (~800 MB download) |
| Mock | local, test only | nothing |

Qwen is the default. Cloud providers are re-read from config before every
recognition, so a model change applies to the next sentence without a restart.

## Configure

**Shared settings** (backend, hold time, denoiser, audio tuning) live in
fcitx5's own configuration and appear in **fcitx5-configtool** under
*Addons → Ainput*. Nothing to edit by hand unless you prefer to.

**Per-provider data** (credentials, model id, paths) lives in
`~/.config/ainput/*.json`. Every file is created on first run with all fields
present, their defaults, and comments, so opening one shows everything that can
be set. Minimum for a cloud provider:

```json
// ~/.config/ainput/qwen.json
{ "api_key": "sk-..." }
```

```json
// ~/.config/ainput/doubao.json
{ "api_key": "...", "resource_id": "volc.seedasr.auc" }
```

Protect the credential files: `chmod 600 ~/.config/ainput/{qwen,doubao}.json`.
Keys are plaintext; there is no secret-store integration by design.

Full reference, defaults, validation rules and the migration from the old `config.json`:
[`docs/configuration.md`](docs/configuration.md).

## Local models (optional)

Only needed if you want to dictate without sending audio to a cloud service.

```bash
mkdir -p ~/.local/share/ainput/{sherpa-onnx/bin,models}

# Binaries: the -static archive contains both `sherpa-onnx` and
# `sherpa-onnx-offline`. Check the latest tag at
# https://github.com/k2-fsa/sherpa-onnx/releases
VERSION=v1.13.2
curl -LO "https://github.com/k2-fsa/sherpa-onnx/releases/download/v${VERSION}/sherpa-onnx-v${VERSION}-linux-x64-static.tar.bz2"
tar xf "sherpa-onnx-v${VERSION}-linux-x64-static.tar.bz2"
cp -r "sherpa-onnx-v${VERSION}-linux-x64-static/bin/"* ~/.local/share/ainput/sherpa-onnx/bin/

# Models
MO="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"
curl -LO "$MO/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2"
tar xf sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2 \
    -C ~/.local/share/ainput/models/
mv ~/.local/share/ainput/models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20 \
   ~/.local/share/ainput/models/zipformer-zh-en

curl -LO "$MO/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2"
tar xf sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2 \
    -C ~/.local/share/ainput/models/
```

The provider expects the file names produced by these two archives. Other
archives with different names need a matching `model_dir` prepared by hand;
custom paths go in `zipformer.json` / `fire_red.json`.

## Troubleshooting

**CapsLock stays on after dictation.** `Ainput` needs write access to
`/dev/uinput`. Check:

```bash
getfacl /dev/uinput        # expect a user:<you>:rw- line
```

If the line is missing, the log says `cannot open /dev/uinput (errno=13)`.
Reload the shipped rule:

```bash
sudo modprobe uinput
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=misc --sysname-match=uinput
fcitx5 -r
```

On some compositors (Hyprland was observed; niri works) the synthetic toggle
is ignored even with correct permissions — voice input still works, only the
lock state is not restored.

**Nothing appears in the panel.** Check `fcitx5` logs for `Ainput`; make sure
the addon is enabled and the provider has a valid key. Each failure path has
its own message (`fill api_key in ...`, `API key rejected`, `network error`,
`recognition timed out`, ...).

**Recognition queue full.** At most three recordings are accepted at once.
Wait for `recognizing...` to finish.

**Settings went wrong.** Delete `~/.config/fcitx5/conf/ainput.conf` while
fcitx5 is stopped and start it again: the file is regenerated with every option
and its default.

**Settings don't appear in fcitx5-configtool.** The addon must have loaded at
least once (restart fcitx5), and `fcitx5-configtool` must be from the same
fcitx5 version. Settings are stored in `~/.config/fcitx5/conf/ainput.conf`.

## Documentation

- [`docs/configuration.md`](docs/configuration.md) — every config key and default
- [`docs/architecture.md`](docs/architecture.md) — modules, provider contract,
  how to add a backend
- [`docs/development.md`](docs/development.md) — build, tests, diagnostics,
  repository conventions
- [`docs/history.md`](docs/history.md) — dated failure analysis and the
  engineering log (archive; not a specification)
- [`docs/qwen-asr-api.md`](docs/qwen-asr-api.md),
  [`docs/doubao-auc-api.md`](docs/doubao-auc-api.md) — vendor API notes
- `man ainput` — the installed man page

## License

MIT
