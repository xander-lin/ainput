# History

This file is an archive, not a specification. Everything below describes how
the project looked at a point in time; some of it has since been replaced.
For current behavior read `README.md`, `docs/configuration.md`, and
`docs/architecture.md`.

- **Part 1** is a 2026-09 failure analysis. Its header lists which findings
  still apply to the current code.
- **Part 2** is the chronological engineering log that used to live in
  `FINDINGS.md`. Test counts such as "4/4 passed" are from that moment.

Do not implement against this file without checking the source first.

**Update 2026-10 (0.2 rewrite).** Part 1's findings were addressed by the
Rust rewrite; the analysis is kept verbatim below as the record of what was
wrong and why.

| Finding | Outcome in 0.2 |
|---------|----------------|
| 1. `activeRecognition_` deadlock | Gone. One recognition worker; the pending counter is reserved up front and released on every path, including cancellation. A cancelled request returns `Timeout("cancelled")` instead of returning silently. |
| 2. Results dropped on UUID mismatch | Gone. The commit falls back to the currently focused input context and logs it. |
| 3. Key state stuck | Mitigated. A recording older than 120 s is force-stopped, and a debounce counter idle for 10 s is cleared. |
| 4. `ProcessingTurn` lock | Gone. Each recording has its own thread and buffer; there is no global ticket. |
| 5. Crest/VAD gate | Unchanged behaviour, now adjustable in fcitx5-configtool. |
| 7. Suggested fixes | Not applied as written; superseded by the rewrite. |

---

## Part 1 — 2026-09 failure analysis


> **报告日期**：2026-09-07。这是一份分析报告，不是已实施修复清单。
>
> **2026-10-17 状态核对**（以当前源码为准）：
>
> | 条目 | 当前状态 |
> |------|----------|
> | 1 `activeRecognition_` 调度 | **仍适用**。`dispatchNextRecognition()` 仍以 `activeRecognition_` 为闸门；provider 的取消路径仍直接 `return`、不触发完成回调；仍没有看门狗或超时复位。 |
> | 2 `OutputHandler` UUID 丢弃 | **仍适用**。`commitPending()` 找不到原 UUID 时仍直接丢弃，没有回退到当前焦点上下文。 |
> | 3 按键状态机 | **部分过时**。`isHyprlandSession()` 分支已在 `dfd0d55`（2026-09）删除，所有合成器走同一路径；`revertDebounce_` / `active_` 残留导致后续按键被拦的通用风险仍然存在。第 3 节代码摘录已更新为当前实现。 |
> | 4 `ProcessingTurn` 票据锁 | **仍适用**。代码未变。 |
> | 5 Crest/VAD 门限 | **仍适用**。`crest_threshold` 默认 2.4，可配置为 0 关闭。 |
> | 第 7 节修复建议 | **均未落地**（截至 2026-10-17）。 |
>
> 下面的代码片段是报告撰写时的证据；除第 3 节外未逐行同步。修改代码前请直接查看当前源码。

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

> 说明：2026-09 的 `dfd0d55` 删除了所有合成器专属分支（`isHyprlandSession()` 已不存在），下面是与当前实现一致的摘录。第 3 条描述的是通用状态机风险，不再是 Hyprland 专属逻辑。

### 关键代码片段
```cpp
void onKeyEvent(fcitx::KeyEvent &keyEvent) {
    bool capsLock = (keyEvent.key().sym() == FcitxKey_Caps_Lock);

    if (keyEvent.isRelease()) {
        if (capsLock) {
            if (revertDebounce_ > 0) {
                revertDebounce_--;   // 吞掉 uinput 合成事件
                return;
            }
            if (active_) {
                onDeactivate();
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
        if (timer_ || active_ || switchActive_) return; // 关键拦截
        ...
    }
}
```

### 产生机制
1. **uinput 虚拟按键与物理按键竞争**：为了在松开 CapsLock 时恢复原按键状态，代码通过 `/dev/uinput` 注入了合成事件，并使用 `revertDebounce_` 进行事件消除。
2. **事件漏失/时序错乱**：在 compositor 快速焦点切换或快捷键打断时，如果物理 Release 事件被 compositor 拦截，或者 uinput 合成事件未被按预期消费：
   - `revertDebounce_` 可能残留非零值，后续真实按键被当成合成事件吞掉；
   - 或 `active_` 标志位未执行 `onDeactivate()`，依然为 `true`。
3. **后果**：下一次用户长按 CapsLock 时，在 `if (timer_ || active_ || switchActive_) return;` 处被无条件拦截，长按计时器根本无法启动，插件彻底不响应按键。代码没有按键状态超时复位，只能重启 fcitx5。

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

---

## Part 2 — Engineering log

> 下面是原来的 `FINDINGS.md`，按写作时间累积，不是当前设计文档，也不是
> 安装/配置指南。条目里早期的“当前方案”后来可能已被替换。以下内容**已经
> 废弃**，不要据此实现：
>
> - `/etc/vinput` 默认配置层与 pacman `backup=`/`.pacnew` 流程（2026-10 移除，现在只用 `~/.config/ainput/`）
> - `advanced.json`、`vinput.json`、`audio.json` 等旧配置文件（已合并进 `config.json`）
> - systemd 懒启动 `sherpa-onnx-offline-websocket-server` + `ws_client.h` / `service_helper.h`（已删除，改为每次录音 `posix_spawn` 一次）
> - Secret Service / keyring 存储（已删除，API key 为明文 JSON 字段）
> - Hyprland 专属的 CapsLock 交互与桌面环境自动检测（已删除，所有合成器走同一路径）
> - `pa_buffer.json` 位于 `~/.config/ainput/`（现已移到 `~/.cache/ainput/`）
>
> 文中“4/4 通过”“3/3 通过”等是当时的测试数量，不是当前结果。当前设计见
> `README.md`、`docs/configuration.md`、`docs/architecture.md`；经验教训见
> `found.md`。

## 2026-06-16 (续)

### 独立 uinput CapsLock 测试工具

#### 动机
- 用户要求不要每次都重建/安装整个 `adapter` 插件，只验证 `uinput` 路径本身。

#### 已新增
- `tools/test_uinput_caps.cpp`：独立创建 `/dev/uinput` 虚拟键盘，只发送 CapsLock press/release。
- `tools/meson.build`：新增 `test_uinput_caps` 可执行目标。

#### 用法
- 构建：`meson setup build --reconfigure && ninja -C build tools/test_uinput_caps`
- 运行：`build/tools/test_uinput_caps --taps 1 --settle-ms 800`

#### 当前观测
- 在 Hyprland 会话中运行后，Hyprland 里新增的 `ainput-uinput-caps-test` 键盘存在，但 `hyprctl devices -j` 显示所有键盘 `capsLock=false`。
- 这说明独立 `uinput` 虚拟键盘已创建并发出事件，但当前事件序列没有把 Hyprland 的 CapsLock 锁定状态置为 on。

