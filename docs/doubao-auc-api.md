# 豆包录音文件识别 API

> **Ainput 的实际用法**：下面的 `audio.url` 是厂商文档中的公网链接方式；
> Ainput 不上传公网 URL，而是把本地 WAV 以 base64 内联在
> `audio.data` 字段中提交（`format: "wav"`）。`enable_itn` / `enable_punc`
> 由 `doubao.json` 控制，默认都为 `true`（与厂商“默认 false”不同），
> 并且每次请求都会显式发送。

大模型录音文件识别服务的处理流程分为提交任务和查询结果两个阶段：
- **任务提交**：提交音频链接，获取服务端分配的任务 ID
- **结果查询**：通过任务 ID 查询转写结果

---

## 提交任务

**接口地址**：`https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit`

**请求方式**：HTTP POST。请求和应答均在 HTTP BODY 中传输 JSON 格式字串。

### Header

**新版本控制台**：

| Key | 说明 | 示例 |
|-----|------|------|
| X-Api-Key | 火山引擎控制台获取的 APP Key | |
| X-Api-Resource-Id | 资源 ID | volc.bigasr.auc (1.0) / volc.seedasr.auc (2.0) |
| X-Api-Request-Id | 任务 ID，推荐 UUID | |
| X-Api-Sequence | 固定值 -1 | -1 |

### 请求字段

| 字段 | 说明 | 必填 |
|------|------|------|
| audio.url | 音频链接（Ainput 改用 `audio.data` 内联 base64，二选一） | ✓ |
| audio.format | 音频容器格式：raw/wav/mp3/ogg | ✓ |
| audio.codec | 音频编码：raw/opus，默认 raw(pcm) | |
| audio.rate | 采样率，默认 16000 | |
| audio.bits | 采样位数，默认 16 | |
| audio.channel | 声道数：1(mono)/2(stereo)，默认 1 | |
| request.model_name | 模型名称，目前只有 bigmodel | ✓ |
| request.enable_itn | 启用 ITN 文本规范化，默认 true | |
| request.enable_punc | 启用标点，默认 false | |
| request.enable_ddc | 启用语义顺滑，默认 false | |
| request.show_utterances | 输出分句/分词信息 | |

```json
{
    "audio": {
        "format": "wav",
        "url": "http://xxx.com/sample.wav"
    },
    "request": {
        "model_name": "bigmodel",
        "enable_itn": true,
        "enable_punc": true
    }
}
```

### 应答 Header

| Key | 说明 |
|-----|------|
| X-Api-Status-Code | 20000000 表示提交成功 |
| X-Api-Message | OK 表示成功 |

Response body 为空。

---

## 查询结果

**接口地址**：`https://openspeech.bytedance.com/api/v3/auc/bigmodel/query`

**请求方式**：HTTP POST。Header 同上（不需要 X-Api-Sequence）。Body 为 `{}`。

### 应答 Body

```json
{
    "audio_info": {"duration": 3696},
    "result": {
        "text": "这是字节跳动，今日头条母公司。",
        "utterances": [
            {
                "definite": true,
                "end_time": 1705,
                "start_time": 0,
                "text": "这是字节跳动，",
                "words": [
                    {"text": "这", "start_time": 740, "end_time": 860},
                    {"text": "是", "start_time": 860, "end_time": 1020}
                ]
            }
        ]
    }
}
```

---

## 错误码

| 错误码 | 含义 |
|--------|------|
| 20000000 | 成功 |
| 20000001 | 正在处理中 |
| 20000002 | 任务在队列中 |
| 20000003 | 静音音频 |
| 45000001 | 请求参数无效 |
| 45000002 | 空音频 |
| 45000131 | 提交速度超限 |
| 45000132 | 音频大小超限（<512M） |
| 45000151 | 音频格式不正确 |
| 55000031 | 服务器繁忙 |
