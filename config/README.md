# Configuration

Everything user-editable lives in `~/.config/ainput/`. Defaults are built into
the code — files are **optional and sparse**: create only what you want to
change, copy nothing you don't. `//` line comments are allowed in every file.

```
~/.config/ainput/
├── config.json        # provider selection + [ui] + [audio]
├── qwen.json          # cloud provider (api_key, model, ...)
├── doubao.json
├── zipformer.json     # local providers (model_dir, ...)
└── fire_red.json
```

## Self-healing templates

`config.json`, `qwen.json` and `doubao.json` **regenerate whenever they are
found missing**: every config read checks for the file, and a missing one is
recreated as a commented template (mode `0600`). Want the template back? Just
rename the file away — the next recognition or provider switch recreates it.
Existing files are never touched. Local provider files are not seeded; they
work with built-in defaults once the models are installed.

Seeded credential templates carry `"api_key": "PASTE_YOUR_KEY_HERE"`, which
counts as *not configured*: recognition fails with
`Ainput: fill api_key in ~/.config/ainput/qwen.json` until you replace it.
The first recognition on a fresh install therefore walks the user straight
to a self-documenting file — no manual hunting for docs.

There is no `/etc/vinput` layer and no pacnew dance. The auto-detected device
buffer cache moved to `~/.cache/ainput/pa_buffer.json` — it is state, not
configuration, and regenerates itself.

## Boundary (repo side)

- Track `*.example` in Git; never track real `*.json` (`.gitignore` covers `config/*.json`).
- Real API keys and paths belong only in `~/.config/ainput/`.

## `config.json` — global file

| Key | Default | Purpose |
|-----|---------|---------|
| `provider` | (required) | Active ASR backend: `qwen` \| `doubao` \| `zipformer` \| `fire_red` \| `mock`. `Ctrl+CapsLock` switching rewrites this line |
| `ui.activation_msec` | `300` | How long CapsLock must be held |
| `ui.notification_timeout` | `2000` | Switch notification duration (ms) |
| `ui.debounce_count` | `2` | Key-release debounce samples |
| `audio.denoise` | (auto) | `speexdsp` \| `deepfilter` \| `none` |
| `audio.lufs_target` | `-16.0` | Loudness target for gain normalization |
| `audio.speex_level` | `-15` | speexdsp suppression level (dB) |
| `audio.crest_threshold` | `2.4` | Crest-based speech gate; `0` disables |

## `qwen.json` keys

| Key | Default | Purpose |
|-----|---------|---------|
| `api_key` | (required) | Plaintext DashScope API key; keep the file mode 0600 |
| `model` | `qwen-audio-3.1-asr-flash` | ASR model ID, e.g. `qwen-audio-3.0-asr-flash`, `fun-asr-flash-2026-06-15`, or legacy `qwen3-asr-flash` (wire format auto-detected per model family) |
| `endpoint` | `https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation` | Full request URL; switch to the `{WorkspaceId}.cn-beijing.maas.aliyuncs.com` domain when migrating |
| `language_hints` | (auto-detect) | JSON array of language codes, e.g. `["zh","en"]` (max 4) |
| `keep_dialect` | `false` | `qwen-audio-3.1-asr-flash` only: keep dialect wording |
| `speaker_diarization` | `false` | `qwen-audio-3.1-asr-flash` only: label speakers |
| `vocabulary` | (off) | Instant hot words `{"word": weight}`; weight 1–5, or 50 for super hot words |
| `vocabulary_id` | (off) | Precompiled hot-word list ID from the DashScope console |
| `timeout_sec` | `60` | Whole-request curl timeout |

## `doubao.json` keys

| Key | Default | Purpose |
|-----|---------|---------|
| `api_key` | (required) | Plaintext ByteDance API key; keep the file mode 0600 |
| `resource_id` | (required) | ByteDance resource ID, e.g. `volc.seedasr.auc` |
| `model_name` | `bigmodel` | `request.model_name` sent to the submit API |
| `enable_itn` | `true` | Inverse text normalization |
| `enable_punc` | `true` | Punctuation restoration |
| `timeout_sec` | `90` | Whole-recognition budget: submit + result polling share one deadline |

## `zipformer.json` / `fire_red.json` keys

| Key | Default | Purpose |
|-----|---------|---------|
| `model_dir` | `~/.local/share/ainput/models/...` | sherpa-onnx model directory |
| `timeout_sec` | `120` | Local binary wall-clock timeout |
| `bin_path` | `~/.local/share/ainput/sherpa-onnx/bin/...` | sherpa-onnx binary (`sherpa-onnx` for zipformer, `sherpa-onnx-offline` for fire_red) |

## API keys (plaintext)

Cloud API keys are plain `api_key` fields in the vendor JSON — there is no
secret store integration (removed 2026-10 by design). Protect the files:

```bash
chmod 600 ~/.config/ainput/qwen.json ~/.config/ainput/doubao.json
```

## Comments

Every config file supports `//` line comments. They are stripped before
validation and parsing (string-aware, byte offsets preserved), so
`"https://..."` values are never mangled:

```json
{
    // switch back after testing the local models
    "provider": "qwen",
    "audio": { "crest_threshold": 0 }  // keep gating off on this mic
}
```

## Config validation

Every config file is validated on read against a schema
(`ASR_provider/src/config_schema.h`):

- JSON syntax errors are pinpointed with a byte offset and reported in the
  input panel (`Ainput: config.json: ...`), and the file's values are not
  trusted (defaults apply).
- Unknown or misspelled fields get a "did you mean" suggestion
  (`"modelname" (did you mean "model"?)`), type mismatches are named
  (`field "timeout_sec" expects an integer`), and missing required fields are
  reported — these warnings go to the fcitx5 log and are appended to the
  provider switch notification (`Ctrl+CapsLock`) as a `⚠` line.
- Retired fields (e.g. `request_style`, `max_polls`, `num_threads`) are
  flagged as unknown so stale files tell you they do nothing.

Cloud provider configs are re-read on the worker thread before every
recognition, so model/parameter changes take effect on the next recording
without restarting fcitx5. (Breaking changes 2026-10: `advanced.json` was
split per provider; `vinput.json` + `audio.json` merged into `config.json`;
the `/etc/vinput` default layer, the fcitx5 `DefaultProvider` option and the
tuning knobs above were removed.)

## Update Rule

When adding a new runtime config key, update the relevant `.example`, this
README, the user-facing configuration section in `README.md`, and the schema
in `ASR_provider/src/config_schema.h` — schemas live next to the module and
must change together with the readers.
