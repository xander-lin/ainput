# fcitx5-ainput — found.md (lessons learned)

## 1. Alibaba DashScope ASR model lifecycle (discovered 2026-10)

- **Mistake/context**: The `qwen` provider was built against `qwen3-asr-flash`
  (non-realtime, OpenAI-compatible multimodal call). Alibaba treats ASR models
  as evolving lines: mainline auto-updates to dated snapshots (notice 118053,
  2026-03), and snapshots get deprecated with a *different series* as the
  official replacement (notice 118434, 2026-07:
  `qwen3-asr-flash-2026-02-10` → `fun-asr`, same price 0.00022 CNY/s).
- **Root cause**: Coding a hard-coded model name against a fast-moving cloud
  catalog without tracking deprecation notices.
- **Correction**: Latest generation is Qwen-Audio-3.x-ASR-Flash / Fun-ASR-Flash
  (short audio, sync, base64 OK). Migration is NOT a model-name-only change:
  request content becomes `{"type":"input_audio","input_audio":{"data":...}}`,
  `parameters.format` becomes **required**, `asr_options.enable_itn` no longer
  exists, and the response drops `choices` in favor of `output.text` /
  `output.sentence.text`.
- **Prevention**:
  - Before touching the Qwen provider, re-check
    https://help.aliyun.com/zh/model-studio/asr-model and the 百炼 notices page.
  - Cloud model IDs, endpoints, and feature switches live in per-vendor config
    files, never hardcoded; config is re-read per request (implemented
    2026-10, `QwenSettings` / `DoubaoSettings`).
  - Response parsing must target the documented field paths; the legacy
    `find("\"text\":\"")` trick only coincidentally matched the new schema.
    `src/qwen_json.h` now holds scope-aware extractors with unit tests.
  - Old endpoint `dashscope.aliyuncs.com` still works, but new deployments
    prefer `{WorkspaceId}.cn-beijing.maas.aliyuncs.com` — the endpoint is
    configurable via `qwen.json`.

## 2. Manual JSON parsing needs bracket-depth bookkeeping (found 2026-10)

- **Mistake/context**: `qjsonRawValue` (config passthrough of `language_hints`
  arrays and `vocabulary` objects) initialized its scan cursor at `pos + 1`,
  so the opening `[`/`{` was never counted; depth went 0 → -1 at the closing
  bracket and the function returned "" — hot words and language hints were
  silently dropped.
- **Root cause**: Hand-rolled bracket matching that forgot the opening
  delimiter must participate in depth counting; the scalar branch's `pos + 1`
  start was copy-carried into the container branch.
- **Correction**: Scan from `pos` so the opening bracket increments depth;
  caught by `tests/test_qwen_json.cpp` ("raw array value" / "raw object value")
  before release.
- **Prevention**:
  - Every new manual parser path must have a unit test exercising a realistic
    payload (see `tests/test_qwen_json.cpp` for the patterns: field order
    swapped, nested same-name keys, escapes, braces inside strings).
  - When a passthrough silently yields "", log or test it — silent config
    drops look like "the cloud ignored my settings".

## 3. JSON property removal needs comma/whitespace bookkeeping (found 2026-10)

- **Mistake/context**: `qjsonRemoveTopLevelProperty` (written for stripping
  `api_key` after a keyring import; that import path was since removed, but
  the parsing lesson applies to any JSON text surgery) initially removed only
  the key/value span, leaving
  either a double comma (middle property) or a blank line (first property in
  pretty-printed JSON) — the first variant produced invalid JSON.
- **Root cause**: Deleting a JSON property means also deleting exactly one of
  its adjacent separators (the comma before OR after, never both, never
  none), and deciding whether surrounding whitespace is the removed line's
  indent or the next property's indent.
- **Correction**: middle/last property → drop the preceding comma; first
  property → drop the following comma plus the removed line's indentation,
  keep the next property's indent. Covered by tests in
  `tests/test_qwen_json.cpp` (`testRemoveTopLevelProperty`).
- **Prevention**: any text surgery on user-owned files must round-trip
  through a parser check in tests (`qjsonStringValue(stripped, ...)` must
  still decode every remaining field) before shipping.

## 4. "Already clean" checks must be scope-aware (found 2026-10)

- **Mistake/context**: `importConfigSecret` (the keyring-import step, since
  removed with the secret store in 2026-10) decided whether a config file
  still needed its `api_key` stripped by searching for the key name anywhere
  in the file; a nested `vocabulary.api_key` made it think the file was
  dirty, the rewrite "failed", and the key was never cached.
- **Root cause**: First-occurrence string search vs. top-level scope: the
  naive check cannot distinguish a top-level property from a same-named
  nested one.
- **Correction**: Use the scope-aware walker (`qjsonTopLevelString` on the
  root object) for presence checks; nested-only matches mean "already clean".
- **Prevention**: In this codebase, never use bare `find("\"key\"")` presence
  checks for JSON semantics — always the scope-aware helpers in
  `qwen_json.h`.

## 5. Tests must isolate HOME from the installed /etc/vinput fallback (found 2026-10)

