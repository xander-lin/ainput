# fcitx5-ainput

[English](README.md) | **中文**

fcitx5 的按住说话语音输入插件：按住 **CapsLock** 说话，松开后识别结果直接
插入光标处。Ainput（**a**udio **input**）是一个运行在 Linux 桌面上的小型
fcitx5 addon。面向 fcitx5 的那一层用 C++，采集、音频处理与识别后端用 Rust。

## 功能

- **CapsLock 按住说话** —— 不需要额外的快捷键守护进程，插件由 fcitx5 自身
  加载。
- **可插拔的识别后端** —— 两个云端接口（阿里千问、字节豆包）和两个本地引擎
  （sherpa-onnx Zipformer / FireRed）共用一个接口，`Ctrl+CapsLock` 随时切换，
  选择会写回配置。
- **连续说话** —— 上一句还在识别时可以开始下一句（最多 3 条在途，顺序保持，
  每条结果回到该次录音开始时的焦点窗口）。
- **音频预处理** —— 响度归一化、语音门限、可选降噪、静音裁剪。
- **可读的失败提示** —— 缺 key、key 被拒、模型不存在、本地环境错误、网络、
  超时、未检测到语音，状态栏会指出是哪一种，并且绝不会把错误文本插入文档。
- **CapsLock 还原** —— 通过 `/dev/uinput` 注入一次合成按键，把锁定状态恢复，
  不会说完一句话后 CapsLock 一直亮着。

## 依赖

- 安装了 fcitx5 的 Linux 桌面（`fcitx5`、`libpulse`、`libebur128`、
  `libcurl`、`speexdsp`、`libsoxr`）
- 从源码构建另需 C++20 编译器、Meson、Ninja 与 Rust 工具链
- 使用本地引擎时另需 `sherpa-onnx` 二进制与模型文件（可选，合计约 1.3 GB）

## 安装

Arch Linux（AUR）：

```bash
paru -S fcitx5-ainput-git
```

从源码构建：

```bash
meson setup build --prefix=/usr
ninja -C build
sudo meson install -C build
```

两种方式装完都要重启 fcitx5：

```bash
fcitx5 -r
```

