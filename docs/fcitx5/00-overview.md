# Fcitx5 框架概述

## 什么是 Fcitx5

Fcitx5 是一个跨平台的输入法框架，使用 C++ (C++17) 编写，发布于 LGPL-2.1+ 许可下。它通过 addon（插件）机制提供高度可扩展的架构。

- 源码: https://github.com/fcitx/fcitx5
- Wiki: https://fcitx-im.org
- API 文档: https://codedocs.xyz/fcitx/fcitx5/

## 核心架构

```
[fcitx5 server]
│
├── frontend/    # 前端 addon - 与应用通信 (XIM, DBus, Wayland)
├── modules/     # 模块 addon - 辅助功能 (剪贴板, 表情, 拼写检查等)
├── ui/          # 用户界面 addon (classicui, kimpanel)
├── im/          # 输入法引擎 addon (拼音, 五笔, 日文, 韩文等)
├── lib/         # 核心库 (fcitx, fcitx-utils, fcitx-config)
└── server/      # 服务器主程序
```

## 四种 Addon 类型

| 类型                   | 职责                                         | 示例                         |
| ---------------------- | -------------------------------------------- | ---------------------------- |
| **Frontend** (前端)    | 与应用通信，创建和管理 InputContext           | XIM, WaylandIM, DBusFrontend |
| **InputMethodEngine**  | 将用户输入 (按键) 转为文字                    | Pinyin, Mozc, Rime           |
| **UserInterface** (UI) | 显示输入法界面 (候选窗、状态栏等)             | ClassicUI, Kimpanel          |
| **Module** (模块)      | 不属于以上三类的其他功能 addon                | Clipboard, Spell, Emoji      |

对 Ainput (语音输入) 项目而言：
- `adapter/` 对应 Module 类型 addon，挂载到 fcitx5 框架中
- `ASR_provider/` 对应语音识别后端逻辑

## 文件结构

一个共享库 addon 的典型文件布局：

```
[install prefix]
├── share/fcitx5
│   ├── addon/<addon name>.conf        # addon 注册文件
│   └── inputmethod/<input method>.conf # 输入法注册文件 (仅 InputMethodEngine 需)
└── lib/fcitx5
    └── <library name>.so              # 编译产物
```

Fcitx 也遵循 XDG 目录标准，会检查 `$XDG_DATA_DIR/fcitx5` 下的文件。