#### 后续方向
- 优先在 `tools/test_uinput_caps.cpp` 上继续隔离实验，调整事件序列后再回写到 `adapter/src/ainput.cpp`。
- 不要回到整插件安装验证流程，避免把构建/安装成本混入单变量测试。
- 更长期的正确方向是调用 Hyprland API 做真正意义上的 compositor 内部控制：在 Hyprland 内部读写 CapsLock/XKB lock state，而不是通过另一个虚拟键盘间接影响。
- 当前阶段不实现 Hyprland 内部控制；本次以交互调整方案收尾。

### Hyprland CapsLock 停止录音交互调整

#### 问题
- Hyprland 下 CapsLock lock state 由 compositor/XKB 维护，`uinput` 另建虚拟键盘只能影响 LED，不能可靠恢复实际大小写锁定状态。
- 用户不采用键盘代理、Hyprland 插件、禁用 CapsLock、换触发键等方案。

#### 方案
- `adapter/src/ainput.cpp` 新增 `AinputAddon::isHyprlandSession()`，通过 `HYPRLAND_INSTANCE_SIGNATURE` 判断 Hyprland。
- 非 Hyprland 行为保持不变：长按 CapsLock 激活录音，松开停止录音并通过 `revertCapsLock()` 恢复 CapsLock。
- Hyprland 行为改为：长按 CapsLock 激活录音，第一次松开不停止录音；录音中再次按下 CapsLock 才停止录音，并调用 `onDeactivate(false)`，不再尝试恢复 CapsLock。

#### 验证
- `ninja -C build`：成功。
- `meson test -C build`：4/4 通过。

### Hyprland CapsLock 还原尝试（已被交互调整取代）

#### 现象
- 同一套 uinput CapsLock 还原逻辑在 niri 上可正常工作，但在 Hyprland 上 CapsLock 不会恢复。

#### 根因
- `adapter/src/ainput.cpp` 的 `AinputAddon::revertCapsLock()` 原先把虚拟 CapsLock press 与 release 写入同一个 `SYN_REPORT` 同步帧。
- niri 能接受这种事件序列并切换锁定状态；Hyprland/libinput 路径下同帧 press+release 不可靠，可能被折叠或不触发 CapsLock 锁定状态第二次切换。

#### 尝试
- `AinputAddon::revertCapsLock()` 改为：发送 `KEY_CAPSLOCK press` → `SYN_REPORT` → `KEY_CAPSLOCK release` → `SYN_REPORT`。
- 修改范围仅限 `adapter/src/ainput.cpp`，不改变按键状态机、录音生命周期、桌面策略或输出提交逻辑。

#### 后续结论
- 该尝试在 Hyprland 下仍不能可靠恢复实际 CapsLock lock state，最终采用上方“再按一次停止录音”的 Hyprland 专用交互调整。

#### 验证
- `ninja -C build`：成功。
- `meson test -C build`：4/4 通过。

### 桌面环境自动检测（替代 output.json 配置）

#### 动机
原先 `OutputHandler` 构造函数从 `~/.config/ainput/output.json` 的 `desktop` 字段读取桌面环境（`niri`/`hyprland`/`none`），需要用户手动配置。实际上 compositor 可以通过环境变量自动检测，无需用户干预。

#### 方案
- `DesktopStrategy` 新增 `static autoDetect()` 方法，通过环境变量检测当前 compositor：
  1. `HYPRLAND_INSTANCE_SIGNATURE` → HyprlandStrategy
  2. `NIRI_SOCKET` → NiriStrategy
  3. `XDG_CURRENT_DESKTOP` / `XDG_SESSION_DESKTOP` 回退检测
  4. 都不匹配 → NoopStrategy
- `OutputHandler` 构造函数使用 `autoDetect()` 代替 `readDesktopConfig()`
- `captureCurrentWindow()` 每次录音激活时重新调用 `autoDetect()`，确保检测最新状态
- 删除 `readDesktopConfig()` 函数，不再读取 `output.json`
- 删除 `config/output.json.example`，`PKGBUILD` 的 `backup` 数组移除 `output.json`

#### 各文件变化
- `adapter/src/desktop_strategy.h`：新增 `autoDetect()` 声明
- `adapter/src/desktop_strategy.cpp`：实现 `autoDetect()`，添加 `<cstdlib>` 头文件
- `adapter/src/output_handler.cpp`：删除 `readDesktopConfig()`，构造函数改用 `autoDetect()`，`captureCurrentWindow()` 每次重新检测
- `config/output.json.example`：删除
- `PKGBUILD`：`backup` 移除 `output.json`
- `README.md`、`config/README.md`：移除 `output.json` 相关说明

#### 性能影响
- `getenv()` 调用开销可忽略（纳秒级），在录音激活时执行不影响音频流水线
- 每次 `captureCurrentWindow()` 重新创建策略对象（`make_unique`），开销极小

#### 验证
- `ninja -C build`：成功
- `meson test -C build`：4/4 通过

## 2026-06-16

### 安装/更新配置文件策略

- 用户要求语义：配置文件不存在时安装，存在时更新不能覆盖用户配置。
- Arch/pacman 正确实现路径不是写入 `~/.config/ainput/`：包安装脚本以 root 运行，无法可靠判断目标普通用户 home。
- 当前方案：`PKGBUILD` 把 `config/*.json.example` 安装为 `/etc/vinput/*.json`，并在 `backup=(...)` 中声明这些文件；pacman 首次安装会创建它们，升级时会保留本地修改并按 pacman 规则生成 `.pacnew`。
- 运行时读取顺序：`ASR_provider/src/ainput_config.h` 的 `readConfigFile()` 先读 `~/.config/ainput/<name>`；用户文件缺失且 `/etc/vinput/<name>` 存在时，会自动复制到 `~/.config/ainput/<name>` 后读取；已有用户文件不覆盖。原先 `adapter/src/output_handler.cpp` 的 `output.json` 读取也走同一规则；桌面策略移除后不再读取该文件。
- `PKGBUILD` 构建参数使用 `meson setup ... --buildtype=plain -Dcpp_args='-O2 -march=native'`，按当前机器启用 O2 和 native CPU 优化。
- `PKGBUILD.install` 中本地模型需自行下载的提示使用 ANSI 红字输出。

## 2026-06-04

### 录音尾部丢失：最终修复（动态缓冲区 + 自动检测）

#### 历史问题
1. 原始 4K buffer → 尾部丢失
2. 64K (65536) buffer → 跨双周期阻塞, 丢数据
3. 16K + drain → 可行但延迟大

#### 最终方案
- **动态缓冲区**: 首次使用自动检测硬件 burst 大小, 缓存到 `~/.config/ainput/pa_buffer.json`
- 检测逻辑 (`buffer_detect.cpp`): 256 字节小 buffer 录 6s, 聚类读写耗时识别 fill/burst 周期
- 每次 `pa_simple_read(buf, N)` 中的 N = 检测到的 burst 字节数, 恰好对齐单周期
- 无 drain, 无额外的延迟

