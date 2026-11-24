# Development

Build, test, diagnostics, and repository conventions. User-facing usage lives
in `README.md`; configuration in `docs/configuration.md`.

## Build

Requirements: a C++20 compiler, Meson, Ninja, **cargo**, and the development
packages for `fcitx5`, `libpulse`, `libebur128`, `libcurl`, `speexdsp`,
`libsoxr`.

```bash
meson setup build --prefix=/usr
ninja -C build
sudo meson install -C build
fcitx5 -r
```

Meson runs `cargo build` for `rust/` and links the resulting static archive
into the addon. The Rust target directory lives inside the build directory, so
the source tree stays clean. `meson test` runs the Rust unit tests.

Diagnostic logging is a build option:

```bash
meson setup build-diag --buildtype=release -Ddiagnostic_logging=true
ninja -C build-diag
```

It enables the `diagnostics` cargo feature.

Packaging is `PKGBUILD`. Meson installs the addon, its manifest, the man page,
and `udev/70-ainput-uinput.rules`; `PKGBUILD.install` reloads udev.

## Layout

```text
rust/            core: capture, DSP, ASR, config, uinput, diagnostics
  src/ffi/       hand-written FFI to libebur128, speexdsp, libsoxr
shell/           C++: AddonInstance, AinputConfig, key events, output handler
  ainput_core.h  the entire C ABI between the two halves
  status.h       the panel sentence for each error category
docs/            configuration, architecture, development, history
udev/            /dev/uinput rule
```

PulseAudio and libcurl use the maintained `libpulse-binding` and `curl` crates;
both link the system libraries, so the runtime dependency set is unchanged.
libebur128, speexdsp and libsoxr have no maintained Rust crate and are wrapped
by hand in `rust/src/ffi/dsp.rs` — the wrappers declare only the symbols used,
with enum values copied from the headers.

## Tests

```bash
meson test -C build          # Rust unit tests and three C++ tests, no hardware
cd rust && cargo test        # the Rust tests directly
cargo test --lib -- --ignored   # records from the real microphone
```

The ignored test is the end-to-end check: it starts the engine, records 1.5 s,
stops, and requires a callback (text or error) within 15 s. Run it after
touching capture, the queue, or the callback plumbing.

Meson also builds three C++ tests when the target can run executables:
`delivery-order` (per-context FIFO), `status-text` (one panel sentence per
error category) and `terminal-error-delivery`, which embeds a real fcitx5
event loop with two input contexts: an outcome that arrives during a newer
recording goes to the notification callback without touching the live
status, and with nothing in flight it is shown in the panel.

Unit coverage by area:

| Area | What is asserted |
|------|------------------|
| `asr::qwen` | base64 vectors, legacy/new format selection, text extraction paths, HTTP status → category, the vendor's no-words reply through the real request path, request body shape |
| `asr::doubao` | status-code mapping, result extraction, request id uniqueness |
| `asr::sherpa` | CLI argument contract, JSON text extraction, empty-result handling |
| `asr::mod` | registry order, every descriptor creates a provider |
| `config` | comment stripping (offset preserving), schema suggestions, type and required checks, tilde expansion |
| `templates` | every template parses, passes its schema, and lists every field |
| `dsp` | post-denoise activity, configurable active time, the loudness gate and both premises it needs (a denoiser, a recording R128 could measure), quiet recordings, SpeexDSP delay and error handling, concurrent denoisers, resampling, trimming |
| `settings` | defaults and clamps of every bounded option, the speech-gate parameters passed through unchanged |
| `wav` | round trip, rejection of non-WAV input |
| `engine` | queue limit, provider selection, cancellation, deadline recovery, final WAV validation and real microphone callback (ignored) |
| `uinput` | ioctl numbers are derived, not hard-coded |
| `http` | header lookup, replacing connections on transport failure or network change |
| `diagnostic` | hash stability, civil-date conversion, failure details folded onto one bounded line |

Tests that touch files set `HOME` to a temporary directory. The old C++ suite
had a bug where a test wrote into the real `~/.config`; see `found.md` #10, and
do not reintroduce a test that constructs configuration-reading code without
isolating `HOME`.

## Diagnostics

