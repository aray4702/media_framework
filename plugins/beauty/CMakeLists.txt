# The beauty effect plugin: a MODULE (loaded with dlopen, never linked). It needs only the plugin
# header, not the framework's libraries. Built into <build>/plugins, where loadEffectPlugins()
# looks when an app runs from the build tree.
add_library(mf_beauty MODULE beauty.mm)
target_include_directories(mf_beauty PRIVATE ${PROJECT_SOURCE_DIR}/platform/macos/include)
target_compile_options(mf_beauty PRIVATE -fobjc-arc)
target_link_libraries(mf_beauty PRIVATE "-framework Foundation" "-framework Metal")
set_target_properties(mf_beauty PROPERTIES
  OUTPUT_NAME beauty PREFIX "" SUFFIX ".dylib"
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
  CXX_VISIBILITY_PRESET hidden OBJCXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
