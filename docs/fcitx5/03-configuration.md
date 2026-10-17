# Fcitx5 配置系统

## 定义配置类

使用 `FCITX_CONFIGURATION` 宏定义配置结构：

```cpp
#include <fcitx/configuration.h>

FCITX_CONFIGURATION(
    AinputConfig,
    fcitx::KeyOption triggerKey{
        this, "TriggerKey", _("Trigger Key"),
        {fcitx::Key("Control+Alt+v")},
        fcitx::KeyListConstrain()
    };
    fcitx::Option<std::string> asrProvider{
        this, "AsrProvider", _("ASR Provider"), "openai"
    };
    fcitx::Option<std::string> apiKey{
        this, "ApiKey", _("API Key"), ""
    };
    fcitx::Option<int, fcitx::IntConstrain> sampleRate{
        this, "SampleRate", _("Sample Rate"), 16000,
        fcitx::IntConstrain(8000, 48000)
    };
);
```

## Option 类型

| Option 类型              | 说明                |
| ------------------------ | ------------------- |
| `Option<T>`              | 基本类型选项        |
| `Option<T, ConstrainT>`  | 带约束的选项        |
| `KeyOption`              | 快捷键选项          |
| `KeyListOption`          | 快捷键列表选项      |
| `Option<Color>`          | 颜色选项            |
| `Option<Font>`           | 字体选项            |
| `Option<I18NString>`     | 可翻译字符串        |

## 实现配置读写

```cpp
class AinputAddon : public fcitx::AddonInstance {
public:
    AinputAddon(fcitx::Instance *instance) : instance_(instance) {
        reloadConfig();
    }

    void reloadConfig() override {
        readAsIni(config_, configFile);
    }

    const fcitx::Configuration *getConfig() const override {
        return &config_;
    }

    void setConfig(const fcitx::RawConfig &config) override {
        config_.load(config, true);
        safeSaveAsIni(config_, configFile);
    }

private:
    static constexpr char configFile[] = "conf/ainput.conf";
    AinputConfig config_;
};
```

## 配置文件路径

配置文件保存在 `$XDG_CONFIG_HOME/fcitx5/conf/<addon_name>.conf`。

## Addon 注册文件中的配置标记

```ini
[Addon]
Configurable=True    # 启用配置 UI
```

设置为 `True` 后，`fcitx5-configtool` 会自动为你的 addon 生成配置界面。

## InputMethod 级别配置

输入法也可以有自己的配置。在 addon 类中覆写：

```cpp
const Configuration *getSubConfig(const std::string &path) const override {
    // 返回特定 input method 的配置
}

void setSubConfig(const std::string &path, const RawConfig &config) override {
    // 设置特定 input method 的配置
}
```

并在 inputmethod 注册文件中设置 `Configurable=True`。