The standard fcitx5 journal identifies each dictation by `id=` and `stage=`,
from recording release through provider requests, deadline/error, and commit.
A failure line also carries its category and the core's own description, for
example `Ainput: id=7 stage=empty_result Qwen HTTP 400 CLIENT_ERROR:
ASR_RESPONSE_HAVE_NO_WORDS`, so a friendlier panel sentence never hides the
raw cause.

A `-Ddiagnostic_logging=true` build appends one JSON object per line to
`~/.local/share/ainput/diagnostic.log`. `AINPUT_DIAGNOSTIC_LOG` overrides the
path. The file stops accepting events at 1 GiB.

It contains no audio, API keys or recognized text: text and identifiers are
recorded as lengths and truncated SHA-256 hashes.

```bash
fcitx5 -r
cp ~/.local/share/ainput/diagnostic.log /tmp/ainput-diagnostic.log
```

Useful events: `engine/settings_applied`, `audio/capture_started`,
`audio/capture_stopped`, `engine/recognition_queued`,
`engine/recognition_result`, `engine/recognition_deadline`,
`engine/recording_cancelled`, `uinput/error`.

## Audio pipeline

```text
PulseAudio -> reject empty or all-zero capture -> normalize (-16 LUFS)
           -> optional denoise -> activity check -> optional crest + VAD
           -> silence trim -> WAV
```

For audio below the R128 measurement gate or shorter than its 400 ms window,
normalization falls back to RMS. Gain is capped at 1000x and limited by the
input peak to avoid clipping. Low-level background noise is amplified too.
The captured samples enter normalization as recorded. After denoising, each
20 ms frame is scored by RMS variation around its mean. At least
`MinActiveMsec` (250) of frames must meet the 64-count RMS floor (about
−54 dBFS). With a denoiser enabled, the integrated loudness of the denoised
audio must also stay within `LufsMargin` (4.2) LUFS of `LufsTarget`; the check
is skipped without a denoiser and when R128 could not measure the recording.
Setting `CrestThreshold=0` skips the additional crest and VAD checks. In this
mode, sustained noise can still pass the activity check. The normal journal
logs input and output peak, the loudness before and after denoising, gain,
peak frame RMS, active time, the gate parameters, gate result and denoiser.
These entries contain no audio. The default loudness margin and its limits
were measured through `dsp::preprocess`; see `found.md` #39.
An exactly constant capture cannot contain audio activity. It still runs
through normalization and denoising, but artifacts from that processing do
not qualify it for recognition.

Trimming runs last so the denoiser can use leading noise to build a profile.
Active frames and a varying partial final frame are kept even when trimming
VAD misses them. The SpeexDSP denoiser flushes one frame of internal delay
and keeps the original sample count.

## Configuration options

Shared settings live in `AinputConfig` in `shell/ainput.cpp`. Two rules:

- A setting that only matters when something goes wrong (buffer size, loudness
  target, suppression level, speech gate) is a `fcitx::HiddenOption`. It is
  written to `~/.config/fcitx5/conf/ainput.conf` and can be edited there, but it
  does not appear in fcitx5-configtool. Hiding rather than removing keeps the
  file complete and fcitx5's regeneration intact.
- A setting a user actually chooses (backend, hold time, notification timeout,
  denoiser choice, credentials) stays a plain `Option`.

A new option also needs a field in `AinputSettings` (`shell/ainput_core.h`) and
in `Settings` (`rust/src/settings.rs`), plus a clamp in `Settings::sanitized`.

## Performance

There is no benchmark harness; `rust/examples/` holds one small program per
question, run with `cargo run --release --example <name>`. They are listed with
their measured findings in `rust/examples/README.md`.

Two rules learned from using them:

- **Measure the stage, not the total.** "Recognition takes 800 ms" invites
  guessing; "preprocess 16 ms, network 803 ms" ends the discussion.
- **Measure the real path.** An early A/B of connection reuse used the
  transport only (fast 401 responses) and reported a 4% gain. The same A/B on
  real recognitions reported 44%, because a fast response hides the handshake
  behind nothing, while slow inference overlaps it differently. Use real audio
  and a real endpoint for anything user-visible.

Add a `rust/examples/<question>.rs` rather than extending this document with
numbers that will be stale after the next change.

