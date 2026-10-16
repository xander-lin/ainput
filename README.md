# fcitx5-vinput

Voice input addon for [fcitx5](https://github.com/fcitx/fcitx5). Push-to-talk speech recognition via CapsLock.

## Usage

| Action | Keys |
|--------|------|
| Voice input | Hold **CapsLock** → speak → release |
| Switch ASR provider | **Ctrl+CapsLock**, then **←/→** |
| Switch denoiser | **Ctrl+CapsLock**, then **↑/↓** |

### Recognition status and consecutive speech

The input panel beside the cursor reports the current phase:

```
listening → processing audio → recognizing → result or an actionable error
```

You may begin the next recording while an earlier cloud request is still
recognizing. Vinput preserves capture and commit order, and binds each result
to the application and input context active when that recording began.

To prevent unbounded memory and service requests, at most three recognitions
(including the active request) are accepted at once. When the queue is full,
Vinput shows `recognition queue full; try again` and discards only the newest
recording.

Cloud failures are shown in the input panel as actionable errors, one per
failure path: missing API key, rejected API key, model not found, local
engine setup (missing binary/model files), network, service unavailable,
timeout, no speech, audio capture, and uncategorized failures (which show
the provider's raw detail). These messages are status UI, not text inserted
into the application.

## CapsLock Restore (`/dev/uinput`)

Holding CapsLock to dictate has a side effect: the kernel or compositor has
already toggled the lock state on the physical key press. When you release the
key, Vinput injects one synthetic CapsLock press+release through `/dev/uinput`
to put the lock state back. Vinput therefore needs **write access to
`/dev/uinput`**.

It does *not* need — and should not get — read access to `/dev/input/event*`.
These are two different things:

| Node | What it grants | Needed by Vinput |
|------|----------------|------------------|
| `/dev/uinput` | create a virtual keyboard and inject key events | **yes** |
| `/dev/input/event*` | read every real keystroke on the machine | no |

`/dev/uinput` is a *static* node: `systemd-tmpfiles` creates it before
`systemd-udevd` starts and leaves it at `0600 root:root` unless a rule chgrps
it, so the fix has to be a persistent udev rule. The package ships one:

```udev
# /usr/lib/udev/rules.d/70-vinput-uinput.rules
KERNEL=="uinput", SUBSYSTEM=="misc", OPTIONS+="static_node=uinput", TAG+="uaccess"
```

`TAG+="uaccess"` hands the decision to `systemd-logind`: whichever user holds
the active session on a local seat gets a per-uid ACL on the node. That covers
every local account automatically — including users created *after* the
package was installed — with no group membership, no re-login, and without
granting access to the keyboard event devices. `OPTIONS+="static_node=uinput"`
is mandatory here because udev ignores static nodes without it.

### Verify

```bash
getfacl /dev/uinput
```

A working setup lists a `user:<you>:rw-` entry. If that line is missing,
CapsLock stays locked on after every dictation while the rest of voice input
keeps working; the fcitx5 log then contains
`Vinput: cannot open /dev/uinput (errno=13); CapsLock restore disabled`.

### Apply after a manual install

The `pacman` install/upgrade path does this for you:

```bash
modprobe uinput
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=misc --sysname-match=uinput
fcitx5 -r
```

### Why not group membership?

A common older pattern is `GROUP="input", MODE="0660"` plus adding the user to
the `input` group — or simply depending on another package (for example
`ydotool`'s rule) that configures `/dev/uinput` for you. It works, but (a) the
`input` group also carries read access to every `/dev/input/event*`, i.e. the
ability to log all keystrokes, and (b) group membership is per-user data, so
it cannot cover accounts created later without extra machinery. `uaccess` has
neither problem, and it targets only `/dev/uinput`.

## ASR Backends

| Provider | Type | Requires |
|----------|------|----------|
| Mock | Test | Nothing |
| Zipformer | Local (~100MB) | `sherpa-onnx` binary |
| FireRed | Local (~1.2GB) | `sherpa-onnx-offline` binary |
| Doubao | Cloud (ByteDance) | API key |
| Qwen | Cloud (Alibaba) | API key |

## Quick Start

### 1. Install

```bash
# AUR (Arch Linux)
paru -S fcitx5-vinput-git

# Manual build
meson setup build --prefix=/usr
ninja -C build
sudo meson install -C build
```

The package also installs `/usr/lib/udev/rules.d/70-vinput-uinput.rules`,
which is what lets Vinput put the CapsLock state back after dictation — see
[CapsLock Restore](#capslock-restore-devuinput) for what it does, how to
verify it, and what to do when it is missing.

### 2. Configure (cloud ASR)

Defaults are built into the code; config files under `~/.config/vinput/` are optional and sparse. Copy only what you need, and keep in mind every file supports `//` comments:

```bash
mkdir -p ~/.config/vinput

# Copy tracked examples, then edit local files under ~/.config/vinput/
cp config/config.json.example ~/.config/vinput/config.json
cp config/qwen.json.example ~/.config/vinput/qwen.json
cp config/doubao.json.example ~/.config/vinput/doubao.json

# Cloud providers: put your API key into the JSON (plaintext; chmod 600).
# Every config file is validated on read — typos and syntax errors are
# reported in the switch notification and the input panel.
```

### 3. Restart fcitx5

```bash
fcitx5 -r
```

## Local Models (Optional)

### Prepare directory layout

```bash
mkdir -p ~/.local/share/vinput/{sherpa-onnx/bin,models}
```

Expected structure after setup:
```
~/.local/share/vinput/
├── sherpa-onnx/bin/
│   ├── sherpa-onnx          # Zipformer (streaming transducer)
│   └── sherpa-onnx-offline  # FireRed (offline AED)
└── models/
    ├── zipformer-zh-en/
    │   ├── encoder-epoch-99-avg-1.onnx
    │   ├── decoder-epoch-99-avg-1.onnx
    │   ├── joiner-epoch-99-avg-1.onnx
    │   └── tokens.txt
    └── sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26/
        ├── encoder.int8.onnx
        ├── decoder.int8.onnx
        └── tokens.txt
```

### 1. Download & extract sherpa-onnx binaries

```bash
# Find latest version at: https://github.com/k2-fsa/sherpa-onnx/releases
VERSION=v1.13.2
ARCHIVE="sherpa-onnx-${VERSION}-linux-x64.tar.bz2"
curl -LO "https://github.com/k2-fsa/sherpa-onnx/releases/download/${VERSION}/${ARCHIVE}"
tar xf "$ARCHIVE"
cp -r "sherpa-onnx-${VERSION}-linux-x64/bin/"* ~/.local/share/vinput/sherpa-onnx/bin/

# Clean up
rm -rf "sherpa-onnx-${VERSION}-linux-x64" "$ARCHIVE"
```

### 2. Download & extract ASR models

| Model | Size | Cold Start | Archive |
|-------|------|------------|---------|
| Zipformer zh-en | ~100MB | ~1s | `sherpa-onnx-zipformer-zh-en-2023-06-26` |
| FireRed AED zh-en | ~1.2GB | ~3s | `sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26` |

```bash
MO="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"

# Zipformer (real-time streaming transducer)
curl -LO "$MO/sherpa-onnx-zipformer-zh-en-2023-06-26.tar.bz2"
tar xf sherpa-onnx-zipformer-zh-en-2023-06-26.tar.bz2 -C ~/.local/share/vinput/models/

# FireRed (offline AED, higher accuracy)
curl -LO "$MO/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2"
tar xf sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2 -C ~/.local/share/vinput/models/

# Clean up
rm *.tar.bz2
```

> Model archives are at [sherpa-onnx ASR models](https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models).  
> Custom paths can be set in `~/.config/vinput/zipformer.json` and `~/.config/vinput/fire_red.json`.


## Configuration

All user config lives in `~/.config/vinput/`; defaults are built into the code, so files are **optional and sparse** — copy only what you want to change. Every file supports `//` line comments. There is no `/etc` layer. See man page: `man vinput`

`config.json`, `qwen.json` and `doubao.json` self-heal: whenever a read finds
them missing, a commented template is regenerated (mode `0600`). Rename one
away to get a fresh template back. Seeded credentials carry a
`PASTE_YOUR_KEY_HERE` placeholder that counts as "not configured", so the
first recognition on a fresh install points you at the exact file to edit.

Tracked files under `config/*.example` are commented examples only. Runtime `*.json` files are ignored by Git and should stay under `~/.config/vinput/`.

```
~/.config/vinput/
├── config.json        # provider selection + [ui] + [audio]
├── qwen.json          # cloud providers: api_key, model, ...
├── doubao.json
├── zipformer.json     # local providers: model_dir, ...
└── fire_red.json
```

Cloud provider configs are re-read before each recognition, so model and parameter changes apply on the next recording without restarting fcitx5. `Ctrl+CapsLock` provider switching writes the selection back into `config.json`'s `"provider"` line.

### User-facing (must configure)

| File | Purpose | Example |
|------|---------|---------|
| `qwen.json` | Qwen API key, ASR model, hot words | copy from `config/qwen.json.example` |
| `doubao.json` | Doubao API key, resource ID, model | copy from `config/doubao.json.example` |

### Optional (all defaults in code)

| File | Purpose |
|------|---------|
| `config.json` | Active provider (rewritten by `Ctrl+CapsLock`), `[ui]` hotkey/notification tuning, `[audio]` denoiser + audio tuning |
| `zipformer.json` | Local Zipformer: model dir, binary, timeout |
| `fire_red.json` | Local FireRed: model dir, binary, timeout |

### API key security

API keys are plaintext fields in `qwen.json`/`doubao.json` (no secret store
integration; removed 2026-10 by design). Keep the files mode `0600`:
`chmod 600 ~/.config/vinput/{qwen,doubao}.json`.

### Config validation

Every config file is validated on read: JSON syntax errors (with byte
offset) show in the input panel and disable the file's values; unknown
fields get a "did you mean" suggestion, type mismatches and missing
required fields are reported — warnings appear in the fcitx5 log and as a
`⚠` line in the provider switch notification. Retired fields (e.g.
`request_style`, `max_polls`, `num_threads`) are flagged so stale files
tell you they do nothing.

### Cloud polling

Doubao's submit + result polling share one deadline: `timeout_sec` (default
`90`) is the whole-recognition budget. The poll cadence is fixed in code —
first query after 300 ms, then 500 ms, then 800 ms — which keeps short
utterances fast without exposing knobs nobody should turn.

## Dependencies

| Library | Arch Package |
|---------|-------------|
| fcitx5 | `fcitx5` |
| libebur128 | `libebur128` |
| libpulse | `libpulse` |
| libcurl | `curl` |
| speexdsp | `speexdsp` |
| libsoxr | `libsoxr` |

CapsLock restore additionally needs write access to `/dev/uinput`, granted to
local sessions by the shipped udev rule (see [Install](#1-install)). Voice
input still works without it; only the CapsLock state restore is lost.

## Audio Pipeline

```
PulseAudio → ebur128 norm (-16 LUFS) → denoise → VAD trim → WAV → ASR → commit
```

Capture uses an asynchronous PulseAudio stream. Releasing CapsLock returns
immediately while Vinput retains a short tail window before starting audio
processing, avoiding both input-method stalls and clipped final syllables.

## Reliability Notes

- Doubao and Qwen use a persistent serialized worker. This retains curl
  connection state across requests while preserving result order.
- Local Zipformer and FireRed runs remain one process per utterance. This is
  intentional; no long-running local model server is required.

## Developer Diagnostics

The developer build records one JSON object per line to
`~/.local/share/vinput/diagnostic.log`. It is disabled by default and can be
enabled with the Meson option below:

```bash
meson setup build-diagnostic . --wipe -Ddiagnostic_logging=true
meson compile -C build-diagnostic
sudo meson install -C build-diagnostic
fcitx5 -r
```

The optional `VINPUT_DIAGNOSTIC_LOG` environment variable overrides the log
path. The file is never rotated and stops accepting new events at 1 GiB. The
log contains no audio, API keys, or recognition text. Text and sensitive
identifiers are represented by length and truncated SHA-256 values.

After a reproduction, restart fcitx5 before collecting the file so the old
process flushes its final events:

```bash
fcitx5 -r
cp ~/.local/share/vinput/diagnostic.log /tmp/vinput-diagnostic.log
```

## License

MIT
