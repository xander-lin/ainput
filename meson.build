project('fcitx5-ainput', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++20', 'warning_level=3', 'werror=true'])

# The implementation lives in two places:
#   rust/   - capture, DSP, ASR backends, config files, uinput (Rust)
#   shell/  - fcitx5 addon class, configuration UI, event loop (C++)
# meson builds the Rust static archive with cargo and links it into the addon.

add_project_arguments('-Wno-variadic-macros', language: 'cpp')

# Cross builds against a sysroot tripped -Wpedantic inside glibc's own headers
# (bits/fcntl-linux.h declares `unsigned char f_handle[0]`). That is a property
# of the target system headers, not of our code, and with -Werror it is fatal.
# Only disable it for cross builds so native ones keep the stricter check.
if meson.is_cross_build()
  add_project_arguments('-Wno-pedantic', language: 'cpp')
endif

# Under a cross build the Rust archive has to be built for the target too,
# otherwise the linker gets host objects ("file in wrong format").
cpu_family = host_machine.cpu_family()
rust_target = ''
if cpu_family == 'aarch64'
  rust_target = 'aarch64-unknown-linux-gnu'
elif cpu_family == 'x86_64'
  rust_target = 'x86_64-unknown-linux-gnu'
endif

cargo = find_program('cargo', required: true)

rust_profile = get_option('buildtype') == 'debug' ? 'debug' : 'release'
rust_features = get_option('diagnostic_logging') ? 'diagnostics' : ''

rust_core = custom_target('ainput_core_rust',
  output: 'libainput_core.a',
  command: [
    find_program('rust/build.sh'),
    meson.project_source_root(),
    '@OUTPUT@',
    cargo.full_path(),
    rust_profile,
    rust_features,
    meson.current_build_dir() / 'rust-target',
  ],
  env: rust_target == '' ? {} : {'CARGO_BUILD_TARGET': rust_target},
  build_by_default: true,
  # cargo does its own change detection; marking the target stale keeps meson
  # from needing a hand-maintained list of every Rust source file.
  build_always_stale: true,
  install: false,
)

fcitx5_core = dependency('Fcitx5Core')
fcitx5_config = dependency('Fcitx5Config')
fcitx5_module = dependency('Fcitx5Module')

# Libraries the Rust archive references; they must be on the final link line.
pulse = dependency('libpulse')
curl = dependency('libcurl')
ebur128 = dependency('libebur128')
speexdsp = dependency('speexdsp')
soxr = dependency('soxr')

ainput = shared_library('ainput',
  ['shell/ainput.cpp', 'shell/output_handler.cpp'],
  rust_core,
  dependencies: [
    fcitx5_core, fcitx5_config, fcitx5_module,
    pulse, curl, ebur128, speexdsp, soxr,
  ],
  name_prefix: '',
  install: true,
  install_dir: fcitx5_core.get_variable(pkgconfig: 'libdir') / 'fcitx5')

addon_conf = configure_file(
  input: 'shell/ainput-addon.conf.in',
  output: 'ainput.conf',
  configuration: {'PROJECT_VERSION': meson.project_version()},
  install: true,
  install_dir: get_option('datadir') / 'fcitx5' / 'addon')

install_man('docs/ainput.1')

# CapsLock restore needs /dev/uinput (see udev/70-ainput-uinput.rules).
# TAG+="uaccess" lets systemd-logind grant the node to any local session,
# so no per-user group setup is required.
install_data('udev/70-ainput-uinput.rules',
  install_dir: 'lib/udev/rules.d')

# `meson test` runs the Rust unit tests; they need no desktop, microphone,
# cloud key or model files.
test('rust-unit',
  find_program('rust/run-tests.sh'),
  args: [meson.project_source_root(), rust_features],
  timeout: 300)