> The `/etc/vinput` fallback layer described here was removed in 2026-10. The
> lesson generalizes: any test that constructs a component reading global
> config must own `HOME`, because the file that is read may live outside the
> test's temp tree (today: a self-healing seed, previously: `/etc`).

- **Mistake/context**: After the package was installed system-wide,
  `test_cloud_provider_queue` started making REAL HTTPS requests to
  DashScope (HTTP 401 InvalidApiKey): `readConfigFile` copies
  `/etc/vinput/qwen.json` (which ships the placeholder `sk-YOUR_API_KEY`)
  into the test HOME when the user file is missing, and the placeholder is
  a non-empty key, so the provider went to the network.
- **Root cause**: The config fallback copy is correct production behavior,
  but tests assumed "missing user config" equals "no config"; they never
  accounted for a packaged /etc being present on the build machine.
- **Correction**: Tests that exercise cloud providers write their own
  qwen.json/doubao.json into the test HOME (empty key + unroutable
  endpoint). The secret-store injection (`NullSecretStore`) from that era no
  longer exists — the keyring layer was removed in 2026-10 — so the file
  contents alone decide whether a test can reach the network. Local-provider
  tests create the model files the preflight requires.
- **Prevention**: Any test touching `readConfigFile` must pre-create every
  config file it cares about inside its temp HOME; watch for test runtimes
  that jump (0.4s -> 0.01s here was the network disappearing).

## 6. Breaking config migrations must reconcile user customizations (found 2026-10)

- **Mistake/context**: Removing `advanced.json` moved every tuning key into
  per-provider files with code defaults, but the user's existing
  `advanced.json` values were not merged — a hand-tuned
  `crest_threshold: 0` (silence gating disabled) silently reverted to the
  2.4 default, changing recording behavior after upgrade.
- **Root cause**: The restructure treated the breaking change as
  file-layout-only; user-owned values inside the removed file were treated
  as disposable, which is correct only for untouched defaults.
- **Correction**: Manually merged the user's values into the then-current
  `~/.config/ainput/audio.json` and trashed the stale `advanced.json`. (The
  2026-10 restructure later folded `audio.json` into `config.json`'s
  `[audio]` section; the lesson is about carrying values across a breaking
  change, not about the filename.)
- **Prevention**: When a breaking change removes or renames a config file,
  diff the user's file against the shipped example of the old version;
  anything that differs from the old default must be carried into the new
  layout (or explicitly reported), never silently dropped.

## 7. Never redirect a command's output onto its own input path (found 2026-10)

- **Mistake/context**: A chained command did `cd /tmp/pkgdir && ... ; sed
  ... "$REPO/PKGBUILD" > PKGBUILD`. The `cd` failed, `&&` stopped, but the
  `;`-separated `sed` still ran from the repo root, where `> PKGBUILD`
  truncated the very file sed was reading — the repo PKGBUILD became empty
  and was committed that way.
- **Root cause**: Relative output redirection resolves before the reader
  opens the file, so redirecting onto the input path empties it; the failed
  `cd` shifted the working directory silently because `;` ignores the
  failure.
- **Correction**: Restored the file from the previous commit and amended.
- **Prevention**:
  - Transform files in place via a temp copy (`sed ... src > /tmp/x && mv`),
    or use `sed -i`; never `cmd src > src`.
  - Prefer `set -e`-style strict chaining (stop on first failure) in shell
    one-liners; a failing `cd` must abort the rest.
  - After any file-producing shell step, verify non-empty before committing
    (`wc -l`) — the empty PKGBUILD sailed through `git add -A`.

## 8. A redirect truncates the target even when the producing command fails

- **Mistake/context**: `makepkg --printsrcinfo > .SRCINFO` ran in the repo
  root, where `pkgver()` cannot evaluate (no VCS source dir yet) and makepkg
  errored out with "pkgver is not allowed to be empty". The shell had already
  created/truncated `.SRCINFO`, so a zero-byte file was committed — the same
  failure mode as #7, one indirection away.
- **Root cause**: `>` is applied by the shell before the command's exit status
  is known; a command that prints nothing leaves an empty target. Nothing in
  the pipeline reports "the producer died" to git.
- **Correction**: Regenerated `.SRCINFO` through a guarded sequence
  (`cmd > tmp && [ -s tmp ] && mv tmp target`) and committed the result.
- **Prevention**:
  - Never `cmd > tracked_file` in one step for a generated/tracked file. Use
    `cmd > /tmp/x && [ -s /tmp/x ] && cp /tmp/x tracked_file`, or check
    `$?` and the byte count before the file can reach `git add`.
  - Generated files need a generator that works in-repo; if the generator
    only works in a build directory, generate there and copy the result back.
  - `git add -A` will happily stage empty files; check `git diff --cached
    --stat` for suspicious deletions of line counts before committing.

## 9. CapsLock restore depended on a udev rule the project never shipped
  (found 2026-10)

- **Mistake/context**: On one machine (x18) CapsLock stopped being restored
  after push-to-talk: press CapsLock, release, and the lock stayed on — while
  another machine (x19) running the same build was fine. Nothing in the code
  had changed; the regression had no visible date.
