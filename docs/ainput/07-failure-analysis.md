# Ainput 语音识别无响应及“重启 fcitx5 恢复”问题根因深度分析报告

> **环境与场景说明**：
> - 仅针对云端供应商（阿里千问 `qwen` 与 字节跳动豆包 `doubao`）。
> - 不依赖本地模型，排除网络中断因素。
> - 现象表现为：语音输入偶尔无任何文字上屏或无响应，但重启 fcitx5 后立即恢复正常。

---

## 目录
1. [核心根因 1：`activeRecognition_` 任务调度死锁（最主要元凶）](#1-核心根因-1activerecognition_-任务调度死锁最主要元凶)
2. [核心根因 2：`OutputHandler` 严格 UUID 匹配致使识别结果被静默丢弃](#2-核心根因-2outputhandler-严格-uuid-匹配致使识别结果被静默丢弃)
3. [核心根因 3：按键去抖与状态机标志位错位（无法再次触发录音）](#3-核心根因-3按键去抖与状态机标志位错位无法再次触发录音)
4. [核心根因 4：`AudioCapture` 全局严格票据锁（`ProcessingTurn`）阻塞](#4-核心根因-4audiocapture-全局严格票据锁processingturn阻塞)
5. [次要诱因 5：本地 VAD 与 Crest Factor 门限误判导致请求未发出](#5-次要诱因-5本地-vad-与-crest-factor-门限误判导致请求未发出)
6. [为什么“重启 fcitx5”就能恢复？](#6-为什么重启-fcitx5-就能恢复)
7. [排查与修复建议指南](#7-排查与修复建议指南)

---

## 1. 核心根因 1：`activeRecognition_` 任务调度死锁（最主要元凶）

### 代码位置
- `adapter/src/ainput.cpp`
- `ASR_provider/src/qwen_provider.cpp`
- `ASR_provider/src/doubao_provider.cpp`

### 机制与设计缺陷
为了支持连续录音流水线，Ainput 引入了单任务激活与请求队列机制：
```cpp
// adapter/src/ainput.cpp
struct RecognitionRequest { ... };
static constexpr size_t kMaxPendingRecognitions = 3;
std::deque<RecognitionRequest> recognitionQueue_;
std::optional<RecognitionRequest> activeRecognition_; // 标识当前正在识别的任务
```

任务分发逻辑：
```cpp
void dispatchNextRecognition() {
    // 关键拦截点：只要 activeRecognition_ 包含有效值，后续所有请求直接 return，无法进入 ASR！
    if (activeRecognition_ || recognitionQueue_.empty() || shuttingDown_) return;
    activeRecognition_ = std::move(recognitionQueue_.front());
    ...
    asr_->transcribe(...);
}

void finishRecognition() {
    activeRecognition_.reset(); // 唯一清空 activeRecognition_ 的地方
    dispatchNextRecognition();
}
```

### 为什么会死锁？
`finishRecognition()` 依赖于一个漫长且脆弱的跨线程回调链路：
```
Qwen/Doubao 后台工作线程 (onResult_ / onError_)
  └──> OutputHandler::submit() [跨线程写入 pending_ 并调用 wake()]
        └──> write(wakePipe_[1]) [自管道唤醒 fcitx5 主循环]
              └──> OutputHandler::drainAndCommit() [主线程接收]
                    └──> OutputHandler::dispatchNextCommit()
                          └──> commitPending()
                                └──> completion() 回调
                                      └──> finishRecognition() [重置 activeRecognition_]
```

#### 致命漏洞点：
1. **Cancel 路径丢失回调**：
   在 `qwen_provider.cpp` 和 `doubao_provider.cpp` 中，存在如下逻辑：
   ```cpp
   if (res != CURLE_OK) {
       if (cancel->load()) return; // 致命：若任务被取消，直接 return，没有调用 onR 也没有调用 onE！
       ...
   }
   ```
   在豆包提供商的轮询循环中：
   ```cpp
   if (!waitCancelable(cancel, pollDelay)) return; // 致命：直接退出，未触发任何回调！
   if (cancel->load()) return;                      // 同样未触发回调！
   ```
   只要该任务在某种情况下触发了 cancel（例如新的请求覆盖或内部状态重置），`onResult_` 和 `onError_` 均不会触发。
2. **连锁反应**：
   回调断裂 $\rightarrow$ `finishRecognition()` 永远不被调用 $\rightarrow$ `activeRecognition_` 永远占用 $\rightarrow$ 随后所有的录音在 `dispatchNextRecognition()` 处被直接拦截 $\rightarrow$ 连续录音 3 次后触发 `recognition queue full; try again` 并被彻底丢弃。
3. **缺少看门狗机制**：
   代码中没有任何针对 `activeRecognition_` 的超时重置逻辑（Watchdog）。一旦卡死，状态在进程生命周期内永久残留。

---

## 2. 核心根因 2：`OutputHandler` 严格 UUID 匹配致使识别结果被静默丢弃

### 代码位置
- `adapter/src/output_handler.cpp`

### 关键代码片段
```cpp
void OutputHandler::commitPending(Pending pending, const char *label) {
    auto *ic = instance_->inputContextManager().findByUUID(pending.targetUuid);
    if (!ic) {
        FCITX_INFO() << "Ainput [" << label << "] no focused ic, drop"; // 致命：找不到上下文则直接静默丢弃！
    } else {
        FCITX_INFO() << "Ainput [" << label << "] ic=" << ic
                     << " program=" << ic->program()
                     << " text=\"" << pending.text << "\"";
        if (!pending.text.empty()) ic->commitString(pending.text);
    }
    if (pending.completion) pending.completion();
}
```

### 产生机制
1. 云端识别（Qwen/Doubao）需要经历“音频编码 $\rightarrow$ 网络传输 $\rightarrow$ 云端大模型推理 $\rightarrow$ 结果轮询/接收”，全程通常需要 **0.5 秒 ~ 2 秒**。
2. 录音开始时，Ainput 记录了当时的 `targetUuid = ic->uuid()`。
3. 在 Wayland 环境（Hyprland、Niri 等）下，若用户在说话或等待上屏的 1~2 秒内：
   - 切换了激活窗口；
   - 点击了同一窗口内的不同文本框；
   - 目标应用程序（如 Chrome、VSCode、微信、基于 WebKit/Electron 的应用）在焦点变动时**重新创建了 `text-input-v3` 上下文**。
4. 此时，fcitx5 为该应用分配了全新的 UUID，`findByUUID(pending.targetUuid)` 返回 `nullptr`。
5. **Ainput 直接执行丢弃逻辑（drop），没有任何 fallback 回退到当前焦点输入上下文（`instance_->mostRecentInputContext()`）的代码**。
6. 现象：云端识别完全成功，但文字被直接丢弃，用户体感为“识别不出来”。

---

## 3. 核心根因 3：按键去抖与状态机标志位错位（无法再次触发录音）

### 代码位置
- `adapter/src/ainput.cpp`

### 关键代码片段
```cpp
void onKeyEvent(fcitx::KeyEvent &keyEvent) {
    bool capsLock = (keyEvent.key().sym() == FcitxKey_Caps_Lock);

    if (keyEvent.isRelease()) {
        if (capsLock) {
            if (revertDebounce_ > 0) {
                revertDebounce_--;
                return; // 去抖过滤
            }
            if (active_) {
                if (!isHyprlandSession()) {
                    onDeactivate();
                }
            } else if (switchActive_) {
                switchActive_ = false;
                revertDebounce_ = debounceCount_;
                playSound("deactivate");
                revertCapsLock();
            } else {
                timer_.reset();
            }
            keyEvent.filterAndAccept();
        }
        return;
    }

    // ---- 按下事件 ----
    if (capsLock) {
        if (revertDebounce_ > 0) {
            revertDebounce_--;
            return;
        }
        if (active_ && isHyprlandSession()) {
            onDeactivate(false);
            keyEvent.filterAndAccept();
            return;
        }
        if (timer_ || active_ || switchActive_) return; // 关键拦截：状态标志位残留时拦截所有按键！
        ...
    }
}
```

### 产生机制
1. **uinput 虚拟按键与物理按键竞争**：为了在松开 CapsLock 时恢复原按键状态，代码通过 `/dev/uinput` 注入了合成事件，并使用 `revertDebounce_` 进行事件消除。
2. **事件漏失/时序错乱**：在 compositor 快速焦点切换或快捷键打断时，如果物理 Release 事件被 compositor 拦截，或者 uinput 合成事件未被按预期消费：
   - `revertDebounce_` 可能残留非零值；
   - 或 `active_` 标志位未执行 `onDeactivate()`，依然为 `true`。
3. **后果**：下一次用户长按 CapsLock 时，在 `if (timer_ || active_ || switchActive_) return;` 处被无条件拦截，长按计时器根本无法启动，插件彻底不响应按键。

---

## 4. 核心根因 4：`AudioCapture` 全局严格票据锁（`ProcessingTurn`）阻塞

### 代码位置
- `ASR_provider/src/audio_capture.cpp`

### 关键代码片段
```cpp
namespace {
std::mutex audioProcessingMutex;
std::condition_variable audioProcessingReady;
uint64_t nextProcessingTicket = 0;
uint64_t nextProcessingId = 0;

class ProcessingTurn {
public:
    explicit ProcessingTurn(uint64_t id) : lock_(audioProcessingMutex) {
        audioProcessingReady.wait(lock_, [id] { return id == nextProcessingId; });
    }

    ~ProcessingTurn() {
        ++nextProcessingId;
        lock_.unlock();
        audioProcessingReady.notify_all();
    }
private:
    std::unique_lock<std::mutex> lock_;
};
}
```

### 产生机制
1. `ProcessingTurn` 保证音频处理的绝对 FIFO 顺序。每次录音分配一个单调递增的 `processingTicket`，只有当 `nextProcessingId == ticket` 时才允许进入后续处理。
2. 在 `recordLoop` 中：
   ```cpp
   ProcessingTurn processingTurn(processingTicket);
   // 响度归一化、VAD 检测、降噪（applyDenoise）、WAV 保存均在此作用域内执行
   ```
3. **死锁隐患**：
   - 若某次音频处理线程在 `applyDenoise` 中（如 SpeexDSP 处理异常、DeepFilter 守护进程未响应或管道读写阻塞）卡死或抛出未捕获异常退出；
   - 当前线程无法析构 `ProcessingTurn`，`nextProcessingId` 永远无法递增；
   - 随后所有录音线程在 `ProcessingTurn` 的 `wait` 处**永久休眠**。
4. 表现为：录音看似完成，但音频处理线程永远无法推进到 `onRecorded_`，请求无法发给云端。

---

## 5. 次要诱因 5：本地 VAD 与 Crest Factor 门限误判导致请求未发出

### 代码位置
- `ASR_provider/src/audio_capture.cpp`

### 关键代码片段
```cpp
bool AudioCapture::hasVoice(const std::vector<int16_t> &samples) {
    if (samples.empty()) return false;

    // 峰值因子（Crest Factor）门限
    int32_t peak = 0;
    double sumSq = 0;
    for (auto s : samples) {
        sumSq += (double)s * (double)s;
        int32_t a = s >= 0 ? (int32_t)s : -(int32_t)s;
        if (a > peak) peak = a;
    }
    double rms = std::sqrt(sumSq / samples.size());
    double crestFactor = (double)peak / (rms > 0 ? rms : 1);
    if (crestFactor < crestThreshold_) return false; // 默认 2.4

    // Speex VAD 门限
    ...
    if (voiceCount < kMinVoiceFrames) return false;
    if (ratio < kMinVoiceRatio) return false;

    return true;
}
```

### 产生机制
1. 录音结束后先进行 `normalizeSamples` 放大，若环境底噪较大或麦克风增益异常，导致 `rms` 偏大，`crestFactor` 很容易低于 `2.4`。
2. `hasVoice()` 判定为 `false` 后，`recordLoop()` 执行：
   ```cpp
   else if (isBlank) {
       unlink(wavPath_.c_str());
       if (onStatusText_) onStatusText_("Ainput: no speech detected");
   }
   ```
3. **`onRecorded_` 未被触发**，临时 WAV 文件被直接删除，请求根本未发向阿里或豆包。状态栏短暂提示后静默结束，没有任何文字上屏。

---

## 6. 为什么“重启 fcitx5”就能恢复？

因为上述所有死锁与异常状态**全部驻留在 fcitx5 进程内的堆内存与全局静态变量中**：

| 故障状态 | 驻留位置 | 重启 fcitx5 产生的作用 |
| :--- | :--- | :--- |
| `activeRecognition_` 死锁 | `AinputAddon` 实例成员 | 析构旧 Addon，重新构建，`activeRecognition_` 和请求队列重置为空 |
| `ProcessingTurn` 票据阻塞 | 匿名命名空间全局变量 | 进程重启，重置 `nextProcessingTicket = 0` 和 `nextProcessingId = 0` |
| `revertDebounce_` / `active_` 错位 | `AinputAddon` 实例成员 | 重置按键状态机为初始状态（`active_ = false`, `debounce = 0`） |
| `OutputHandler` 管道与焦点上下文 | `OutputHandler` 实例 | 重建 pipe 管道与 event loop IO 事件监听，重新挂接 Compositor 焦点 |

---

## 7. 排查与修复建议指南

### 建议 1：为 `activeRecognition_` 增加防御性超时与 Cancel 回调补全
1. 在 `qwen_provider.cpp` 与 `doubao_provider.cpp` 中，凡是 `cancel->load()` 导致提前 `return` 的地方，必须确保触发回调（如调用 `onError_("canceled")`），保证 `finishRecognition()` 得到执行。
2. 在 `AinputAddon` 中为当前执行的 recognition 增加超时定时器（如 10 秒）。超时未完成则强制调用 `finishRecognition()` 清空状态并推进队列。

### 建议 2：`OutputHandler` 增加输入上下文 Fallback
修改 `OutputHandler::commitPending`：
```cpp
void OutputHandler::commitPending(Pending pending, const char *label) {
    auto *ic = instance_->inputContextManager().findByUUID(pending.targetUuid);
    if (!ic) {
        // Fallback: 如果原 UUID 失效，尝试提交给当前最新的激活输入上下文
        ic = instance_->mostRecentInputContext();
        FCITX_INFO() << "Ainput [" << label << "] targetUuid lost, fallback to mostRecentInputContext: " << ic;
    }
    if (ic && !pending.text.empty()) {
        ic->commitString(pending.text);
    }
    if (pending.completion) pending.completion();
}
```

### 建议 3：重构按键状态机，增加按键超时复位
当按下 CapsLock 启动录音后，若超过最大录音时长（如 60 秒）或检测到异常失焦，自动触发 `onDeactivate()`，强制复位 `active_ = false` 与 `revertDebounce_ = 0`。