#### 检测原理
```

### 桌面环境策略解耦（refactor: 2026-06-04）

#### 动机
niri 窗口切换逻辑硬编码在 `OutputHandler` 中，无法支持其他桌面环境（Hyprland、GNOME 等）。

#### 方案
- 新增 `DesktopStrategy` 抽象接口（`desktop_strategy.h/.cpp`）
  - `getFocusedWindowId()` / `focusWindow(id)` / `supportsSwitching()` / `name()`
  - `NoopStrategy`: 空实现，`supportsSwitching()=false`，回退到直接提交
  - `NiriStrategy`: `niri msg focused-window` / `niri msg action focus-window --id`
  - `HyprlandStrategy`: `hyprctl activewindow -j` / `hyprctl dispatch focuswindow address:`
- `OutputHandler` 通过 `DesktopStrategy::create(desktop)` 工厂选择策略
- 配置文件: `~/.config/ainput/output.json` — `{"desktop": "niri"|"hyprland"|"gnome"|"none"}`
- 默认值: `"none"`（不执行窗口切换）

#### GNOME 支持情况
- GNOME/Mutter on Wayland **不支持**程序化窗口焦点切换（无官方 IPC）
- `gnome` 值被接受但等价于 `none`（NoopStrategy），日志中会提示

#### 各文件变化
- `adapter/src/desktop_strategy.h/.cpp`：新增，~70 行 + ~70 行
- `adapter/src/output_handler.h`：移除静态 niri 方法，新增 `desktop_` 成员
- `adapter/src/output_handler.cpp`：构造函数读取 `output.json` 并实例化策略，所有 niri 调用改为 `desktop_->` 多态调用
- `adapter/src/ainput.cpp`：无变化
- `adapter/src/meson.build`：添加 `desktop_strategy.cpp`


#### 配置文件
- 路径: `~/.config/ainput/pa_buffer.json`
- 格式: `{"buffer_bytes": 64000}`
- 未配置时自动检测, 检测后在输入框显示 "Detecting hardware buffer period..."
- 连续流声卡(无 burst 模式) fallback 到 16384

#### 代码修改
- 新增: `ASR_provider/src/buffer_detect.h`, `buffer_detect.cpp`
- `asr_provider.h`: 新增 `AsrStatusTextCallback` + `onStatusText_`
- 三个 provider: 动态 `std::vector<uint8_t> buf(bufferBytes_)`, 检测前显示状态
- `ainput.cpp`: PendingCommit 加 `isStatus` 字段, pipe reader 用 `commitString` 直接上屏

#### pa_simple_read 关键特性
- `pa_simple_read(buf, N)` 成功时**必定返回恰好 N 字节**, 数据不足就死等
- 不同于 POSIX `read()` 的「有多少返回多少」
- 缓冲区 > burst → 跨周期死等; 缓冲区 = burst → 完美对齐; 缓冲区 < burst → 多读但无 drain
- `-s`: 扫频测试 (1/2/3/4/5/8s)
- `<秒数> <wav>`: 固定时长测试
- `-i <wav>`: 交互模式 (Ctrl+C 模拟 CapsLock 松键)

### CX31993 硬件占空比（录音尾部丢失根因）

#### 发现过程
- `tools/record_timing.cpp` 测试发现无论 buffer 大小（128/1024/4096），`pa_simple_read` 始终呈现 **~2s 阻塞 + ~2s burst（0ms reads）** 的 50% 占空比
- 48kHz 原生采样率也如此，parecord 同效
- 即使在录音同时另开一条常驻流读 48kHz，占空比不变
- **结论**: CX31993 芯片硬件内部以 ~2s 为周期填充缓冲 → PulseAudio 瞬间取走（burst）→ 下一轮填充（阻塞 2s），任何持续读取都无法打破

#### 后果
- 4s 录音: 前 2s burst 被捕获，后 2s 落在下一轮填充期（丢失）
- 这就是「后半段丢失」的根因

#### 解决方案
- **Drain**: `stop()` 后不立即停止录音，继续读直到检测到 2 次长阻塞（＞500ms）或 4s 超时
  - 这确保了跨过一个完整 burst 周期，捕获尾部音频
  - 代价: 松键后 1-4s 延迟出结果
- **去掉 keep-alive**: keep-alive 流不能阻止 CX31993 的硬件占空比，是之前的误判

#### 实测数据
```
4096 buf: 63 次, eff=80%, max=2036ms  ┐
1024 buf: 251 次, eff=80%, max=2005ms ─ 完全一致
128  buf: 2001 次, eff=80%, max=2005ms ┘
```

### 豆包录音文件识别 API（非流式）

#### API 流程
- **提交任务**: `POST /api/v3/auc/bigmodel/submit` → 返回 `X-Api-Status-Code: 20000000`
- **轮询结果**: `POST /api/v3/auc/bigmodel/query` → 直到 `20000000`（成功）、`20000003`（静音）或超时
- **比流式更适合语音输入**: 先录完再识别，结果一次返回，不需要处理 preedit 中间态

#### audio.data 字段（内联音频）
- 官方文档只有 `audio.url`，但 **`audio.data`（base64 编码）实测可用**
- `format:"wav"` + `data:"<base64>"` 提交成功，服务端正常解码
- 注意：`format:"raw"` 会返回 `45000151`（音频格式不正确），服务端无法解析裸 PCM
- base64 大小：4 秒 16kHz mono WAV ≈ 174KB，在 curl 的默认限制内

#### Header 解析陷阱
- 服务端返回的 header 名是**小写** `x-api-status-code`（不是 `X-Api-Status-Code`）
- `access-control-expose-headers` 头里**恰好包含字串** `X-Api-Status-Code`
- 用 `find("X-Api-Status-Code")` 会命中 `access-control-expose-headers` 而非真正的 status header
- **修复**: 匹配 `\n<name>:` 模式，确保匹配的是独立的 header 行而非其他 header 的值

#### 错误码
| 错误码 | 含义 | 处理 |
|--------|------|------|
| 20000000 | 成功 | 提取 `result.text` |
| 20000001/2 | 处理中 | 继续轮询 |
| 20000003 | 静音音频 | 返回空文本，不报错 |
| 45000151 | 音频格式不正确 | format/编码不匹配 |
| 45000001 | 参数无效 | 检查必填字段 |

#### 轮询策略
- 间隔 800ms，最长 60 秒（75 次）
- 每次 `curl_easy_init/cleanup` 新建连接，避免复用导致的状态问题
- HTTP 层超时设为 15s，curl 错误时 continue 重试

#### 配置
- 文件: `~/.config/ainput/doubao.json`
- 格式: `{"api_key":"xxx", "resource_id":"volc.seedasr.auc"}`
- 资源 ID: `volc.bigasr.auc` (1.0 小时版) / `volc.seedasr.auc` (2.0)

#### 实现注意事项
- **不需要 keep-alive 流**也可以（zipformer 的 keep-alive 是为了防 PipeWire 挂起），共用 zipformer 的常驻流模式
- `curl_global_init(CURL_GLOBAL_ALL)` 必须在任何 curl 操作前调用（静态初始化器）
- UUID 用 `getrandom()` 而非 `rand()`，避免冲突和时间种子质量差
- 响度归一化（ebur128 → -16 LUFS）对云 ASR 同样有效，避免音量过低导致 20000003

### PipeWire/PA 录音（keep-alive 被证实为误判，见上方 CX31993 条目）
- ~~使用 pa_simple_read 录音效率仅 67%~~ → 实际是 CX31993 硬件占空比
- ~~PipeWire suspend-node 导致设备挂起~~ → 测试证明加常驻流也无法阻止
- ~~常驻录音流解决~~ → keep-alive 流已移除，替换为按需录音 + drain

### Zipformer 调试

#### sherpa-onnx 输出流
- sherpa-onnx 将文本结果和 JSON 输出到 **stderr**，而非 stdout
- stdout 只有 "Start to create recognizer" 和 "Recognizer created in X s"
- 用 `popen("r")` 只能读 stdout，需改为 fork + pipe 分开捕获 stdout/stderr
- **选项格式**: sherpa-onnx 要求 `--encoder=value`（带`=`），用 `execl` 时必须拼接为单个 argv 字符串，不能用空格分隔的独立参数

#### 录音竞争条件
- `stop()` 中用 `std::thread(...).detach()` 启动子进程识别
- 析构函数中 `unlink(tempWavPath_)` 删除 WAV，与 detached 线程存在 **TOCTOU 竞争**
- 修复: `unlink(wav)` 移到 detached 线程中 `pclose` 之后执行，析构函数不删文件
- 线程安全: lambda 按值捕获 `modelDir_`，不持有 `this` 指针

#### 退出码 65280
- 65280 = 0xFF00 = `WEXITSTATUS(255)` × 256
- `pclose` 返回 raw wait status，需用 `WIFEXITED/WEXITSTATUS` 提取退出码
- exit code 255 常见原因: shell 找不到命令 / 文件不存在 / 参数格式错误

#### JSON 解析
- `rfind("\"text\"")` 定位到 `"text":` key 后，需跳过 9 个字符（不是 8 个）
  - `"text"` = 6, `": "` = 2, 值开头的 `"` = 1, 共 9