- **Root cause**: `AinputAddon::revertCapsLock()` needs write access to
  `/dev/uinput` to inject a synthetic CapsLock press+release. `/dev/uinput` is
  a **static node** created by systemd-tmpfiles at boot and left at
  `0600 root:root` unless some udev rule chgrps it. The project never shipped
  such a rule, so the capability silently depended on an unrelated package's
  rule (on x19 `ydotool`'s `80-uinput.rules` did it, and the login user
  happened to be in the `input` group). On x18 none of those packages were
  installed, `initUinput()` hit `open() = EACCES` and only logged
  `Ainput: cannot open /dev/uinput` at INFO level — voice input still worked
  perfectly, so the failure looked like "the fix mysteriously disappeared".
- **Correction**: Shipped `udev/70-ainput-uinput.rules`
  (`KERNEL=="uinput", SUBSYSTEM=="misc", OPTIONS+="static_node=uinput",
  TAG+="uaccess"`), installed by meson into `lib/udev/rules.d`, plus a udev
  reload in `PKGBUILD.install`.
- **Prevention**:
  - When a feature depends on an OS-level prerequisite (device node, kernel
    module, group, capability), the **package must provide** that
    prerequisite; never rely on another package happening to configure it.
  - For "every local user, including future ones" access, use
    `TAG+="uaccess"` (systemd-logind grants the active seat session), not
    `GROUP=` + `usermod`: groups are per-user data and cannot cover accounts
    created later without extra machinery. Avoid `input` group for this — it
    also grants read access to every `/dev/input/event*` (keylogging).
  - A device node is an ordinary file node: permissions are normal POSIX
    mode/ACL, but `/dev` is devtmpfs, so anything set by hand is reset at boot
    (see the `c!` entries in `/run/tmpfiles.d/static-nodes.conf`). Persistent
    changes must come from udev/tmpfiles rules.
  - `static_node=` is mandatory for `/dev/uinput`; rules without it do not
    touch static nodes (udev(7)).
  - Watch for capabilities that fail **silently and degradedly**: verify the
    prerequisite after install, and prefer a loud, actionable error over an
    INFO log. `getfacl /dev/uinput` showing a `user:<you>:rw-` line is the
    one-command check.

## 10. A config-seeding test leaked into the real HOME (found 2026-10)

- **Mistake/context**: While renaming the project's config directory
  (`~/.config/vinput` -> `~/.config/ainput`), a generated `config.json`
  template kept appearing at the *new* real path during `meson test`.
  `tests/test_audio_capture_stop.cpp` constructs `AudioCapture`, whose
  `start()` reads the global config, so the self-healing seed wrote the
  template into whatever `HOME` pointed at. The test never set `HOME`, so
  that was the real user HOME.
- **Root cause**: The same class of bug as #5, but hidden for months because
  `~/.config/vinput` **already existed** on every dev machine — the seed only
  fires when the file is *missing*, so an existing directory silently masked
  the leak. Renaming the directory (a path that did not exist yet) exposed it.
- **Correction**: `test_audio_capture_stop.cpp` now creates a temp root under
  `MESON_TEST_TMPDIR` (fallback `/tmp/ainput-audio-capture-stop`), makes
  `<root>/.config/ainput`, and `setenv("HOME", root, 1)` before constructing
  `AudioCapture`, matching `test_asr_registry.cpp` / `test_config_read.cpp`.
- **Prevention**:
  - A green test suite does **not** prove HOME isolation. Verify by deleting
    the real config dir (or pointing `HOME` at a fresh temp dir) and re-running
    every test binary, then checking that the real `~/.config/ainput` and
    `~/.cache/ainput` are still absent — not just that the tests pass.
  - Any test that constructs a component which reads global config
    (`AudioCapture`, providers, `OutputHandler`, `diagnosticLog`) must set
    `HOME` first; "it does not call `readConfigFile` directly" is not enough,
    because the read can happen inside the component's constructor/`start()`.
  - When a rename touches a path that is normally pre-existing, treat the
    window where it does not exist as a rare chance to catch masked
    path-dependent bugs (see also the `ainput/config.json` stray noted during
    this rename).

## 11. A real fix sat trapped in a dangling commit (found 2026-10)

- **Mistake/context**: The `docs/vinput.1` switch paragraph ended a bold run
  with `\fB)` instead of `\fP)`, so `read from the provider's config file.
  While holding CapsLock:` rendered **bold** for months. The fix already
  existed — it was in commit `0a12dc8`, which never reached master because it
  was a pre-rebase/pre-amend copy of `905b5a5` and became unreachable.
  Discovered only while auditing local branches for the fcitx5-vinput ->
  fcitx5-ainput rename.
- **Root cause**: Rebase/amend rewrites commit objects. The superseded object
  keeps the *same commit title* but is no longer on any branch, so it shows up
  only as a `dangling commit` in `git fsck`. Comparing by title makes the
  duplicate look like "already applied", hiding the extra hunk it carried.
  Nobody re-diffed the tree.
