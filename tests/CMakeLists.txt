qt_add_executable(tst_fileoperations tst_fileoperations.cpp)
target_link_libraries(tst_fileoperations PRIVATE kaftilous_core Qt6::Test)

add_test(NAME fileoperations COMMAND tst_fileoperations)
# Runs headless; the test sandboxes HOME and the trash itself.
set_tests_properties(fileoperations PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KAFTILOUS_TRASH_HOME_ONLY=1"
    TIMEOUT 300
)

qt_add_executable(tst_directorymodel tst_directorymodel.cpp)
target_link_libraries(tst_directorymodel PRIVATE kaftilous_core Qt6::Test)
add_test(NAME directorymodel COMMAND tst_directorymodel)
set_tests_properties(directorymodel PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KAFTILOUS_TRASH_HOME_ONLY=1"
    TIMEOUT 300
)

qt_add_executable(tst_bookmarks tst_bookmarks.cpp)
target_link_libraries(tst_bookmarks PRIVATE kaftilous_core Qt6::Test)
add_test(NAME bookmarks COMMAND tst_bookmarks)
set_tests_properties(bookmarks PROPERTIES TIMEOUT 60)
