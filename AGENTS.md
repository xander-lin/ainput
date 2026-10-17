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
- Configuration layout (2026-10 restructure, user-selected): one global
  `config.json` (`provider` selection + `[ui]` + `[audio]` sections, `//`
  comments allowed, switch write-backs land here) plus one file per provider
  (`qwen.json`, `doubao.json`, `zipformer.json`, `fire_red.json`) for
  credential isolation. Defaults live in code only — files are optional and
  sparse, there is no `/etc/vinput` layer (removed 2026-10: no pacnew noise,
  no test-isolation footgun). `advanced.json`, `vinput.json` and
  `audio.json` are historical names that no longer exist.
- API keys are plaintext JSON fields (secret-store integration was added
  and then removed in 2026-10 by explicit decision: no Secret Service on
  the target desktop; simplicity wins). Recommend `chmod 600` on the
  credential files.
- Every config file is schema-validated on read (`src/config_schema.h`):
  syntax errors carry a byte offset and disable the file (input-panel
  error), unknown fields get "did you mean" suggestions, type mismatches
  and missing required fields are logged and appended to the switch
  notification. Retired fields are flagged as unknown so stale files tell
  the user they do nothing. Schemas live next to the module; update them
  together with the readers.
- Config discovery is self-healing, not doc-first: config.json/qwen.json/
  doubao.json regenerate as commented templates whenever a read finds them
  missing (users rename a file away to recover the template); the seeded
  api_key placeholder counts as "not configured" so errors point at the
  exact file. `src/config_templates.h` holds the texts — they must stay
  valid JSON after comment stripping and pass their schemas.
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

- Modular, pluggable providers: a new ASR backend = new `{id}_provider.{h,cpp}`
  in `ASR_provider/src/` + factory self-registration + one line in
  `adapter/src/ainput.cpp` to link. Keep mock provider as the interface spec.
- Cloud providers own a single serialized worker thread; tasks are queued;
  cancellation is cooperative via `std::atomic_bool`.
- JSON parsing in providers is intentionally dependency-free (string find /
  manual extraction). Keep it minimal but robust to field order.
- Build: meson (`meson.build`, `meson_options.txt`, per-dir `meson.build`).
  Packaging: PKGBUILD (Arch). Verification: `ninja -C build` + tests under
  `tests/`. Meson install carries the fcitx5 addon files **and** the udev
  rule (`lib/udev/rules.d`); `PKGBUILD.install` reloads udev on
  install/upgrade.
- Release chain: repo → GitHub (`origin`) + Gitee (`gitee`, PKGBUILD source) →
  AUR (`aur/fcitx5-ainput-git`). For a `-git` package AUR and paru can only
  display the **static** `pkgver=` placeholder (they cannot run `pkgver()`),
  so keep that placeholder at the real current version and regenerate
  `.SRCINFO` (`makepkg --printsrcinfo > /tmp/x && [ -s /tmp/x ] && cp /tmp/x
  .SRCINFO`) before every push to AUR. AUR push: SSH as `aur` with the full
  package name — `aur@aur.archlinux.org:fcitx5-ainput-git.git` (HTTPS push
  is rejected with 403). aurweb's RPC index lags a git push by ~2 minutes, so
  a stale `paru -qa` right after pushing is normal, not a packaging bug.
- Docs live in `docs/` (`qwen-asr-api.md` etc.), accumulated engineering notes
  in `FINDINGS.md` (historical, at repo root).

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
  provider each behind `IAsrProvider`; only the config schema is unified.
- JSON parsing helpers live in `src/qwen_json.h` and are unit-tested in
  `tests/test_qwen_json.cpp`. When Alibaba deprecates the next model,
  the expected fix is a config/example/doc update, not code — unless the wire
  protocol itself changes.
