# fcitx5-ainput — Project AGENTS.md

## Project Identity And Purpose

fcitx5-ainput is a voice input addon for fcitx5 (Linux IME framework). The user
holds **CapsLock**, speaks, releases, and the recorded audio is transcribed by an
ASR backend and committed as text into the focused application. It serves users
who want fast push-to-talk dictation without leaving the keyboard.

## Functional Goals And Scope

- Push-to-talk capture (CapsLock hold), status UI via fcitx5 input panel,
  consecutive-speech queueing (max 3 in-flight recognitions, FIFO order
  preserved per app/input context).
- Pluggable ASR backends behind `IAsrProvider` / `IAsrProviderFactory` and the
  `AsrProviderRegistry`:
  - `mock` — deterministic test provider.
  - `zipformer`, `fire_red` — local providers via external `sherpa-onnx` binaries.
  - `doubao` (ByteDance), `qwen` (Alibaba DashScope) — cloud providers via libcurl,
    persistent serialized worker threads.
- Audio capture with optional denoising; cloud failures surfaced as actionable
  status errors (network / service / timeout / microphone / no-speech).
- Scope boundary: short-duration dictation only (≤ minutes). Long-audio
  file transcription (filetrans/async job APIs) is out of scope.

## Product Philosophy

- Low latency and predictable UX first; diagnostics and timers are built in
  (`Ainput Qwen [timer] encode/network/parse` logs, `diagnostic_log` events).
- Every provider must degrade loudly and recoverably: cancellable requests,
  curl handle eviction on transport failure, actionable error strings.
- Configuration split (2026-10, Rust rewrite):
  - Shared settings (provider, key timing, denoise, audio tuning) live in
    fcitx5's own `Configuration`, so they appear in `fcitx5-configtool` and land
    in `~/.config/fcitx5/conf/ainput.conf`. fcitx5 writes only non-defaults.
  - Per-provider data (credentials, model ids, endpoints, paths, timeouts) is
    `~/.config/ainput/<provider>.json`. Every file is seeded on first run with
    **all** fields, their defaults and English comments; a template that omits a
    field fails `templates_list_every_field`.
  - There is no `config.json` anymore. The pre-rewrite file is migrated once
    into `ainput.conf` and renamed to `config.json.migrated` so migration cannot
    run twice and overwrite later UI changes.
  - No `/etc` layer, and no pacnew dance: package upgrades never touch user
    files.
- Generated config templates are English only. `README.zh-CN.md` is the only
  Chinese artefact in the repository.
- Tuning knobs stay out of config unless a user has a real reason to turn
  them: request_style, the doubao poll knobs and num_threads were retired
  into code constants in 2026-10 (doubao keeps one whole-budget
  `timeout_sec`). New knobs need a justification, not just "might be
  useful".