## Repository conventions

Tracked: `rust/`, `shell/`, `docs/`, `udev/`, `meson.build`,
`meson_options.txt`, `PKGBUILD`, `PKGBUILD.install`, `.SRCINFO`, `README.md`,
`README.zh-CN.md`, `LICENSE`, `AGENTS.md`, `found.md`.

Never tracked: `build*/`, `rust/target/`, `compile_commands.json`, `pkg/`,
`src/` (makepkg), `*.pkg.tar.*`, `.venv/`, `~/.config` or `~/.cache` contents,
models, diagnostic logs, editor state (`.idea/`, `.vscode/`).

`.gitignore` anchors root-only entries with `/`; a bare `src/` would also hide
`rust/src` and `shell/`. See `found.md` #13 — verify with
`git check-ignore -v rust/src/probe.rs`, which must exit 1.

### Documentation conventions

One file per topic:

- `README.md` / `README.zh-CN.md` — introduction, install, usage. No
  engineering detail; keep both languages structurally identical.
- `docs/configuration.md` — every option, default, validation, migration.
- `docs/architecture.md` — module split, data flow, provider contract.
- `docs/development.md` — this file.
- `docs/history.md` — archive; never the current design.
- `docs/qwen-asr-api.md`, `docs/doubao-auc-api.md` — vendor notes.

Durable claims (archive names, sizes, paths, defaults, test counts) are checked
against the code or upstream before shipping. See `found.md` #12.

### Release chain

repo -> GitHub (`origin`) + Gitee (`gitee`, the PKGBUILD source) -> AUR.

Two AUR packages, two separate AUR repositories:

| Package | Built from | AUR repo |
|---|---|---|
| `fcitx5-ainput-git` | source at `master` (`PKGBUILD` in this repo) | `fcitx5-ainput-git.git` |
| `fcitx5-ainput-bin` | the GitHub release artifact (`PKGBUILD.bin` in this repo) | `fcitx5-ainput-bin.git` |

They install the same files, so `-bin` declares `conflicts=(fcitx5-ainput-git)`
and `provides=(fcitx5-ainput)`. Only one can be installed.

#### Cutting a release

1. Bump the version in `meson.build`, `rust/Cargo.toml` and `PKGBUILD`, and the
   man page date/version in `docs/ainput.1`. Regenerate `rust/Cargo.lock`.
2. Build both architectures:
   ```bash
   meson setup build --buildtype=release && ninja -C build
   tools/cross-aarch64.sh <aarch64-sysroot> <version>
   ```
   The cross script needs a sysroot with the target's headers, link-time `.so`
   files, and pkg-config metadata, including libcurl's private pkg-config
   requirements. It refuses to publish when the aarch64 dependency list
   differs from the native one. Meson builds the C++ tests when the target
   executables can run.
3. Produce the artifacts and their checksums:
   ```bash
   tools/make-release.sh <version> x86_64
   tools/make-release.sh <version> aarch64
   ```
   It refuses to publish if `ainput.so` is not staged. That guard exists
   because meson prepends `PKG_CONFIG_SYSROOT_DIR` to install paths, which once
   produced a tarball containing everything except the addon.
4. Put the two sha256 values into `PKGBUILD.bin` (`sha256sums_x86_64`,
   `sha256sums_aarch64`) and push `PKGBUILD.bin` + `PKGBUILD.install` to the
   `-bin` AUR repo with a regenerated `.SRCINFO`.
5. Tag and publish:
   ```bash
   git tag -a <version> -m "..." && git push origin master <version>
   gh release create <version> dist/*.tar.zst dist/*.sha256
   ```
6. Keep the static `pkgver=` placeholder in `PKGBUILD` current and regenerate
   its `.SRCINFO` before pushing to the `-git` AUR repo.

The release URL uses the tag verbatim (`/releases/download/<version>/`). A
`v` prefix there produces a 404 that only shows up when building the package
from the published release, never from a local copy of the artifact.

#### Architecture support

`x86_64` is built and tested on the development machine. `aarch64` is
cross-compiled; it is **not** run on arm hardware, so only the object format,
the dependency list and the exported symbols are checked. Treat aarch64 as
"should work, unverified" until someone reports otherwise.
