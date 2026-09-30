add_executable(mf_demo main.mm)
target_link_libraries(mf_demo PRIVATE mf_macos)
add_dependencies(mf_demo mf_beauty)  # the effect plugins it loads from the build tree