- **Correction**: Applied `\fB)` -> `\fP)` to `docs/ainput.1` and verified by
  rendering (`groff -man -Kutf8 -Tutf8`, tracking bold state per text run) that
  the tail of the paragraph is no longer bold.
- **Prevention**:
  - Before deleting stale branches or consolidating history, run
    `git fsck --lost-found --no-reflogs` and inspect every `dangling commit`.
    Judge it by `git diff <master-counterpart> <dangling>`, **never** by commit
    title or by `merge-base --is-ancestor` of a sibling.
  - A dangling commit whose tree hash equals its master counterpart is a pure
    duplicate (e.g. `0c1e5f7` vs `f6691ec`); one that differs by even 2 lines
    may hold the only copy of a fix.
  - Trust the rendered output over the source when editing roff: unbalanced
    `\f` switches leak formatting silently and produce no groff warning even
    under `-ww`.



## 12. Documentation sprawl produced instructions that could not work
  (found 2026-10)

- **Mistake/context**: A docs audit found both structural sprawl and factual
  errors. Nine entry points covered five topics (`README.md`, per-module
  `ASR_provider/README.md` and two `WhyThisArchWork.md`, `config/`,
  `tests/`, `tools/` readmes, a `docs/README.md` index), and the user-facing
  README carried build flags and module contracts. The audit also found text
  describing a program that did not exist:
  - The README's local-model section told users to download
    `sherpa-onnx-zipformer-zh-en-2023-06-26.tar.bz2`, which returns **404**
    (the asset never existed; the streaming archive is
    `...-bilingual-zh-en-2023-02-20`), and told them to download
    `sherpa-onnx-v1.13.2-linux-x64.tar.bz2`, also 404 (the release publishes
    `-static`, `-shared`, and GPU variants).
  - Both local models were listed as "~100MB" / "~1.2GB" when the real
    downloads are ~490 MB / ~800 MB, and the man page and README disagreed on
    where the buffer cache lives (`~/.config/ainput` vs `~/.cache/ainput`).
  - The addon manifest declared `Configurable=True` while the addon has no
    `getConfig()`, so fcitx5-configtool offered an empty Ainput page.
  - The dated failure analysis presented Hyprland-only code as current months
    after `dfd0d55` deleted the compositor branches, and a copy of a generic
    fcitx5 CMake/fcitx-config tutorial sat next to project docs with no
    warning that this project uses Meson and its own JSON config.
  - `tests/README.md` described 3 of 11 tests; `tools/README.md` omitted
    `test_uinput_caps` and gave no `tail_loss_test` flags.
  - There was no Chinese README, and tracked `.idea/` files carried one
    machine's IDE settings.
- **Root cause**: Two failures with one shared cause — no ownership and no
  verification. (a) Nothing ever executed the download URLs, compared config
  paths against the code, or counted registered tests, so claims drifted
  freely. (b) Every topic was restated several times, so the copies disagreed
  and no single file was authoritative.
- **Correction**: Verified every factual claim against the tree and the
  upstream GitHub releases API (`gh api .../releases/tags/asr-models`, asset
  sizes, tar listings) and rewrote the wrong parts. The streaming download was
  executed end to end: `sherpa-onnx` with
  `--encoder/--decoder/--joiner/--tokens` recognized a test WAV.
  Structure was then collapsed to one file per topic:
  `docs/architecture.md` (module boundaries + provider contract),
  `docs/configuration.md`, `docs/development.md` (build/test/tools/diagnostics
  **and** what to commit), `docs/history.md` (dated analysis + old log, with a
  status table), plus the two vendor notes. The per-module readmes,
  `WhyThisArchWork.md` files, `docs/README.md`, and the generic fcitx5 tutorial
  set were deleted. `scripts/test_ws_server.py` and `scripts/df_denoise.py`
  belonged to designs the code no longer has and were removed. `README.md` was
  cut back to project introduction (no engineering detail) and translated to
  `README.zh-CN.md`. `Configurable=True` became `False`, and tracked `.idea/`
  state was untracked and ignored.
- **Prevention**:
  - One file per topic. Adding a second file that restates an existing table
    is the bug — extend the owner instead, and link rather than copy.
  - User-facing docs stay user-facing: build flags, module contracts, and
    repository conventions belong in `docs/development.md` / `docs/architecture.md`.
  - Keep `README.md` and `README.zh-CN.md` structurally identical (same
    headings, same order) so a change to one is obviously a change to both.
  - Every document is either current or explicitly archived under
    `docs/history.md`. Nothing is both.
  - Check tracked files against the `docs/development.md` list before
    committing; IDE state (`.idea/`, `.vscode/`) and runtime config never
    belong in the repository.
  - Any doc that names an external archive, URL, or default value must be
    checked against the upstream API or the code before it ships — a plausible
    name is not a valid asset name.
  - Configuration paths are code facts: `grep` the reader (`configDir()`,
    buffer cache, diagnostic log) instead of copying the previous doc.
  - Documentation that describes tests or tools must list what Meson actually
    registers (`tests/meson.build`, `tools/meson.build`).
  - Prefer a short pointer to the authoritative file over duplicating a table
    that will drift; where duplication is unavoidable, verify both copies.
