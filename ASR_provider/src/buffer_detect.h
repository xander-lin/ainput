#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace ainput {

// 从 ~/.cache/ainput/pa_buffer.json 按 PulseAudio 默认 source 读取或自动检测硬件 burst 大小
// 首次调用时检测并按设备缓存到配置文件, 后续同一设备直接读取
// onStatus: 检测期间回调状态消息(如显示到输入框), 仅实际检测时触发
size_t loadOrDetectBufferBytes(
    std::function<void(const std::string &)> onStatus = nullptr);

// 强制重新检测（忽略缓存）
size_t detectHardwareBurstBytes();

} // namespace ainput
