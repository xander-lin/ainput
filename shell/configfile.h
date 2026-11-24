#pragma once

// The configuration file lists every option with its current value.
//
// fcitx5 leaves an option at its default out of the file: Configuration::save
// marks it as implicit and the INI writer emits it as a comment, "#Key=value".
// That suits options shown in the settings dialog, but most of Ainput's tuning
// is hidden from the dialog and can only be changed in the file. A user who
// opens it should see every field, its default, and the comment explaining it.

#include <string>

#include <fcitx-config/configuration.h>
#include <fcitx-config/rawconfig.h>

namespace ainput {

/// Path of the shared settings, relative to fcitx5's package configuration
/// directory (`~/.config/fcitx5`).
inline constexpr char kConfigFile[] = "conf/ainput.conf";

/// `config` as the file holds it: every option with its current value, each
/// under its description.
inline fcitx::RawConfig completeConfig(const fcitx::Configuration &config) {
    fcitx::RawConfig raw;
    config.save(raw);
    for (const auto &name : raw.subItems()) {
        raw.get(name)->setImplicit(false);
    }
    return raw;
}

/// Whether `onDisk` holds a value for every option of `config`. A commented
/// default ("#Key=value") is a comment to the reader, so it does not count,
/// and neither does a missing file.
inline bool listsEveryOption(const fcitx::RawConfig &onDisk,
                             const fcitx::Configuration &config) {
    fcitx::RawConfig all;
    config.save(all);
    for (const auto &name : all.subItems()) {
        if (!onDisk.get(name)) {
            return false;
        }
    }
    return true;
}

} // namespace ainput
