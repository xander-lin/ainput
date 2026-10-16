project('fcitx5-vinput', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++20', 'warning_level=3', 'werror=true'])

add_project_arguments('-Wno-variadic-macros', language: 'cpp')
add_project_arguments('-DVINPUT_DIAGNOSTICS_ENABLED=' +
  (get_option('diagnostic_logging') ? '1' : '0'), language: 'cpp')

subdir('ASR_provider/src')
subdir('tests')
subdir('tools')
subdir('adapter')

install_man('docs/vinput.1')

# CapsLock restore needs /dev/uinput (see udev/70-vinput-uinput.rules).
# TAG+="uaccess" lets systemd-logind grant the node to any local session,
# so no per-user group setup is required.
install_data('udev/70-vinput-uinput.rules',
  install_dir: 'lib/udev/rules.d')
