add_library(mf_macos STATIC
  src/at_audio_decoder.cpp
  src/au_speaker.cpp
  src/avf_demuxer.mm
  src/mac_platform.mm
  src/metal_display.mm
  src/vt_video_decoder.mm)
target_include_directories(mf_macos PUBLIC include)
target_compile_options(mf_macos PUBLIC $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(mf_macos PUBLIC mf_core
  "-framework Foundation" "-framework AppKit" "-framework AVFoundation" "-framework CoreMedia"
  "-framework CoreVideo" "-framework VideoToolbox" "-framework AudioToolbox" "-framework CoreAudio"
  "-framework Metal" "-framework QuartzCore" "-framework UniformTypeIdentifiers")

# Runs the real demuxer and decoders on a clip, without a window or audio output.
add_executable(macos_adapter_tests tests/adapter_tests.mm)
target_link_libraries(macos_adapter_tests PRIVATE mf_macos)
