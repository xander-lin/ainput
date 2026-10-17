# Performance tools

These are `cargo run --release --example <name>` binaries, not a benchmark
harness: the addon is I/O bound enough that a statistical framework adds more
ceremony than signal. Each one answers one question.

| Example | Question | Needs |
|---|---|---|
| `pipeline` | How long does each DSP stage take, per audio length? | nothing |
| `stage_breakdown` | For real clips, what fraction of the user-visible wait is preprocess vs network? | `<dir>` of WAVs, an API key |
| `vad_passes` | How much of the DSP time is the three speexdsp VAD passes? | a WAV |
| `qwen_latency` | End-to-end qwen latency, with the addon's real preprocessing | a WAV, an API key |
| `doubao_latency` | Same for doubao (submit + poll) | a WAV, an API key |
| `http_reuse` | Transport-only cost of a cold connection vs a kept-alive one | network |
| `reuse_gain` | The same A/B on real recognitions (what the user feels) | `<dir>`, an API key |
| `capture_cost` | CPU cost of the capture poll loop per second of audio | nothing |

Build them with `--features diagnostics` to get the `qwen_timing` stage log
lines (`encode_ms` / `network_ms` / `parse_ms`).

## Findings (2026-10, Arch, PipeWire, wired network)

| Stage | 5 s utterance | 10 s utterance |
|---|---|---|
| normalize | 0.9 ms | 1.8 ms |
| gate (crest + VAD) | 4.3 ms | 8.6 ms |
| speexdsp denoise | 4.2 ms | 8.5 ms |
| trim (VAD) | 4.3 ms | 8.5 ms |
| **preprocess total** | **~14 ms** | **~28 ms** |
| encode (base64 + JSON) | 0.2 ms | 0.4 ms |

Against real recordings (8 clips, 3 s each):

- preprocess: **16 ms** (2.0% of the wait)
- recognition: **803 ms** (98.0%)

So the addon's own compute is not the bottleneck. What is:

1. **TLS handshake per request** — libcurl keeps its connection cache on the
   easy handle, and a fresh `Easy` per request reconnects every time.
   Measured on real recognitions: **925 ms -> 517 ms, 44% saved**.
2. **Server-side inference variance** — the same clip on the same day ranged
   950-4300 ms. Nothing client-side changes that.
3. **doubao's fixed 300 ms poll delay** — the first query was made only after a
   300 ms sleep; now it is immediate with exponential backoff
   (**1765 ms -> 1600 ms** median).

Not worth changing: the capture poll loop (0.16 ms of CPU per second of
audio), base64 (0.4 ms per 10 s), and the WAV write (<1 ms).