## 13. A bare `src/` in .gitignore silently hid real source directories
  (found 2026-10)

- **Mistake/context**: `.gitignore` used `src/` to exclude the directory
  `makepkg` extracts next to the PKGBUILD. In gitignore syntax a pattern
  without a leading slash matches at **any** depth, so the rule also covered
  `ASR_provider/src/` and `adapter/src/`. Those directories are already
  tracked, which masks the problem: existing files still show up in
  `git status`, but a **new** file created there is ignored without a word.
  `git check-ignore -v ASR_provider/src/<new>.cpp` reported
  `.gitignore:6:src/` as the culprit.
- **Root cause**: Ignore entries were written with the mental model "path
  relative to repo root", but gitignore only anchors for patterns containing a
  slash in a non-trailing position — `build/` matches `xff/build/` too.
  `pkg/`, `out/`, and `log.txt` had the same latent problem.
- **Correction**: Anchored every root-only entry with a leading slash
  (`/build/`, `/builddir/`, `/pkg/`, `/src/`, `/out/`, `/log.txt`,
  `/compile_commands.json`) and left only genuinely global patterns
  (`*.pkg.tar.*`, `.venv/`, `config/*.json`, IDE state) unanchored.
- **Prevention**:
  - Anchor repo-root-only ignore entries with `/`. Assume anything unanchored
    applies at every depth.
  - After editing `.gitignore`, prove it: create a throwaway file in each
    directory that must stay tracked (`git check-ignore -v
    ASR_provider/src/probe.cpp` must exit 1), and one that must be ignored,
    then remove both.
  - "Already tracked so it works" is not a verification — gitignore only
    affects untracked paths, so the failure appears on the next new file.
## 14. Porting the core to Rust: what the rewrite had to fix first
  (found 2026-10)

- **Mistake/context**: The C++ core left several failures unobservable (a
  missing denoiser helper degraded silently, config was read once per process,
  a cancelled provider request returned without completing the queue entry).
  A Rust port was proposed. Before committing, the fcitx5 load path was
  verified with a throwaway addon: `dlopen` → `dlsym("fcitx_addon_factory_instance")`
  returns a `fcitx::AddonFactory *`, so a C++ shell can call a Rust
  `staticlib` through a plain C ABI. Only one C symbol is needed; no fcitx5
  type has to cross the boundary.
- **Root cause**: The load protocol is C++-shaped (vtable + `std::string`), so
  a "pure Rust addon" means reimplementing the Itanium C++ ABI by hand — which
  breaks silently on fcitx5 changes. Anything *below* that boundary is plain C
  and POSIX, where Rust is a better fit.
- **Correction**: Split the addon: `shell/` (C++, `AddonInstance`,
  `AinputConfig`, key events, `OutputHandler`) and `rust/` (capture, DSP, ASR,
  config, uinput, diagnostics), joined by `shell/ainput_core.h`. While porting,
  each silent failure was converted into a categorised `Error`.
- **Prevention**:
  - Verify a foreign-language addon by actually loading it into fcitx5 and
    checking `Loaded addon <name>` in the log; a successful link proves nothing.
  - Keep the FFI surface to integers, C strings and POD structs. A `std::string`
    or a class with a destructor in the signature is a red flag.
  - When rewriting, port each known bug to a test or an explicit rule; a
    straight translation would have reproduced them.

## 15. Three concrete traps hit while porting (found 2026-10)

- **`UI_SET_LEDBIT` is `_IOW('U', 105, int)`, not 103.** 103 is `UI_SET_ABSBIT`,
  so the ioctl returned EINVAL and the whole `initUinput()` sequence failed with
  a message that named the wrong cause. Deriving the numbers from `_IOW(base,
  nr, size)` in code (`const fn iow`) prevents the next transcription error.
- **`UI_DEV_SETUP` is mandatory.** Skipping it makes `UI_DEV_CREATE` fail; the
  kernel wants `struct uinput_setup` (name + `input_id` with
  `bustype = BUS_VIRTUAL`) first. Also: `UI_SET_EVBIT` rejects `EV_SYN`, so
  enabling it there fails with EINVAL even though `EV_SYN` is sent later.
- **fcitx5's `Configuration` has no `double`.** `Option<double>` fails to
  compile: there is no `marshallOption` overload and no
  `configTypeNameHelper(double*)`. A custom `Marshaller` plus an explicit
  `OptionTypeName<double>` specialization works; note that
  `FCITX_SPECIALIZE_TYPENAME(double, ...)` does **not**, because the helper is
  a function overload and two-phase lookup cannot find it for a fundamental
  type.

## 16. Generated config must migrate exactly once (found 2026-10)

- **Mistake/context**: The 0.2 migration reads the 0.1 `config.json`, copies its
  values into fcitx5's configuration, and left the original in place. On the
  next start it would run again and overwrite anything the user had since
  changed in `fcitx5-configtool`.
- **Root cause**: "Do not delete the user's file" was implemented as "leave it
  where the migration looks for it", so the migration had no way to know it had
  already run.
