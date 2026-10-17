asr_pulse_simple = dependency('libpulse-simple')
asr_pulse = dependency('libpulse')
asr_ebur128 = dependency('libebur128')
asr_libcurl = dependency('libcurl')
asr_soxr = dependency('soxr')
asr_speexdsp = dependency('speexdsp')

asr_provider_lib = static_library('asr_provider',
  'asr_provider.h',
  'asr_provider.cpp',
  'audio_capture.h',
  'audio_capture.cpp',
  'buffer_detect.h',
  'buffer_detect.cpp',
  'mock_provider.h',
  'mock_provider.cpp',
  'zipformer_provider.h',
  'zipformer_provider.cpp',
  'fire_red_provider.h',
  'fire_red_provider.cpp',
  'doubao_provider.h',
  'doubao_provider.cpp',
  'qwen_provider.h',
  'qwen_provider.cpp',
  'qwen_json.h',
  'config_schema.h',
  'config_templates.h',
  'ainput_config.h',
  'sha256.h',
  'diagnostic_log.h',
  'diagnostic_log.cpp',
  dependencies: [asr_pulse_simple, asr_pulse, asr_ebur128, asr_libcurl, asr_soxr, asr_speexdsp],
  install: false)

asr_provider_dep = declare_dependency(
  include_directories: include_directories('.'),
  link_whole: asr_provider_lib,
  dependencies: [asr_pulse_simple, asr_pulse, asr_ebur128, asr_libcurl, asr_soxr, asr_speexdsp])