- `pos += 8` 正好落在值开头的引号上，`find('"', pos)` 找到同一字符，substr 结果为空

#### 响度归一化
- 简单峰值归一化不如 EBU R128 标准（K-weighting + gating）
- 使用 **libebur128** (`pkg-config: libebur128`)，依赖加入 `adapter/src/meson.build`
- ebur128 API: `ebur128_init(ch, sr, EBUR128_MODE_I)` → `ebur128_add_frames_short()` → `ebur128_loudness_global()` → `ebur128_destroy()`
- 归一化目标: **-16 LUFS**（语音播客标准响度，比广播 -23 LUFS 更适合 ASR）
- **边界处理**: `loudness < -70` 或 `-inf` 时 skip 归一化（gain=1.0）
- gain 裁剪: 0.05x ~ 20x，防止极端情况

#### 录音流程改进
- 改为先收集全部 PCM 到 `std::vector<int16_t>`，归一化后再写 WAV，便于内存中处理
- 添加 `max_abs` 调试日志，方便排查录音是否静音
- `pa_simple_read` 阻塞直到 `bytes` 字节就绪：`sizeof(buf)=4096` 在 16kHz S16LE 下 ≈128ms/iter

#### 音量对识别的影响（sherpa-onnx zipformer）
- 用测试音频 (10s) 降幅测试:
  - 100% → 完整识别全文
  - 10% → 部分缺漏
  - 1% → 几乎全丢 (只识别尾部少数词)
- `normalize_samples=True` 有内部归一化，但极端小振幅 + 量化噪声时无效

### 构建依赖
- libebur128: Arch `extra/libebur128`, 头文件 `<ebur128.h>`, meson `dependency('libebur128')`
- parecord 测试麦克风: `parecord --rate=16000 --format=s16le --channels=1 /tmp/test.wav`
- `pactl list short sources` 查看可用录音源

### 常驻 WebSocket Server 尝试（最终回退到 fork+exec）

#### 动机
sherpa-onnx-offline 每录音 fork+exec 一次，模型加载（尤其是 FireRed 1.2GB AED 模型）在每次调用中重新初始化 ONNX Runtime 计算图，耗时 ~3.2s。用 `sherpa-onnx-offline-websocket-server` 常驻进程可以省掉重复初始化。

#### 协议陷阱
WebSocket server 的 binary 消息格式是：
```
[int32_le sample_rate] [int32_le float_byte_count] [float_le PCM...]
```
**不是**原始 WAV 文件。发 WAV 会导致 sample_rate 被读成 `"RIFF"`（0x46464952=1.18e9），duration 被算成 6.8e-5 秒，触发 `payload too large` 拒绝。解决方案：解析 WAV header，int16→float 转换后按协议打包。

#### 崩溃问题
server 进程在 fcitx5 内 `fork()` 出来后不稳定：
- 端口打开成功（TCP probe 通过）
- ~2s 后进程崩溃（ECONNRESET）
- 降线程（30→4→1）、加 settle 等待、进程探活均无效
- 根因推测：fcitx5 多线程环境下 `fork()` 存在 mutex 继承、FD 泄漏等问题，子进程状态不确定

#### 结论
- **回退到 fork+exec 每录音**，简单可靠
- 后续考虑 systemd service 托管 server 进程（不在 fcitx5 进程内 fork）
- FireRed 常驻化预期可省 ~2s（计算图初始化），推理本身 ~1s
- zipformer（~100MB）模型小、doubao（云端）不受影响

### 64K 录音缓冲区（消除 drain）

#### 发现
- 原 4K 缓冲在 CX31993 burst 期需读 15 次，`stop()` 可能落在 burst 中间导致丢尾部数据
- 改成 **64K 缓冲区**（2s 音频量）后 `pa_simple_read` 阻塞读自然跨过硬件周期：fill 期阻塞 2s 等于一次完整 burst 投递
- `stop()` 置位后循环末次读一定在 fill 期阻塞过，数据完整
- n_reads 从 ~2000（4K）降到 ~1（64K），一次读就够了

#### 结论
显式 drain 逻辑（原有时间推算/计时检测方案）全部移除，大缓冲区 + 阻塞读本身既是同步也是收尾。

### Ctrl+CapsLock 模型切换

#### 设计
- **CapsLock 长按** → 语音录音识别（不变）
- **Ctrl+CapsLock** → 独立切换模式：方向键切换模型，不启动录音
- `switchActive_` 标志隔离两个模式，消除识别与切换的冲突

### Wayland 下 commitString 窗口绑定不可行

