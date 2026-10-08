cmake_minimum_required(VERSION 3.20)

set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0 CACHE STRING "Minimum Mac OS version")
project(media_framework LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Portable core: no platform headers.
add_library(mf_core STATIC
  core/src/audio_ring.cpp
  core/src/av_sync.cpp
  core/src/composition.cpp
  core/src/cooperative_scheduler.cpp
  core/src/decode_control.cpp
  core/src/decode_rate.cpp
  core/src/effects.cpp
  core/src/export_driver.cpp
  core/src/exporter.cpp
  core/src/json.cpp
  core/src/layout.cpp
  core/src/leading_clip_driver.cpp
  core/src/live_preview.cpp
  core/src/master_clock.cpp
  core/src/metrics.cpp
  core/src/pipeline.cpp
  core/src/player.cpp
  core/src/scene.cpp
  core/src/segment_recorder.cpp
  core/src/thread_scheduler.cpp
  core/src/types.cpp
  core/src/vsync_driver.cpp)
target_include_directories(mf_core PUBLIC core/include)
find_package(Threads REQUIRED)
target_link_libraries(mf_core PUBLIC Threads::Threads)

enable_testing()
add_executable(core_tests core/tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE mf_core)
add_test(NAME core_tests COMMAND core_tests)

if(APPLE)
  enable_language(OBJCXX)
  add_subdirectory(platform/macos)
  add_subdirectory(platform/web)
  add_subdirectory(plugins/beauty)
  add_subdirectory(apps/macos-demo)
  add_subdirectory(apps/macos-editor)
endif()
