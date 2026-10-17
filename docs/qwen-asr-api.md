# Qwen ASR API（阿里云百炼 DashScope）

Ainput 的 `qwen` provider 通过 DashScope multimodal 同步接口调用阿里云 ASR
模型，音频以 base64 Data URL 内联传输，无需公网 URL。模型 ID、端点与识别
选项全部由 `~/.config/ainput/qwen.json` 驱动，**每次识别前重读配置**，更换
模型不需要重启 fcitx5，也不需要重新编译。

## 模型选型（2026-10 核实）

| 模型 ID | 代际 | 限制 | 说明 |
|---------|------|------|------|
| `qwen-audio-3.1-asr-flash` | 当前旗舰（默认） | ≤5 分钟 / ≤2GB | 热词、上下文、方言保留、说话人分离、按 Token 计费 |
| `qwen-audio-3.0-asr-flash` | 当前 | ≤5 分钟 / ≤10MB | 上一版 |
| `fun-asr-flash-2026-06-15` | 当前 | ≤5 分钟 / ≤2GB | 官方对 `qwen3-asr-flash` 系列的替代，按秒计费 |
| `qwen3-asr-flash` | 旧（逐步下线中） | ≤5 分钟 / ≤10MB | 2026-07 起其最新快照已下线，仍可用但不要再新建业务 |

`fun-asr-flash` 是同步短音频接口；长音频的 `*-filetrans` 与异步 `fun-asr`
是提交+轮询模式，不属于语音输入场景，Ainput 不使用。

## 配置

文件：`~/.config/ainput/qwen.json`（缺失字段用默认值）

```json
{
    "api_key": "sk-xxx",
    "model": "qwen-audio-3.1-asr-flash",
    "endpoint": "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation",
    "language_hints": ["zh"],
    "keep_dialect": false,
    "speaker_diarization": false,
    "vocabulary": {"Ainput": 5},
    "vocabulary_id": "",
    "timeout_sec": 60
}
```

| 字段 | 默认 | 说明 |
|------|------|------|
| `api_key` | 必填 | 明文百炼 API Key（建议文件权限 600；https://bailian.console.aliyun.com/?tab=model#/api-key） |
| `model` | `qwen-audio-3.1-asr-flash` | 模型 ID，见上表 |
| `endpoint` | 北京 DashScope 域名 | 可切换到 `{WorkspaceId}.cn-beijing.maas.aliyuncs.com` 新域名 |
| `language_hints` | 自动检测 | 语言代码数组（最多 4 个），如 `["zh","en"]` |
| `keep_dialect` | `false` | 仅 3.1：保留方言表达（默认转普通话文本） |
| `speaker_diarization` | `false` | 仅 3.1：说话人分离 |
| `vocabulary` | 不启用 | 即时热词 `{"词":权重}`，权重 1–5 或 50（超级热词） |
| `vocabulary_id` | 不启用 | 控制台预编译热词表 ID |
| `timeout_sec` | `60` | 整个请求的 curl 超时 |

新格式中 `format`/`sample_rate` 描述 Ainput 自身录音管线（16 kHz 单声道
WAV），固定发送，不可配置。旧格式（`qwen3-asr-flash` 系列）不发送这两个
字段。配置键 `speaker_diarization` 在新格式请求中的线字段名是
`speaker_diarization_enabled`。

## 请求格式

请求格式按模型家族自动识别（2026-10 起不再可配置）：模型名以
`qwen3-asr-flash` / `qwen2-audio` / `qwen2-asr` 开头 → 旧格式；
其余（含未知新模型）→ 新格式。

### 新格式（qwen-audio-3.x / fun-asr-flash，当前默认）

```json
{
    "model": "qwen-audio-3.1-asr-flash",
    "input": {
        "messages": [{
            "role": "user",
            "content": [{
                "type": "input_audio",
                "input_audio": {"data": "data:audio/wav;base64,{BASE64_DATA}"}
            }]
        }]
    },
    "parameters": {
        "format": "wav",
        "sample_rate": "16000",
        "language_hints": ["zh"],
        "vocabulary": {"Ainput": 5}
    }
}
```

响应（无 `choices` 字段）：

```json
{
    "output": {
        "sentence": {"sentence_id": 1, "sentence_end": true, "text": "识别文本", "words": []},
        "text": "识别文本"
    },
    "usage": {"duration": 4},
    "request_id": "..."
}
```

文本提取路径：`output.text`，回退 `output.sentence.text`。

### 旧格式（qwen3-asr-flash 系列）

```json
{
    "model": "qwen3-asr-flash",
    "input": {
        "messages": [{"content": [{"audio": "data:audio/wav;base64,{BASE64_DATA}"}], "role": "user"}]
    },
    "parameters": {"asr_options": {"enable_itn": false}}
}
```

响应文本路径：`output.choices[0].message.content[0].text`。

## 实现说明

- 端点：`POST {endpoint}`，Header `Authorization: Bearer {api_key}`，
  `Content-Type: application/json`；非流式（`X-DashScope-SSE` 不设置）。
- 响应解析在 `ASR_provider/src/qwen_json.h`（`qwenExtractText` /
  `qwenExtractErrorDetail`），对新旧两种 schema 容错，单测见
  `tests/test_qwen_json.cpp`。
- HTTP 非 200 时提取响应体中的 `code`/`message` 附加到错误信息。
- 诊断日志 `request_started`/`request_result` 事件带 `model` 字段。
- 流式（SSE）输出可作后续优化：长音频时 3.1 支持分句流式返回。

## 限制

- QPS、音频大小/时长限制因模型而异（见上表）；base64 会增大体积 ~33%。
- 热词、`keep_dialect`、`speaker_diarization` 仅对声明支持的模型生效；
  对不支持的模型发送可能报参数错误，此时从配置中移除对应字段即可。
- 地域：北京（默认域名）/新加坡 `dashscope-intl.aliyuncs.com`（改
  `endpoint`）。

## 迁移记录

- 2026-10：默认模型从 `qwen3-asr-flash` 切换为 `qwen-audio-3.1-asr-flash`，
  请求/响应格式按模型自动适配；旧模型仍可通过配置 `model` 字段使用。
  背景：`qwen3-asr-flash-2026-02-10` 快照已于 2026-07 下线（公告 118434），
  官方替代指向 Fun-ASR / Qwen-Audio-3.x 系列。