#### 发现
- `commitString()` 最终通过 `zwp_input_method_v1::commit_string` 发给合成器
- 合成器只路由到**当前键盘焦点 surface**，不认 fcitx5 内部的 IC UUID
- 即使 `findByUUID()` 返回正确 IC，commit 仍到焦点窗口
- 日志验证：`findByUUID` 返回 Firefox IC、`commitString` 调用成功，但文本到鼠标所在窗口

#### niri 窗口绑定方案
- `onActivate()` 时 `niri msg focused-window` 捕获窗口 ID
- 提交时 `niri msg action focus-window --id <captured>` → 等 150ms → `commitString` → `niri msg action focus-window --id <original>` 恢复焦点
- **代价**：提交瞬间焦点跳闪一次
- `capturedWinId_` 不随 `onDeactivate()` 清除（ASR 结果在 deactivate 后到）

### 豆包 API header 解析修正
- doubao_provider.cpp 中 `getHeader()` 匹配 `"\n" + name + ":"` 防止命中 `access-control-expose-headers` 值里的 `X-Api-Status-Code` 字串
- WAV cleanup 移到 detached 线程 lambda 中（RAII `Cleanup` struct），析构函数不删文件

### Python 包管理规则（全局）
写在 `~/.config/opencode/AGENTS.md`：
- 唯一允许用 `uv venv && uv pip install xxx`
- 禁止 pip install、pacman -S python-xxx、`uv pip install --system`

## 2026-06-02

### 构建系统
- fcitx5 需要 **C++20** 编译（头文件中用了 `std::source_location`、`std::span`、`std::string_view::starts_with` 等）
- meson `default_options: ['cpp_std=c++20']`
- pkg-config 依赖名: `Fcitx5Core`

### Addon 注册机制
- `FCITX_ADDON_FACTORY(ClassName)` 宏展开为一个 `extern "C"` 函数 `fcitx_addon_factory_instance()`
- fcitx5 通过 `dlsym` 查找该符号，调用获取 Factory，再通过 `factory->create(manager)` 实例化 AddonInstance
- 定义位置: `/usr/include/Fcitx5/Core/fcitx/addoninstance.h:201`

### 安装路径
- `.so` → `/usr/lib/fcitx5/`
- `.conf` → `/usr/share/fcitx5/addon/`
- 可通过 `pkg-config --variable=libdir Fcitx5Core` 获取 libdir

### 事件监听
- `instance_->watchEvent(EventType, EventWatcherPhase, callback)` 返回 `std::unique_ptr<HandlerTableEntry<EventHandler>>`
- PreInputMethod 阶段可用于拦截快捷键（早于输入法引擎处理）
- `KeyEvent::filterAndAccept()` 拦截事件 + 标记已处理

### 跨 addon 调用 (FCITX_ADDON_DEPENDENCY_LOADER)
- 宏生成懒加载函数，首次调用时通过 `AddonManager::addon("name")` 获取目标 addon 实例
- 必须在调用它的方法之前声明（`auto` 返回类型推导依赖声明顺序）
- 必须在引用的成员变量（如 `instance_`）之后声明
- 获取实例后通过 `addon->call<INamespace::function>(args)` 调用导出函数

### 定时器
- `instance_->eventLoop().addTimeEvent(CLOCK_MONOTONIC, usec, accuracy, callback)` 创建定时器
- **usec 是绝对时间（微秒），不是相对偏移**。需传入 `fcitx::now(CLOCK_MONOTONIC) + delay`
- `fcitx::now(clockid)` 获取当前时间：头文件 `<fcitx-utils/eventloopinterface.h>`
- 返回 `std::unique_ptr<EventSourceTime>`，reset 即取消
- callback 返回 false 表示一次性定时器

### notifications addon
- 依赖声明: `find_package(Fcitx5Module REQUIRED)` / `dependency('Fcitx5Module')`
- addon 注册文件中声明依赖: `[Addon/Dependencies] 0=notifications`
- 跨 addon 调用 API: 头文件 `fcitx-module/notifications/notifications_public.h`
- `sendNotification(appName, replaceId, appIcon, summary, body, actions, timeout, actionCb, closedCb)`
  - 第二个参数 `replaceId` 是 `uint32_t`(数字)，不是字符串

### commitString
- `InputContext::commitString(text)` 向当前应用提交文本
- `InputContext::updateUserInterface(InputPanel)` 刷新 UI

### ASR Provider 接口
- `IAsrProvider`: 抽象基类，`start()` / `stop()` + 异步回调（onResult/onError/onState）
- `IAsrProviderFactory`: 工厂, `id()` / `name()` / `create()`
- `AsrProviderRegistry`: 全局单例注册表，adapter 通过它发现和创建后端
- MockAsrProvider: 测试用，`start()` 立即同步返回 "你好世界"
- **自动注册**: 各 provider .cpp 中用 static 初始化 lambda 调用 `registerFactory()`，编译链接即自动注册，无需手动在 adapter 中注册
- **静态库需要 `link_whole`**: 用 `link_with` 会导致静态初始化器被链接器丢弃，必须改 `link_whole` 才能保留全局 static 对象
- **运行时切换**: 激活后按 ←/h（上一）→/l（下一）切换 ASR 后端，自动保存为默认
- **配置系统**: 使用 `FCITX_CONFIGURATION` 宏 + `readAsIni/safeSaveAsIni` 持久化默认后端
  - 头文件: `<fcitx-config/configuration.h>`, `<fcitx-config/iniparser.h>`, `<fcitx-utils/i18n.h>`

### 常见 addon 参考
- clipboard: `fcitx5/src/modules/clipboard/` (Module 类型)
- quickphrase: `fcitx5/src/modules/quickphrase/` (PreInputMethod 阶段拦截)
- quwei: `fcitx5-quwei/` (简单 InputMethodEngine 入门示例)

### CapsLock 还原
- Wayland V1 下 fcitx5 无法拦截 CapsLock (compositor 先处理)
- **uinput**: 内核级虚键注入, 兼容所有 compositor。关键: 设备必须**常驻**（创建一次、反复发键），不能每次创建销毁——compositor 需要时间初始化虚拟键盘才能正确处理 modifier 切换
- XTest 只能影响 XWayland，Wayland 原生应用无效

### systemd 懒启动 WebSocket 服务（替代 fork+exec）

#### 动机
每录音 fork+exec `sherpa-onnx-offline` 的致命问题：FireRed 1.2GB AED 模型每次初始化 ~3.2s（加载 ONNX + 建计算图），zipformer ~100MB 模型 ~1s。如果用户连说 3 句短句，累计初始化耗时 10s。用 systemd 托管 `sherpa-onnx-offline-websocket-server` 常驻进程，模型只加载一次。

#### 架构
```
fcitx5 addon                    systemd --user               sherpa server
  │                                 │                           │
  │ portReachable(18000)?          │                           │
  │  ├─ yes → connect                │                           │
  │  └─ no  → system("systemctl    │                           │
  │            --user start         │                           │
  │            ainput-sherpa-       │                           │
  │            zipformer")  ──────► │ fork+exec ──────────────► │
  │              wait port ◄────── │                           │
  │              connect ──────────────────────────────────►  │
  │              WAV bytes ◄────────────────────────────────── │
  │                                 │                           │
  │ commitString(text)              │                           │
```

