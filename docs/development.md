# Development

Build, test, diagnostics, and repository conventions. User-facing usage lives
in `README.md`; configuration in `docs/configuration.md`.

## Build

Requirements: a C++20 compiler, Meson, Ninja, and the development packages for
`fcitx5`, `libpulse`, `libebur128`, `libcurl`, `speexdsp`, `libsoxr`.

```bash
meson setup build --prefix=/usr
ninja -C build
sudo meson install -C build
fcitx5 -r
```

`meson_options.txt` has one switch:

```bash
meson setup build-diagnostic . --wipe -Ddiagnostic_logging=true
```

Packaging is `PKGBUILD` (Arch). Meson installs the addon library and manifest,
the man page, and `udev/70-ainput-uinput.rules` into `lib/udev/rules.d`;
`PKGBUILD.install` reloads udev on install and upgrade.

## Tests

```bash
meson test -C build
```

All 11 tests run without a microphone, cloud key, desktop session, or real ASR
model. Tests that touch configuration or capture point `HOME` at a temporary
directory (see `found.md` #10).

| Test | Covers |
|------|--------|
| `test_asr_registry` | provider registration, config-driven display names, `mock` contract |
| `test_audio_capture_file` | file-only pipeline: normalization, gate, trim, WAV output |
| `test_buffer_detect_cache` | per-source buffer cache and legacy-field migration |
| `test_config_read` | template validity, self-healing, 0600 mode, comment stripping |
| `test_config_schema` | syntax offsets, unknown-field suggestions, type/required checks |
| `test_provider_lifetime` | async completion, hung-child cancellation, release from callback |
| `test_curl_cancellation` | in-flight curl abort through the progress callback |
| `test_audio_capture_stop` | distinct WAV paths, non-blocking `stop()`, quick shutdown |
| `test_cloud_provider_queue` | serialized worker FIFO order, draining, temp-WAV cleanup |
| `test_diagnostic_log` | SHA-256 vectors, field integrity, hashing, size limit |
| `test_qwen_json` | Qwen response extraction, escapes, comment stripping, JSON surgery |

Test hooks: `AINPUT_PA_BUFFER_CONFIG` points the buffer cache at a temp file,
`AINPUT_PA_SOURCE_ID` overrides the detected source.

Proving HOME isolation: point `HOME` at a fresh empty directory and rerun; the
real `~/.config/ainput` and `~/.cache/ainput` must stay untouched.

## Manual tools

Built by Meson, not registered as tests. Use them when a check needs a
microphone, PulseAudio, a compositor, `/dev/uinput`, or a real audio file.

```bash
# run a WAV through the pipeline (no microphone)
./build/tools/test_audio_file input.wav output.wav [none|speexdsp|deepfilter]

# record from PulseAudio; 0 = until Ctrl+C
./build/tools/test_audio_pipeline output.wav [duration_sec=0] [denoiser=...]

# virtual keyboard CapsLock taps (needs /dev/uinput write access)
./build/tools/test_uinput_caps [--taps N] [--settle-ms N] [--between-ms N] [--full-keyboard]

# legacy tail-loss diagnosis
./build/tools/tail_loss_test [seconds] [output.wav]
./build/tools/tail_loss_test -i [output.wav]
./build/tools/tail_loss_test -s [buf_bytes]
./build/tools/tail_loss_test -d

# raw capture diagnostics
./build/tools/record_test [seconds] [output.wav]
./build/tools/record_timing [seconds] [buf_bytes]
./build/tools/calibrate_silence [seconds]
```

`tail_loss_test -s` reads the first `buffer_bytes` value in
`~/.cache/ainput/pa_buffer.json`, which may not be the currently selected
source when several devices are cached.

`calibrate_silence` prints a suggested `crest_threshold`; set it in
`~/.config/ainput/config.json` under `[audio]`.

## Diagnostics

Developer builds (`-Ddiagnostic_logging=true`) append one JSON object per line
to `~/.local/share/ainput/diagnostic.log`. `AINPUT_DIAGNOSTIC_LOG` overrides
the path. The file is never rotated and stops accepting events at 1 GiB.

It contains no audio, API keys, or recognized text: text and identifiers are
recorded as lengths and truncated SHA-256 values. Restart fcitx5 before
collecting it so the old process flushes:

```bash
fcitx5 -r
cp ~/.local/share/ainput/diagnostic.log /tmp/ainput-diagnostic.log
```

## Audio pipeline

```text
PulseAudio -> ebur128 norm (-16 LUFS) -> speech gate (crest + speexdsp VAD)
           -> denoise (only when configured) -> silence trim -> WAV
```

Denoising is off unless `audio.denoise` is set. `speexdsp` needs no extra
software; `deepfilter` expects an executable at
`~/.local/share/ainput/bin/deep-filter` and silently falls back to
undenoised audio when it is missing.

Capture uses an asynchronous PulseAudio stream: releasing CapsLock returns to
fcitx5 immediately, a short tail is retained, then processing starts.

## Repository conventions

Tracked (source of truth):

- code, `meson.build` files, `meson_options.txt`
- `PKGBUILD`, `PKGBUILD.install`, `.SRCINFO`
- `tests/`, `tools/`, `udev/`, `config/*.example`
- documentation (`README.md`, `README.zh-CN.md`, `docs/`), `LICENSE`
- `AGENTS.md` (project definition and working rules) and `found.md`
  (lessons learned)

Never tracked (build output, local state, third-party dumps):

- `build/`, `builddir/`, `compile_commands.json`, `pkg/`, `src/` (makepkg),
  `*.pkg.tar.*`
- `.venv/`, any Python environment
- `config/*.json` (runtime config; only `*.example` is tracked)
- `~/.config/ainput`, `~/.cache/ainput`, `~/.local/share/ainput` contents
  (diagnostic log, models, binaries)
- editor/IDE state (`.idea/`, `.vscode/`), `.cortexkit/`, `log.txt`, `out/`

`.gitignore` encodes this list. A tracked file that only exists to configure
one machine's editor or tooling is a bug — remove it rather than documenting
it.

### Documentation conventions

- One file per topic, not per phase of the work. `docs/architecture.md` owns
  module boundaries and the provider contract; `docs/development.md` owns
  build/test/tooling; `docs/configuration.md` owns every config key.
- No duplicate tables. Link to the owning document instead of restating it.
- Historical material is confined to `docs/history.md`, which starts with a
  status table and keeps its original wording.
- A durable claim (archive name, download size, path, default, test count) is
  verified against the code or upstream before it ships. See `found.md` #12.

### Release chain

repo -> GitHub (`origin`) + Gitee (`gitee`, the PKGBUILD source) -> AUR
(`aur/fcitx5-ainput-git`). Keep the static `pkgver=` placeholder current and
regenerate `.SRCINFO` before pushing to AUR. Details in `AGENTS.md`.
