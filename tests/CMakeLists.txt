qt_add_executable(tst_fileoperations tst_fileoperations.cpp)
target_link_libraries(tst_fileoperations PRIVATE kaftilous_fileops Qt6::Test)

add_test(NAME fileoperations COMMAND tst_fileoperations)
# Runs headless; the test sandboxes HOME and the trash itself.
set_tests_properties(fileoperations PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 300
)