#### 端口分配
- zipformer (transducer, 100MB): `127.0.0.1:18000`
- fire_red (AED, 1.2GB): `127.0.0.1:18001`

#### 懒启动机制
- 服务 `WantedBy=default.target` 但 provider 代码按需 `systemctl --user start`
- `ensureSherpaService()`: 先 `portReachable()` → 不通则 `systemctl --user start` → poll connect 最多 10s
- `Restart=on-failure + RestartSec=5`: 进程崩溃自动重启
- `Slice=user-expensive.slice`: 不挤占桌面进程资源

#### 日志
- `~/.local/share/ainput/logs/zipformer-server.log`
- `~/.local/share/ainput/logs/fire-red-server.log`

#### 注意事项
- 服务有 `RestartSec=5`，崩后 5s 内新一轮识别会 connect 失败（已在 try-catch 中处理，触发 error callback）
- 两个 server 常驻会占内存（尤其是 FireRed ~5GB），未来可加 idle 自动 stop 机制
- 如果不需要常驻，`systemctl --user disable --now ainput-sherpa-*` 即可，不影响代码

### rime 拉丁字母模式切换（已解决）

#### 现象
每次按下 CapsLock，rime 输入方案从雾凇拼音切到拉丁字母（ASCII 模式）。长短按都触发，和 Ainput 插件代码无关。

#### 根因
rime 默认配置 `~/.local/share/fcitx5/rime/build/default.yaml`：
```yaml
ascii_composer:
  switch_key:
    Caps_Lock: clear
```
`clear` = 清除 preedit + 切换 ascii_mode。

#### 错误排查方向
- 误以为是 `setCurrentInputMethod()` 重新激活 rime 导致 switch reset
- 改 `rime_ice.custom.yaml` 的 `"switches/@1/reset"`——但 `@1` 在 rime 补丁中是 **0-based**，指第二个 switch（ascii_punct 标点），不是 ascii_mode
- 加了 IM 缓存/恢复代码（`savedIM_`、`setCurrentInputMethod()`），均无效，最终全部回退

#### 修复（rime 配置层）
`~/.local/share/fcitx5/rime/default.custom.yaml`：
```yaml
"ascii_composer/switch_key/Caps_Lock": noop
```
`noop` = rime 不响应 CapsLock 按键。插件的 `filterAndAccept()` + `revertCapsLock()` 正常工作。

### niri 窗口焦点等待改为动态轮询

固定 150ms → 轮询 `niri msg focused-window`（10ms 间隔，最长 500ms 兜底）。确认焦点切到目标窗口后立刻 commit。典型耗时 150ms → ~30ms。

### 构建部署

安装用 `sudo meson install -C build`（禁止手动 `sudo cp`）。安装路径由 fcitx5 pkgconfig 的 `libdir` 决定。

### sherpa-onnx-offline-websocket-server 崩溃问题

