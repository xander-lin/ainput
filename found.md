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

- **Mistake/context**: `qjsonRemoveTopLevelProperty` (used to strip `api_key`
  after keyring import) initially removed only the key/value span, leaving
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

- **Mistake/context**: `importConfigSecret` decided whether a config file
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
  endpoint) and inject NullSecretStore; local-provider tests create the
  model files the preflight requires.
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
- **Correction**: Manually merged the user's values into
  `~/.config/ainput/audio.json` and trashed the stale `advanced.json`.
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

