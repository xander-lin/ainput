# 开发 Fcitx5 Addon 实作指南

## 项目骨架

```
my-addon/
├── CMakeLists.txt
├── LICENSES/
│   └── BSD-3-Clause.txt
├── po/                          # 可选 I18n
│   ├── CMakeLists.txt
│   └── ...
└── src/
    ├── CMakeLists.txt
    ├── addon.conf.in.in         # Addon 注册文件模板
    └── myaddon.cpp              # 实现
```

## CMake 配置

### 根目录 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.21)
project(fcitx5-myaddon)

find_package(Fcitx5Core REQUIRED)
include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5CompilerSettings.cmake")

add_subdirectory(src)
```

### src/CMakeLists.txt (Module 类型)

```cmake
add_library(myaddon SHARED myaddon.cpp)
target_link_libraries(myaddon PRIVATE Fcitx5::Core)
set_target_properties(myaddon PROPERTIES PREFIX "")
install(TARGETS myaddon DESTINATION "${FCITX_INSTALL_LIBDIR}/fcitx5")

configure_file(addon.conf.in addon.conf)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/addon.conf"
        RENAME myaddon.conf DESTINATION "${FCITX_INSTALL_PKGDATADIR}/addon")
```

### 依赖其他 addon 导出模块

```cmake
find_package(Fcitx5Module REQUIRED COMPONENTS Punctuation QuickPhrase)
```

## Addon 配置文件模板

### addon.conf.in (Module 类型)

```ini
[Addon]
Name=My Addon
Category=Module
Version=@PROJECT_VERSION@
Library=myaddon
Type=SharedLibrary
OnDemand=False
Configurable=True
```

## 编译与安装

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Debug
make
sudo make install
```

## 重启 fcitx5

```bash
fcitx5 -rd
```

## Module 类型 Addon 模式

对于语音输入插件，适合实现为 Module 类型：

```cpp
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>

class AinputAddon : public fcitx::AddonInstance {
public:
    AinputAddon(fcitx::Instance *instance)
        : instance_(instance) {
        // 注册事件监听
        eventHandler_ = instance_->watchEvent(
            fcitx::EventType::InputContextKeyEvent,
            fcitx::EventWatcherPhase::PreInputMethod,
            [this](fcitx::Event &event) {
                auto &keyEvent = static_cast<fcitx::KeyEvent &>(event);
                onKeyEvent(keyEvent);
            });
    }

private:
    void onKeyEvent(fcitx::KeyEvent &keyEvent) {
        // 过滤释放事件
        if (keyEvent.isRelease()) return;

        auto *ic = keyEvent.inputContext();
        // 处理触发键
        if (keyEvent.key().checkKey(triggerKey_)) {
            startVoiceInput(ic);
            keyEvent.filterAndAccept();
            return;
        }
    }

    void startVoiceInput(fcitx::InputContext *ic) {
        // 启动语音识别
        // 结果通过 ic->commitString("text") 提交
    }

    fcitx::Instance *instance_;
    std::unique_ptr<fcitx::EventWatcher> eventHandler_;
    fcitx::Key triggerKey_;
};

class AinputFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        FCITX_UNUSED(manager);
        return new AinputAddon(manager->instance());
    }
};

FCITX_ADDON_FACTORY(AinputFactory);
```

## 向 InputContext 提交文本

```cpp
// 提交文本到当前应用
ic->commitString("你好世界");

// 也可以通过 InputPanel 显示候选/预编辑
auto &inputPanel = ic->inputPanel();
// 设置 preedit
inputPanel.setClientPreedit(fcitx::Text("preedit text"));
// 设置候选列表
auto candidateList = std::make_unique<fcitx::CommonCandidateList>();
candidateList->append<fcitx::CandidateWord>(fcitx::Text("候选1"));
inputPanel.setCandidateList(std::move(candidateList));
```

## 跨 Addon 通信

使用 `FCITX_ADDON_DEPENDENCY_LOADER` 宏引用其他 addon 的功能：

```cpp
class MyAddon : public fcitx::AddonInstance {
private:
    FCITX_ADDON_DEPENDENCY_LOADER(quickphrase, instance_->addonManager());
    FCITX_ADDON_DEPENDENCY_LOADER(punctuation, instance_->addonManager());
};
```

第一次调用时，依赖 addon 会被自动加载。

## 参考项目

- 简单输入法入门: https://github.com/fcitx/fcitx5-quwei
- 完整输入法引擎: https://github.com/fcitx/fcitx5-chinese-addons
- 模块类 addon (clipboard): https://github.com/fcitx/fcitx5/tree/master/src/modules/clipboard
- 模块类 addon (spell): https://github.com/fcitx/fcitx5/tree/master/src/modules/spell
- 模块类 addon (unicode): https://github.com/fcitx/fcitx5/tree/master/src/modules/unicode