- OS prerequisites are shipped, not assumed. CapsLock restore injects a
  synthetic CapsLock press+release through `/dev/uinput`; the package must
  therefore provide the udev rule that makes that node writable
  (`udev/70-ainput-uinput.rules`, installed by meson). It uses
  `TAG+="uaccess"` so every local user with an active session — including
  accounts created after installation — works with no group membership, and
  without granting read access to `/dev/input/event*`. Losing this rule makes
  CapsLock restore fail **silently** while voice input still works (see
  `found.md` #9).

## Project Principles

- Two halves, one boundary. `shell/` is the C++ part that must be C++ (fcitx5
  `AddonInstance`, `AinputConfig`, `KeyEvent`, `commitString`). `rust/` owns
  capture, DSP, ASR, config files, uinput and diagnostics. The only contract is
  `shell/ainput_core.h`: integers, C strings and POD structs. Never pass a C++
  type across it.
- A new ASR backend is `rust/src/asr/<id>.rs` + one entry in `DESCRIPTORS`,
  `create()` and `docs/architecture.md`. Keep `mock` as the interface spec.
- One recognition worker for the process; the pending counter is reserved
  before anything is created and released exactly once on every path.
  Cancellation is cooperative via `std::sync::atomic::AtomicBool` and must
  still produce an error, never silence.
- JSON uses `serde_json`; the old hand-written extraction is gone. Config files
  keep `//` comments, stripped by `config::strip_comments` in a
  string-aware, offset-preserving way.
- Dependencies: use maintained Rust crates where they exist and link the system
  library (`libpulse-binding`, `curl`); hand-write FFI only for libraries with
  no maintained crate (`rust/src/ffi/dsp.rs`: ebur128, speexdsp, soxr).
- Failures are never swallowed. Every failure either becomes an `Error` with a
  `Category` that the shell renders as one actionable sentence, or is reported
  to the user. Silently degrading (for example dropping denoising) is a bug.
- Documentation is one file per topic, not one file per phase of work:
  - `README.md` (English) / `README.zh-CN.md` (Chinese) — project introduction,
    install, usage, provider table, quick config. Keep engineering detail out;
    keep both languages in sync.
  - `docs/configuration.md` — every config key, default, validation, migration.
  - `docs/architecture.md` — module boundaries, data flow, provider contract,
    how to add a backend.
  - `docs/development.md` — build, tests, diagnostics, repository rules.
  - `docs/history.md` — archive: dated failure analysis plus the chronological
    log that used to be `FINDINGS.md`. Never present it as the current design.
  - `docs/qwen-asr-api.md`, `docs/doubao-auc-api.md` — vendor API notes.
- Do not reintroduce per-module readmes, `WhyThisArchWork.md` files, a generic
  fcitx5 tutorial set, or a `tools/` directory of one-off probes; those were
  removed in 2026-10. Hardware checks are the ignored Rust test plus the
  diagnostic log.
- Durable claims (archive name, download size, config path, default value,
  test count) are verified against the code or the upstream API before they
  ship. See `found.md` #12.
- Commit `rust/`, `shell/`, `docs/`, `udev/`, build and packaging files,
  `README*`, `LICENSE`, `AGENTS.md`, `found.md`. Never commit build output,
  `rust/target/`, `compile_commands.json`, runtime config, `~/.config` or
  `~/.cache` contents, models, diagnostic logs, or editor/IDE state. The full
  list is in `docs/development.md`.
- Build: meson drives `cargo build` for `rust/` and links the static archive
  into `ainput.so`; `meson test` runs the Rust unit tests.
  Packaging: PKGBUILD (Arch), makedepends include `cargo`.
  Verification: `ninja -C build`, `meson test -C build`, and (for capture or
  queue changes) `cargo test --lib -- --ignored` for the microphone test.
  Meson install carries the addon files **and** the udev rule
  (`lib/udev/rules.d`); `PKGBUILD.install` reloads udev on install/upgrade.
- Release chain: repo → GitHub (`origin`) + Gitee (`gitee`, PKGBUILD source) →
  AUR (`aur/fcitx5-ainput-git`). For a `-git` package AUR and paru can only
  display the **static** `pkgver=` placeholder (they cannot run `pkgver()`),
  so keep that placeholder at the real current version and regenerate
  `.SRCINFO` (`makepkg --printsrcinfo > /tmp/x && [ -s /tmp/x ] && cp /tmp/x
  .SRCINFO`) before every push to AUR. AUR push: SSH as `aur` with the full
  package name — `aur@aur.archlinux.org:fcitx5-ainput-git.git` (HTTPS push
  is rejected with 403). aurweb's RPC index lags a git push by ~2 minutes, so
  a stale `paru -qa` right after pushing is normal, not a packaging bug.
  The web page (`/packages/<name>`) flips over **before** the RPC index does,
  so when RPC still says `resultcount: 0` check the page before assuming the
  push failed.
- Renaming a package on AUR is not possible; the procedure is: publish the new
  `pkgbase` first, then file a **Deletion** request for the old one and link
  the replacement (`/pkgbase/<old>` -> Package Actions -> Submit Request).
  `Merge` is only worth it when the old package has votes/comments to carry
  over; with none, the guidelines state a deletion request linking the
  successor is identical. Only the maintainer can request it, and the reason
  must live in the request itself, not in package comments — once a TU acts,
  the request and `aur-requests` mail are the only record. Deletion leaves the
  old git repo cloneable, so publish the successor first (2026-10 rename:
  `fcitx5-vinput-git` -> `fcitx5-ainput-git`, PRQ#88425).
- `replaces` is inert for how AUR packages are actually consumed: paru builds
  a package and installs it with `pacman -U`, and `-U` ignores `replaces`
  (verified in a throwaway pacman root). Only `conflicts` makes an install
  offer to remove the old package. Declaring `provides`/`conflicts` against a
  name that has no package and no dependents is dead metadata — add the pair
  only when a stable non-`-git` package actually exists.
- Docs are topic-based: `docs/configuration.md`, `docs/architecture.md`,
  `docs/development.md`, `docs/history.md`, plus vendor notes
  (`docs/qwen-asr-api.md`, `docs/doubao-auc-api.md`). `docs/history.md` is an
  archive (dated analysis + old engineering log) and must never be presented
  as the current design. `README.md` and `README.zh-CN.md` stay structurally
  identical.

## Current Cloud Model Status (verified 2026-10)

- `qwen` provider is config-driven end to end (`~/.config/ainput/qwen.json`):
  model (default `qwen-audio-3.1-asr-flash`), endpoint, request style
  (auto-detected per model family), language hints, instant/precompiled hot
  words, dialect retention, speaker diarization. Config is re-read on every
  recognition, so model switches need no restart or rebuild. Legacy
  `qwen3-asr-flash` still works by setting `model` (old wire format is kept).
- `doubao` provider likewise takes `model_name` / `enable_itn` / `enable_punc`
  from `doubao.json`.
- Cross-vendor wire unification was evaluated and rejected: Doubao uses a
  submit/query poll protocol with `X-Api-Key` headers and header-based status
  codes, Qwen uses a synchronous Bearer-auth JSON call. Vendors stay one
  provider each behind `Provider`; only the config schema is unified.
- When Alibaba deprecates the next model, the expected fix is a config/template
  /doc update, not code — unless the wire protocol itself changes. The request
  format is chosen from the model name in `rust/src/asr/qwen.rs`
  (`legacy_format`).
