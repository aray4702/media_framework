add_library(mf_editor_document STATIC document.cpp)
target_link_libraries(mf_editor_document PUBLIC mf_core)

add_executable(mf_editor main.mm preview_overlay.mm sidebar_view.mm timeline_view.mm inspector_view.mm waveform.mm)
target_link_libraries(mf_editor PRIVATE mf_editor_document mf_macos)

add_executable(editor_document_tests document_tests.cpp)
target_link_libraries(editor_document_tests PRIVATE mf_editor_document)
add_test(NAME editor_document_tests COMMAND editor_document_tests)
