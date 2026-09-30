add_library(mf_macos STATIC
  src/at_audio_decoder.cpp
  src/au_speaker.cpp
  src/avf_demuxer.mm
  src/avf_export_sink.mm
  src/effect_plugins.mm
  src/image_loader.mm
  src/mac_platform.mm
  src/metal_compositor.mm
  src/metal_display.mm
  src/vt_video_decoder.mm)
target_include_directories(mf_macos PUBLIC include)
# loadEffectPlugins() also looks here, so apps run from the build tree find the plugins built with them.
target_compile_definitions(mf_macos PRIVATE MF_BUILD_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins")
target_compile_options(mf_macos PUBLIC $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(mf_macos PUBLIC mf_core
  "-framework Foundation" "-framework AppKit" "-framework AVFoundation" "-framework CoreMedia"
  "-framework CoreVideo" "-framework VideoToolbox" "-framework AudioToolbox" "-framework CoreAudio"
  "-framework Metal" "-framework QuartzCore" "-framework CoreText" "-framework ImageIO" "-framework UniformTypeIdentifiers")

# Runs the real demuxer and decoders on a clip, without a window or audio output.
add_executable(macos_adapter_tests tests/adapter_tests.mm)
target_link_libraries(macos_adapter_tests PRIVATE mf_macos)
add_dependencies(macos_adapter_tests mf_beauty)  # its plugin tests load it
