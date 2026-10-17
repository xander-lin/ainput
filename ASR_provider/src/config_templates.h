#pragma once

// Template texts for the three seeded config files. Seeding is done by
// readConfigFile() in ainput_config.h: whenever one of these files is found
// missing, it is regenerated from its template (mode 0600). Users can move a
// file away at any time and the next config read recreates the template.
//
// Constraints on the template text: after // comment stripping it must be
// valid JSON (watch trailing commas) and pass its schema in config_schema.h.

namespace ainput {

// Placeholder written into seeded credential files; providers treat it as
// "not configured yet".
inline constexpr const char *kApiKeyPlaceholder = "PASTE_YOUR_KEY_HERE";

inline const char *configJsonTemplate() {
    return R"JSON({
    // ===== 界面 / 按键(全部为默认值,需要调整再解开注释) =====
    // "ui": {
    //     "activation_msec": 300,        // 按住 CapsLock 多少毫秒才触发录音
    //     "notification_timeout": 2000,  // 切换通知显示时长(毫秒)
    //     "debounce_count": 2            // 按键释放去抖采样数
    // },

    // ===== 采集管线(全部为默认值) =====
    // "audio": {
    //     "denoise": "speexdsp",         // "speexdsp" | "deepfilter" | "none"
    //     "lufs_target": -16.0,          // 响度归一化目标
    //     "speex_level": -15,            // speexdsp 抑制强度(dB)
    //     "crest_threshold": 2.4         // 电平门限; 0 = 关闭
    // },

    // 当前 ASR 后端: "qwen" | "doubao" | "zipformer" | "fire_red" | "mock"
    // Ctrl+CapsLock 切换后端时会自动改写这一行
    "provider": "qwen"
}
)JSON";
}

inline const char *qwenJsonTemplate() {
    return R"JSON({
    // 百炼 API Key: https://bailian.console.aliyun.com/?tab=model#/api-key
    // 填好后无需重启,下一次识别即生效。文件权限建议 600。
    "api_key": "PASTE_YOUR_KEY_HERE"

    // 以下均为默认值,需要调整再解开注释(并给上一行补上逗号)
    // "model": "qwen-audio-3.1-asr-flash",
    // "language_hints": ["zh"],
    // "vocabulary": {"专有名词": 3},
    // "keep_dialect": false,
    // "speaker_diarization": false,
    // "timeout_sec": 60
}
)JSON";
}

inline const char *doubaoJsonTemplate() {
    return R"JSON({
    // 火山引擎 API Key 与资源 ID。填好后无需重启,下一次识别即生效。
    "api_key": "PASTE_YOUR_KEY_HERE",
    "resource_id": "volc.seedasr.auc"

    // 以下均为默认值,需要调整再解开注释(并给上一行补上逗号)
    // "model_name": "bigmodel",
    // "enable_itn": true,
    // "enable_punc": true,
    // "timeout_sec": 90
}
)JSON";
}

} // namespace ainput