- **Correction**: After a successful save the file is renamed to
  `config.json.migrated` (with a numeric suffix if that exists). Nothing is
  deleted, and the migration is idempotent.
- **Prevention**:
  - Any one-shot migration needs a durable "already done" marker, and the
    marker must be written **after** the new data is safely stored.
  - Renaming preserves the old file for the user while removing it from the
    discovery path; prefer that over deleting or over a marker file elsewhere.
  - Editing `~/.config/fcitx5/conf/*.conf` while fcitx5 runs is pointless: it
    rewrites the file on exit. Stop fcitx5 first, then edit, then start.

## 17. A tested feature that was never wired into production (found 2026-10)

- **Mistake/context**: The project shipped hardware-buffer auto-detection:
  `buffer_detect.cpp` recorded 6 s, clustered read latencies to find the
  device's burst period, cached the result per PulseAudio source in
  `~/.cache/ainput/pa_buffer.json`, and had its own unit test plus a
  `test_audio_pipeline` tool printing the detected value. A user asked where
  that cache file was on their machine. It did not exist: `~/.cache/ainput`
  was absent, and the cache was never written. `loadOrDetectBufferBytes()` had
  exactly three callers — the test and the tool. The production capture path
  had `attr.fragsize = 3200;` hard-coded.
- **Root cause**: The detection was developed in isolation and validated
  against itself. The test asserted that the cache migrates and round-trips,
  which it did; nothing asserted that the *capture path consumes the value*.
  A green test suite therefore described a code path production never took,
  and the docs described a cache file that was never created.
- **Correction**: First replaced the detection with a `BufferMs` setting, then
  measured the actual delivery behaviour and removed that too — see #18. The
  fragment size is now a code constant (20 ms), logged as
  `audio/capture_buffer` with the computed byte count.
- **Prevention**:
  - A test proves a component works; it does not prove production uses the
    component. For every feature, grep for callers **outside** `tests/` and
    `tools/` before claiming the feature exists.
  - Retrieval/derivation code should log the value it actually applied at the
    point of use (`capture_buffer` here). Absent that log, an unwired feature
    is invisible even to a user who goes looking for its output.
  - When documentation names an artefact a user can find on disk (cache file,
    log, socket), that artefact must be reproduced on a clean machine during
    review. "It is written by `detectHardwareBurstBytes`" was true and
    irrelevant.

## 18. The mic buffer is not a tunable, and not detectable either (found 2026-10)

- **Mistake/context**: After #17 the fragment size became a user setting
  (`BufferMs`) with advice to "raise it if the end of a phrase is cut off,
  lower it for quicker response". A user pushed back: a buffer should be
  detected, not hand-tuned, and only one value can be optimal. Measuring the
  real device settled both questions.
- **Measurement**: a probe requested 5/10/20/50/100/200/400 ms fragsize, read
  for 3 s, and reported granted size, per-read chunk sizes, and total captured
  with and without a post-stop drain:

  | requested | granted | chunk size | captured (no drain) | captured (120 ms drain) |
  |-----------|---------|------------|---------------------|-------------------------|
  | 20 ms | 640 B | 640 B | 2980 ms | 3100 ms |
  | 50 ms | 1600 B | 1600 B | 2950 ms | 3100 ms |
  | 100 ms | 3200 B | 3200 B | 2900 ms | 3100 ms |
  | 200 ms | 6400 B | 6400 B | 2800 ms | 3000 ms |
  | 400 ms | 12800 B | 12800 B | 2800 ms | 2800 ms |

- **Root cause**: PulseAudio echoes the requested fragsize back as the delivery
  size. There is no device-specific "burst period" to discover through the
  stream API, so the auto-detection in #17 was measuring its own request. What
  *is* deterministic is different: audio buffered but not yet forming a full
  fragment is discarded when the stream closes, so the tail loss is bounded by
  the fragment size. Smaller is strictly better; the only cost is wakeups,
  which is negligible because the capture loop already polls at 2 ms.
  The historical "64K ok, 4K loses the tail" split came from
  `pa_simple_read`, which blocks until the full byte count arrives — an I/O
  pattern the current peek/discard loop does not have.
- **Correction (second pass)**: The first attempt kept a 150 ms post-stop
  drain, reasoning that a partial fragment must be completed. Measuring what
  the drain actually yields killed that idea: for a 1000 ms recording, no drain
  captured 1000 ms of audio, a 150 ms drain captured 1153 ms. Nothing is in
  flight at stop (delivered samples == recording duration, five trials), so the
  drain was not recovering anything — it was recording 150 ms of *new* audio,
  padding every utterance and delaying recognition by the same amount. A 20 ms
  drain does complete one final fragment (1047 ms captured) but buys ~20 ms of
  tail for 20 ms of latency, which is a bad trade for speech.
  Third pass: the size is `BufferMs` again, default 20, clamped to 6–1000 ms,
  but registered as a `HiddenOption` so it is settable in the file and absent
  from the settings dialog. `TAIL_DRAIN = 0`; `audio/capture_buffer` logs the
  value applied.
