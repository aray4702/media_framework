add_library(mf_editor_document STATIC document.cpp)
target_link_libraries(mf_editor_document PUBLIC mf_core)

add_executable(mf_editor main.mm preview_overlay.mm sidebar_view.mm timeline_view.mm inspector_view.mm waveform.mm export_view.mm
               voice_recorder.mm)
target_link_libraries(mf_editor PRIVATE mf_editor_document mf_macos)
add_dependencies(mf_editor mf_beauty)  # the effect plugins it loads from the build tree
# No app bundle: the Info.plist (the microphone usage description) goes into the executable.
target_link_options(mf_editor PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
set_property(TARGET mf_editor APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist)

add_executable(editor_document_tests document_tests.cpp)
target_link_libraries(editor_document_tests PRIVATE mf_editor_document)
add_test(NAME editor_document_tests COMMAND editor_document_tests)