包内还安装了 `/dev/uinput` 的 udev 规则，用于还原 CapsLock 状态。见
[常见问题](#常见问题)。

## 使用

| 操作 | 按键 |
|------|------|
| 语音输入 | 按住 **CapsLock** → 说话 → 松开 |
| 切换识别后端 | **Ctrl+CapsLock**，然后 **← / →**（或 **h / l**） |
| 切换降噪器 | **Ctrl+CapsLock**，然后 **↑ / ↓**（或 **k / j**） |

录音时输入框旁显示 `listening...`；松开后依次经过 `processing audio...`、
`recognizing...`，最后要么上屏文本，要么显示一条可操作的错误提示。

## 识别后端

| 后端 | 运行位置 | 需要 |
|------|----------|------|
| Qwen | 阿里云百炼（云端） | API Key |
| Doubao | 字节火山引擎（云端） | API Key + resource ID |
| Zipformer | 本地流式 | `sherpa-onnx` + 模型（下载约 490 MB） |
| FireRed | 本地离线 AED | `sherpa-onnx-offline` + 模型（下载约 800 MB） |
| Mock | 本地，仅测试 | 无 |

默认使用 Qwen。云端配置在每次识别前重新读取，改模型后下一句话即生效，无需
重启。

## 配置

**共享设置**（后端、按住时长、降噪器、音频参数）存在 fcitx5 自己的配置里，
可直接在 **fcitx5-configtool** 的 *附加组件 → Ainput* 中修改。

**各后端的数据**（凭据、模型 ID、路径）在 `~/.config/ainput/*.json`。这些文件
在首次运行时自动生成，**所有字段都写出来**并带默认值与注释，打开就知道能改
什么。云端最小配置：

```json
// ~/.config/ainput/qwen.json
{ "api_key": "sk-..." }
```

```json
// ~/.config/ainput/doubao.json
{ "api_key": "...", "resource_id": "volc.seedasr.auc" }
```

建议收紧权限：`chmod 600 ~/.config/ainput/{qwen,doubao}.json`。
Key 以明文保存，项目有意不集成密钥环。

完整配置项、默认值、校验规则以及 0.1 → 0.2 的迁移说明见
[`docs/configuration.md`](docs/configuration.md)。

## 本地模型（可选）

只有不想把音频发到云端时才需要。

```bash
mkdir -p ~/.local/share/ainput/{sherpa-onnx/bin,models}

# 二进制：-static 包同时包含 sherpa-onnx 与 sherpa-onnx-offline。
# 版本号请查看 https://github.com/k2-fsa/sherpa-onnx/releases
VERSION=v1.13.2
curl -LO "https://github.com/k2-fsa/sherpa-onnx/releases/download/v${VERSION}/sherpa-onnx-v${VERSION}-linux-x64-static.tar.bz2"
tar xf "sherpa-onnx-v${VERSION}-linux-x64-static.tar.bz2"
cp -r "sherpa-onnx-v${VERSION}-linux-x64-static/bin/"* ~/.local/share/ainput/sherpa-onnx/bin/

# 模型
MO="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"
curl -LO "$MO/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2"
tar xf sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2 \
    -C ~/.local/share/ainput/models/
mv ~/.local/share/ainput/models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20 \
   ~/.local/share/ainput/models/zipformer-zh-en

curl -LO "$MO/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2"
tar xf sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26.tar.bz2 \
    -C ~/.local/share/ainput/models/
```

后端只认这两个压缩包解出的文件名。其他压缩包文件名不同，需要自行准备符合
命名要求的 `model_dir`；自定义路径写在 `zipformer.json` / `fire_red.json`。

## 常见问题

**CapsLock 松手后仍然锁定。** Ainput 需要对 `/dev/uinput` 的写权限：

```bash
getfacl /dev/uinput        # 应出现 user:<你>:rw- 一行
```

没有这一行时，日志会输出 `cannot open /dev/uinput (errno=13)`。重新加载随包
安装的规则：

```bash
sudo modprobe uinput
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=misc --sysname-match=uinput
fcitx5 -r
```

部分合成器（观察到 Hyprland；niri 正常）即使权限正确也会忽略合成按键 ——
语音输入仍然可用，只是锁定状态不会还原。

**面板没有任何反应。** 查看 fcitx5 日志里 `Ainput` 的输出，确认插件已启用且
后端 key 有效。每种失败都有自己的提示（`fill api_key in ...`、
`API key rejected`、`network error`、`recognition timed out` 等）。

**提示 recognition queue full。** 同时最多接受 3 次录音，等
`recognizing...` 结束即可。

**配置改乱了想恢复默认。** 先停掉 fcitx5，删除
`~/.config/fcitx5/conf/ainput.conf`，再启动：文件会用全部选项和默认值重新
生成。

**fcitx5-configtool 里看不到 Ainput 的设置。** 确认插件已成功加载过（重启一次
fcitx5），且 configtool 与 fcitx5 版本一致。设置保存在
`~/.config/fcitx5/conf/ainput.conf`。

## 文档

- [`docs/configuration.md`](docs/configuration.md) —— 全部配置项与默认值
- [`docs/architecture.md`](docs/architecture.md) —— 模块边界、后端接口、
  如何新增后端
- [`docs/development.md`](docs/development.md) —— 构建、测试、诊断日志、
  仓库提交约定
- [`docs/history.md`](docs/history.md) —— 带日期的故障分析与工程日志
  （归档，不是当前规范）
- [`docs/qwen-asr-api.md`](docs/qwen-asr-api.md)、
  [`docs/doubao-auc-api.md`](docs/doubao-auc-api.md) —— 厂商接口笔记
- `man ainput` —— 安装后的手册页

## 许可证

MIT