- **Deeper finding (same day)**: the old detector's "hardware burst period" was
  an artefact. Reproducing its exact algorithm (`buffer_detect.cpp:171-290`) on
  this machine's timings yields **64000 bytes = 2.0 s** — the identical number
  recorded for the CX31993 in `b09e19f`. The cause: the first
  `pa_simple_read(200)` after opening a stream blocks ~2014 ms (device wake-up),
  every later read returns instantly. Over 6 s there is exactly **one** 2 s gap,
  not three, so it is a one-time startup delay — but the detector sorts all
  timings and takes the largest gap, which is always that startup delay, on any
  machine. Hence a device-independent constant masquerading as a device
  property, and a 2 s buffer recommended for every device.
  **The 2 s itself is neither hardware nor startup delay.** It is
  PipeWire's default recording fragment:
  `pipewire-pulse.conf`: `pulse.default.frag = 96000/48000 # 2 seconds`,
  which at 16 kHz mono is exactly 64000 bytes — the number the old detector
  "measured". `pa_simple_new(attr=NULL)` asks for the default, so the very
  first `pa_simple_read` cannot return until one default-sized fragment has
  been filled, and the detector timed that wait. Confirmed by varying the
  request: fragsize default → first read 2015 ms; 3200 B (100 ms) → 100 ms;
  640 B (20 ms) → 33 ms. First-read latency tracks the *requested fragment*,
  not the device. Device wake from `SUSPENDED` costs 43–70 ms measured with
  the async API, so cold start was never the issue.

  Two bugs compounded: blocking `pa_simple_read` made the buffer size
  semantically important, and the measurement attributed a buffering constant
  to hardware periodicity. A 2 s blocking read also *masked* the tail loss
  (the read finishes roughly when audio starts flowing), which is why the
  change looked like a fix. The proper fix was to stop blocking: specifying
  the fragment explicitly and peeking removes both the 2 s latency and the
  tail loss.
- **Prevention**:
  - A one-off startup cost must never enter a "periodic behaviour" measurement.
    Check whether a detected pattern repeats within the observation window
    before calling it periodic (here: 1 occurrence in 6 s ≠ a 2 s period).
  - When a heuristic reproduces the same constant on unrelated hardware, the
    constant describes the measuring code, not the hardware. Trace a suspicious
    value back to its configuration source (`pulse.default.frag` here) before
    building any model on top of it.
  - Blocking convenience APIs (`pa_simple_read`) hide the cost they impose: the
    delay showed up as "the device is slow" instead of "we asked for a 2 s
    fragment". Prefer the API whose parameters are explicit, then set them.
  - Before exposing a knob, measure whether the underlying quantity is
    controllable at all. "It felt tunable" is not evidence; here the parameter
    turned out to be an echo of our own request.
  - Distinguish "quantisation loss bounded by X" from "optimum at X". The
    former means pick the smallest X that is safe; only the latter justifies a
    per-device setting.
  - When a past fix is attributed to a device ("this USB mic needs 64K"), check
    whether the blocking I/O primitive was actually the cause before building a
    device model around it.
  - Hiding an option (`fcitx::HiddenOption`) is the right answer for a value
    that exists but should not be a user decision: it stays in the file,
    keeps fcitx5's regeneration, and disappears from the dialog. Removing the
    option entirely would have made the file lossy.
  - When a config file is deleted while fcitx5 runs, fcitx5 recreates it from
    the in-memory configuration on exit — the "factory reset" only works if
    the process is stopped first. Verify a reset with a throwaway `HOME`
    rather than by deleting the live file.
  - Do not add a flush/drain window without measuring what it actually
    recovers. "Drain for one buffer" sounds principled but can be pure padding:
    here it recorded extra audio rather than retrieving lost audio, and the
    only visible symptom would have been "recognition feels slow".
  - Compare *content length to recording length* when validating a capture
    pipeline. A mismatch of exactly the drain duration is the signature of
    padding rather than recovery.

## 19. A watchdog that fires once is not a watchdog (found 2026-10)

- **Mistake/context**: The Rust rewrite added a one-second event-loop timer that
  force-stops a recording after 120 s and clears a stale CapsLock debounce
  counter. The callback returned `true`. It was deployed, documented in
  `docs/architecture.md`, and described in the changelog as a working safety
  net. A temporary log line showed it fired **once**, one second after startup,
  and never again. Both protections had been dead since they were written.
- **Root cause**: fcitx5 creates time sources as `ONESHOT` (it calls
  `sd_event_add_time` without re-arming). Returning `true` from the callback
  keeps the source alive but does **not** schedule it again. Every periodic
  timer inside fcitx5 itself uses `setNextInterval()` (or `setTime()`) plus
  `setOneShot()` from inside the callback. `setNextInterval` alone is also not
  enough when the source has already been disabled — `setEnabled(true)` or
  `setOneShot()` is what re-arms it. The behaviour is not in the type signature
  and the callback's `bool` return reads like "keep going".
