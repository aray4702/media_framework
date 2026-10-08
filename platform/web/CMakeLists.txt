# The web platform. Its MP4 demuxer and muxer are portable C++, built natively too so they are
# tested against the macOS demuxer; the rest builds only with Emscripten (see platform/web/README.md).
add_library(mf_web_mp4 STATIC src/mp4_demuxer.cpp src/mp4_muxer.cpp)
target_link_libraries(mf_web_mp4 PUBLIC mf_core)

if(APPLE)
  add_executable(web_demuxer_tests tests/mp4_demuxer_tests.mm)
  target_link_libraries(web_demuxer_tests PRIVATE mf_web_mp4 mf_macos)
  add_executable(web_muxer_tests tests/mp4_muxer_tests.mm)
  target_link_libraries(web_muxer_tests PRIVATE mf_web_mp4 mf_macos)
  add_executable(export_reference tests/export_reference.mm)
  target_link_libraries(export_reference PRIVATE mf_macos)
endif()