v1.13.2 二进制启动后 ~15s 必定 SIGABRT 崩溃，与 `--num-threads` 和运行环境（fcitx5 内 fork / systemd 独立进程）无关。社区有类似 issue（[#3381](https://github.com/k2-fsa/sherpa-onnx/issues/3381), [#2864](https://github.com/k2-fsa/sherpa-onnx/issues/2864)）。本地模型目前只能用 fork+exec 每录音。

### Qwen3-ASR-Flash Provider（千问语音识别）

#### API 选择
- **选用模型**: `qwen3-asr-flash`（非 `qwen3-asr-flash-filetrans`）
- **原因**: filetrans 仅支持公网 URL 输入，不支持 base64 内联音频。flash 模型支持 Data URL (`data:audio/wav;base64,xxx`)，适合本地录音输入
- **限制**: ≤10MB / ≤5 分钟，语音输入场景足够

#### API 端点
- 提交: `POST https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation`
- 认证: `Authorization: Bearer {api_key}`
- 调用模式: **同步单次调用**（与 Doubao 的 submit+poll 异步模式不同），一次请求返回结果

#### 关键实现差异（vs Doubao）
- **不需要轮询**: DashScope multimodal API 是同步的，一次 HTTP 调用直接返回识别文本
- **Data URL 格式**: base64 前需加 `data:audio/wav;base64,` 前缀
- **消息格式**: 使用 multimodal conversation 格式而非纯 ASR 格式
- **配置路径**: `~/.config/ainput/qwen.json`（仅 `api_key` 一个字段）

#### 代码位置
- `ASR_provider/src/qwen_provider.h` — 头文件
- `ASR_provider/src/qwen_provider.cpp` — 实现
- Provider ID: `"qwen"`, 显示名: `"Qwen3-ASR-Flash (Alibaba DashScope)"`
- 工厂注册: 在 `qwen_provider.cpp` 末尾 static 初始化器自动注册
- `adapter/src/ainput.cpp:36` — include 确保链接
- `ASR_provider/src/meson.build:13-14` — 编译条目

#### 配置示例
```json
{"api_key": "sk-xxx"}
```

#### API Key 获取
https://bailian.console.aliyun.com/?tab=model#/api-key（北京地域）

### 录音与 ASR 解耦（refactor: 2026-06-04）

#### 动机
录音逻辑（pa_simple_new/read/free、buffer 检测、ebur128 归一化、WAV 写入）在 4 个 provider 中完全重复，每个 ~80 行。录音是 ASR 无关的通用能力。

#### 方案
- 新增 `AudioCapture` 类 (`audio_capture.h/.cpp`)：统一负责 PulseAudio 录音、buffer 检测、ebur128 归一化、WAV 写入
- `stop()` 阻塞等待录音线程结束，调用后立即可取 `takeSamples()` 和 `wavPath()`
- `IAsrProvider` 接口从 `start()/stop()` 改为 `transcribe(samples, wavPath)`，移除 `onState_`/`onStatusText_`
- State/StatusText 回调移到 `AudioCapture`，adapter 直接设置

#### 各文件变化
- `audio_capture.h/.cpp`：新增，~170 行
- `asr_provider.h`：移除 start/stop/onState/onStatusText，加 transcribe
- `mock_provider.h/.cpp`：start/stop → transcribe
- `doubao/qwen/zipformer/fire_red_provider.{h,cpp}`：移除 recordLoop/start/stop/PA/buffer/ebur128/WAV 代码

#### 测试结果
初步测试无异常，功能正常。

## 2026-06-04 (续)

### ASR 结果→上屏 解耦（refactor: 2026-06-04）

### 音频处理流水线最终状态

流水线：`录音 → 归一化(ebur128) → 降噪 → VAD trim(裁头尾) → 写WAV`

#### 降噪后端（Ctrl+CapsLock 后 ↑↓ 切换）
| 降噪器 | 多核 | 耗时 | 原理 |
|--------|------|------|------|
| none | N/A | 0 | 跳过 |
| speexdsp | 单线程（自适应，分块会失效） | 3ms | 频谱减法，帧间累积噪声模型 |
| deepfilter | 单核（GRU 串行依赖） | 220ms | DeepFilterNet3, ONNX tract 推理 |

- **config**: `~/.config/ainput/audio.json` — `{"denoise": "speexdsp"}` / `"deepfilter"` / `false`
- **deepfilter 二进制**: 自编译（`cargo build --release`），含 `--stay` 常驻 daemon 消除模型加载开销（777ms→220ms）
- **deefilter 重采样**: libsoxr 16k↔48k（纯 C，零 Python 依赖）
- **speexdsp 多线程失败原因**: 自适应噪声学习依赖连续帧，分块=每块从头学=无效，回退单线程
- **deepfilter 多线程不可行**: tract 无多线程计划类型，GRU 帧间串行依赖
- **VAD trim**: 最小头部裁切 0.2s，峰均比方案已被 speexdsp VAD 替代，trimStart 钳制到 trimEnd 防下溢

#### 测试工具
- `tools/test_audio_pipeline` — 实时录音测试（PulseAudio → 流水线 → WAV）
- `tools/test_audio_file` — 文件处理测试（WAV → 流水线 → WAV）

#### DeepFilterNet3 细节
- 二进制: `~/.local/share/ainput/bin/deep-filter`（自编译原生二进制，--stay 常驻模式）
- 关键发现: `deep-filter -o <dir>` 当 dir 与输入同目录时原地覆写（非额外输出）
- musl 预编译版 vs 原生编译: 1038ms → 777ms（25% 快），加 daemon 后 218ms
- 代码: `ASR_provider/src/audio_capture.cpp:dfDenoise()`

### ASR 结果→上屏 解耦（refactor: 2026-06-04）

#### 动机
`AinputAddon` 中 self-pipe reader lambda（`ainput.cpp:78-156`）将 ASR 结果处理、状态显示、niri 窗口切换、`commitString()` 调用全部耦合在一个函数中。没有抽象层，无法替换显示策略或扩展。

#### 方案
- 新增 `OutputHandler` 类（`adapter/src/output_handler.h/.cpp`）：封装 self-pipe 机制、niri 窗口焦点切换、commitString() 调用
- `AinputAddon` 不再直接管理 pipe/PendingCommit，改由 `OutputHandler` 处理所有上屏逻辑
- 接口：
  - `submit(text)` — 提交 ASR 识别结果
  - `showStatus(text)` — 显示状态文本
  - `setCaptureWindow(winId)` — 设置 niri 捕获窗口
  - `setPressTime(t)` — 设置计时参考点

#### 各文件变化
- `adapter/src/output_handler.h/.cpp`：新增，~130 行
- `adapter/src/ainput.cpp`：移除 `wakePipe_`、`wakeWatcher_`、`PendingCommit`、`pendingMutex_`、`pendingCommits_`、`capturedWinId_`、`niriFocusWindow()`；新增 `outputHandler_`
- `adapter/src/meson.build`：添加 `output_handler.cpp` 到编译源

#### 解耦边界
```
AinputAddon::onAsrResult()  →  OutputHandler::submit()
AinputAddon::statusText     →  OutputHandler::showStatus()
AinputAddon::onActivate()   →  OutputHandler::setCaptureWindow()

OutputHandler 内部:
  self-pipe  →  drainAndCommit()  →  niri 切换  →  commitString()
```

#### 当前架构全景
```
┌─ AinputAddon ─────────────────────────────────────────────┐
│  KeyEvent → 生命周期 → ASR/降噪器切换 → 提示音 → 配置       │
│                                                            │
│  ┌──────────┐   ┌──────────────┐   ┌──────────────────┐  │
│  │AudioCapture│  │IAsrProvider  │   │ OutputHandler    │  │
│  │ 录音/归一化│  │ transcribe() │   │ 上屏(self-pipe,  │  │
│  │ /降噪/VAD │  │              │   │  niri, commit)    │  │
│  └──────────┘   └──────────────┘   └──────────────────┘  │
│       │                │                    ▲             │
│       └───Callback─────┘────────────────────┘             │
│         Recorded → transcribe → onResult → submit         │
└──────────────────────────────────────────────────────────┘
```

### 配置文件分类（refactor: 2026-06-04）

用户必改 vs 高级可选两个层级：

**用户必改 (lean, 4 个文件):**
| 文件 | 字段 | 说明 |
|------|------|------|
| `doubao.json` | `api_key`, `resource_id` | 豆包 API 凭据 |
| `qwen.json` | `api_key` | 千问 API 凭据 |
| `output.json` | `desktop` | 桌面环境 (`niri`/`hyprland`/`gnome`/`none`) |
| `vinput.json` | `activation_msec`, `debounce_count`, `notification_timeout` | 交互行为参数 |

**高级可选 (单文件, 全部有硬编码默认值):**
| 文件 | 节 | 字段 |
|------|-----|------|
| `advanced.json` | `audio` | `lufs_target`(-16.0), `speex_level`(-15) |
| | `doubao` | `poll_interval_msec`(800), `max_polls`(75), `submit_timeout_sec`(30), `query_timeout_sec`(15) |
| | `qwen` | `timeout_sec`(60) |
| | `zipformer` | `model_dir`, `num_threads`(30), `bin_path` |
| | `fire_red` | `model_dir`, `num_threads`(30), `bin_path` |

`advanced.json` 不存在时所有值退回到 C++ 硬编码默认值。

#### 关键问题：配置加载无日志，排查困难
- `AudioCapture()` 构造函数中加载 `advanced.json[audio]` 的 crest_threshold/lufs_target/speex_level，但没有任何日志
- 如果文件不存在 / section 找不到 / JSON 解析失败，静默用默认值 2.4，用户完全不知道
- `hasVoice()` 的 crest factor 比较同样不记录阈值，只看日志无法确认实际使用的值
- **修复**: `audio_capture.cpp:77-80` 新增构造函数日志（显示实际解析值或 "not found"），`audio_capture.cpp:408-410` 新增 crestFactor vs crestThreshold_ 对比日志

#### 辅助函数
`ainput_config.h` 新增 `advancedSection(key)` — 从 advanced.json 提取嵌套 JSON 节。



## 2026-06-15

### 项目规范化重构：自动化测试与模块边界

#### 问题
- `meson test -C build` 原先输出 `No tests defined.`，没有任何自动化回归入口。
- `docs/ainput/06-asr-provider-api.md` 仍描述旧的 `IAsrProvider::start()/stop()` 录音契约，和当前 `transcribe(samples, wavPath)` 实现不一致。
- `ASR_provider/` 与 `adapter/` 缺少模块级 `WhyThisArchWork.md`，不能满足模块边界和架构论证要求。
- `ASR_provider/src/meson.build` 没有声明自身使用的 `libpulse-simple`、`libebur128`、`libcurl`、`soxr`、`speexdsp` 依赖，依赖边界泄漏到 consumer。

#### 已改动
- 新增 `tests/meson.build` 并在根 `meson.build` 中 `subdir('tests')`。
- 新增 `tests/test_asr_registry.cpp`：验证 `AsrProviderRegistry` 静态注册、`mock` provider 创建和 `transcribe()` 回调结果。
- 新增 `tests/test_audio_capture_file.cpp`：生成确定性 16kHz mono 样本，走 `AudioCapture::processSamples(samples, "none")` 和 `AudioCapture::writeWav()`，验证 WAV header/data size。
- 新增 `tests/README.md`：说明自动化测试边界和 fixture 策略。当前测试使用内存生成音频，不依赖麦克风/API/模型/桌面。
- 更新 `ASR_provider/src/meson.build`：`asr_provider_dep` 现在包含 ASR 模块自身的外部库依赖，consumer 只需依赖 `asr_provider_dep`。
- 简化 `tools/meson.build`：`test_audio_file` 只依赖 `asr_provider_dep`；`test_audio_pipeline` 额外保留直接使用的 `libpulse-simple`。
- 更新 `.gitignore`：忽略当前实际本地构建目录 `build/` 和项目级虚拟环境 `.venv/`。
- 重写 `docs/ainput/06-asr-provider-api.md`：当前契约是 `AudioCapture -> samples + wavPath -> IAsrProvider::transcribe() -> onResult/onError`。
- 重写 `docs/ainput/05-voice-input-design.md`：补齐 `AinputAddon`、`AudioCapture`、`IAsrProvider`、`OutputHandler`、`DesktopStrategy` 的当前数据流和失败模式。
- 新增 `ASR_provider/WhyThisArchWork.md` 与 `adapter/WhyThisArchWork.md`，记录职责边界、依赖、数据流、失败模式、替换成本、验证方式和独立子 Agent 审查结论。

#### 验证
- `meson setup build --reconfigure`：成功，Build targets 从 4 增加到 6。
- `ninja -C build`：成功。
- `meson test -C build`：2/2 通过。

#### 独立审查
- 使用 explore 子 Agent 审查项目规范符合性，确认最高优先级问题为：缺少 Meson 自动化测试、ASR Provider API 文档过期、缺少 `WhyThisArchWork.md`、ASR Meson 依赖边界不清。
- 本次重构采纳了这些建议中的 P0 项和一项 P1 依赖边界修复。

### 项目规范化重构：配置样例与手动工具边界

#### 问题
- `config/*.json` 被 Git 跟踪，虽然当前是占位符，但文件名和运行时配置一致，容易误提交真实 API key。
- `README.md` 直接写入 JSON 配置，弱化了“仓库保存样例、用户本地保存真实配置”的边界。
- `tools/` 下的 `test_*` 文件名容易和自动化测试混淆，但它们依赖麦克风、PulseAudio 或历史诊断上下文，不应默认进入 `meson test`。
- `README.md` 使用 `yay -S`，和本机 AUR 工具约定 `paru` 不一致。

#### 已改动
- `config/doubao.json`、`config/qwen.json`、`config/output.json`、`config/audio.json`、`config/vinput.json`、`config/advanced.json` 迁移为对应的 `.json.example` 文件。
- `.gitignore` 新增 `config/*.json`，避免仓库误收真实本地配置；`.json.example` 仍可跟踪。
- `README.md` 改为复制 `config/*.json.example` 到 `~/.config/ainput/*.json` 后再编辑本地配置。
- `README.md` 的 AUR 示例从 `yay -S fcitx5-ainput-git` 改为 `paru -S fcitx5-ainput-git`。
- 新增 `tools/README.md`，明确 `tools/` 是手动诊断和集成辅助；默认自动化测试在 `tests/` 中通过 `meson test -C build` 运行。
- `tools/meson.build` 删除不再需要的直接依赖查询，仅保留 `test_audio_pipeline` 直接使用的 `libpulse-simple`。

#### 验证
- 并行运行 `meson setup build --reconfigure` 与 `ninja -C build` 时，Meson build directory lock 冲突导致 reconfigure 一次失败；这是并行验证方式问题，不是项目构建失败。
- 顺序运行 `meson setup build --reconfigure`：成功。
- `ninja -C build`：成功。
- `meson test -C build`：2/2 通过。

### 项目规范化重构：ASR_provider 与 config 文档边界

#### 问题
- `ASR_provider/src/` 仍是扁平目录，直接移动文件会引入较大 include/meson 风险，但缺少 README 描述逻辑分组和迁移规则。
- `config/` 已迁移为 `.json.example`，但缺少目录级 README 固定“仓库样例 vs 用户真实配置”的边界。

#### 已改动
- 新增 `ASR_provider/README.md`：记录 public contract、当前逻辑分组（Core/Audio/Providers/Support/Build）、依赖归属、添加 provider 的步骤、失败边界和验证方式。
- 新增 `config/README.md`：记录 `*.json.example` 跟踪规则、`config/*.json` 禁止跟踪规则、复制到 `~/.config/ainput/` 的初始化命令、各配置文件用途和新增配置键时的更新规则。
- 更新 `README.md`：明确 `config/*.json.example` 只是仓库样例，运行时配置文件应放在 `~/.config/ainput/`，仓库内 `config/*.json` 被 Git 忽略。

#### 验证
- `ninja -C build`：成功。
- `meson test -C build`：2/2 通过。

#### 未处理风险
- `ASR_provider/src/` 目录仍较平，但已有 `ASR_provider/README.md` 固定逻辑分组。后续如继续增长，可在测试保护下分阶段移动到 `core/`、`audio/`、`providers/`、`support/`。

### 探测设备占空比缓存按设备隔离（2026-06-15）

#### 问题
- `ASR_provider/src/buffer_detect.cpp` 原先把硬件 burst 检测结果保存为 `~/.config/ainput/pa_buffer.json` 中的单个 `buffer_bytes`。
- 多个 PulseAudio 录音 source 共用该值，导致一台设备的占空比/缓冲大小会错误套用到另一台设备。

#### 已改动
- `ASR_provider/src/buffer_detect.cpp` 现在通过 `pactl get-default-source` 获取当前默认 source id，并按 source id 读写缓存。
- `pa_buffer.json` 新格式为 `{"devices":{"<source-id>":{"buffer_bytes":64000}}}`；旧格式 `{"buffer_bytes":64000}` 会在首次读取时迁移到当前 source。
- 新增测试环境变量：`AINPUT_PA_BUFFER_CONFIG` 指向临时缓存文件，`AINPUT_PA_SOURCE_ID` 覆盖 source id，供自动化测试避免访问真实 PulseAudio 设备。
- 新增 `tests/test_buffer_detect_cache.cpp`，覆盖旧格式迁移和两台设备缓存互不覆盖。
- 更新 `ASR_provider/WhyThisArchWork.md` 与 `tests/README.md` 记录 per-device cache 边界和验证方式。

#### 验证
- `ninja -C build`：成功。
- `meson test -C build`：3/3 通过。