- **Why nothing caught it**: a watchdog is by definition silent until the rare
  condition it guards. There is no user-visible symptom, no assertion, and the
  unit tests live on the Rust side, where the timer does not exist. This is the
  same failure class as the silent CapsLock-restore bug (#9): a protection that
  degrades invisibly.
- **Correction**: `shell/periodic.h` provides `rearmPeriodic()` and every
  periodic source must call it; the watchdog was verified end-to-end by setting
  `MaxRecordingSec=8`, starting a recording, and watching the log
  (`recording exceeded 8 s, force-stopping`). The limit is now the configurable
  `MaxRecordingSec` (default 120, `0` disables) instead of a hard-coded
  constant.
- **Prevention**:
  - Any timer that is supposed to repeat must be proven to repeat: add a
    temporary log (or a counter test) and count the fires over a window before
    declaring it works. "I registered a repeating-looking callback" is not
    evidence.
  - When porting to an unfamiliar event loop, copy the *pattern* from that
    project's own periodic timers rather than composing one from the API
    surface; fcitx5's own `setNextInterval` + `setOneShot` pairing is the
    reference.
  - A watchdog or safety net deserves the same verification as the happy path,
    because it is the only code that runs when something else already went
    wrong. Trigger the condition deliberately instead of reasoning that it
    would fire.

## 20. Three silent-failure regressions found by reviewing the rewrite (found 2026-10)

- **Mistake/context**: A review of the finished rewrite found three cases where
  the old C++ implementation did the right thing and the new code quietly did
  not:
  1. **Provider chosen at recognition time, not capture time.** `Engine::start`
     snapshotted the settings, but the job carried only the WAV path and
     credentials; `recognize()` read a process-global provider. Switching
     backend mid-recording (`Ctrl+CapsLock`) sent the audio to the new backend.
     Measured: recording started on `mock`, switched to `qwen` before release,
     and the job produced `Qwen HTTP 400`.
  2. **The cancellation flag was never set.** Every provider checked
     `input.cancel`, and the shell had a "cancelled" error branch, but no code
     path ever stored `true`. Two further layers hid this: `shutdown()` joined
     the worker without cancelling, so `fcitx5 -r` blocked for the rest of a
     60–120 s network timeout; and even after wiring cancellation through,
     libcurl's progress callback — the only place the flag is read during a
     transfer — was never invoked because progress reporting was off. Fixing
     the flag alone changed nothing; the test still took 29.5 s.
  3. **The status line was never cleared.** `setAuxUp()` had no counterpart, so
     `processing audio...` stayed in the input panel after the text landed. The
     old `onDeactivate` called `inputPanel().reset()`.
- **Root cause**: The rewrite reproduced the *interfaces* faithfully but not the
  *call sites*. Reading the new code alone, each piece looks correct; the bug is
  the missing caller. Compounding it, cancellation had two independent dead
  layers, so fixing the obvious one produced no observable change.
- **Why nothing caught it**: All three need a second action after the first
  (switch backend, shut down mid-request, wait for the panel to clear). Unit
  tests exercised each unit in isolation.
- **Correction**: The provider travels inside the job; `shutdown()` sets the
  active cancel flag and the worker drops queued jobs; `easy.progress(true)`
  keeps the callback live; `commitPending` clears the aux line and erases the
  target. Each fix has a regression test that fails when the fix is reverted —
  verified by reverting them (29.5 s shutdown, `error:6` instead of
  `text:hello world`).
- **Prevention**:
  - When rewriting a subsystem, diff the *call sites*, not just the types. Ask
    "who used to call this, and does the new code call it?" for every hook:
    cancel, reset, cleanup, drain, snapshot.
  - A flag that is read but never written is dead code that looks like
    functionality. Grep for `store(`/`Set()` alongside the reads before
    believing a cancellation or shutdown path exists.
  - Verify a cancellation fix by making the test *require* cancellation to
    finish in time (a server that accepts and then never answers). Against a
    refused connection the test passes either way and proves nothing.
  - A test that passes both before and after a fix is not a regression test.
    Temporarily revert the fix and confirm the test fails.

## 21. Parallel tests sharing process-wide state (found 2026-10)

- **Mistake/context**: Two Rust tests called `std::env::set_var("HOME", ...)`
  and a third mutated a global provider selection. Individually they passed;
  the suite failed intermittently (1 in ~4 runs) with unrelated assertions,
  which invited the conclusion "the code is flaky" rather than "the tests
  interfere".
- **Root cause**: the default Rust harness runs tests on parallel threads in
  one process. `HOME`, the active-credentials slot and the provider selection
  are process-wide, so one test's setup silently changed another's inputs.
- **Correction**: `config::test_lock()` serialises tests that touch global
  state, `config::test_config_dir()` replaces the `HOME` mutation with an
  explicit override, and the global provider was deleted entirely once it
  became unnecessary (the job carries its own).
- **Prevention**:
  - Never mutate environment variables in a test. Provide an explicit override
    plus a lock, or take the path as a parameter.
  - Intermittent failures in a suite are usually interference; get the failing
    test to name the shared state before adding sleeps or retries.
  - Run the suite in a loop (8x here) after changing test isolation. A green
    single run proves nothing when the failure rate is 1 in 4.
