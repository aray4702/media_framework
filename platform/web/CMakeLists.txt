# The web platform. Its MP4 demuxer is portable C++, built natively too so it is tested against
# the macOS demuxer; the rest builds only with Emscripten (see platform/web/README.md).
add_library(mf_web_demuxer STATIC src/mp4_demuxer.cpp)
target_link_libraries(mf_web_demuxer PUBLIC mf_core)

if(APPLE)
  add_executable(web_demuxer_tests tests/mp4_demuxer_tests.mm)
  target_link_libraries(web_demuxer_tests PRIVATE mf_web_demuxer mf_macos)
endif()
