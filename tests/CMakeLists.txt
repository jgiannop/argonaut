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

qt_add_executable(tst_openwith tst_openwith.cpp)
target_link_libraries(tst_openwith PRIVATE kaftilous_core Qt6::Test)
add_test(NAME openwith COMMAND tst_openwith)
set_tests_properties(openwith PROPERTIES TIMEOUT 60)

qt_add_executable(tst_contenttype tst_contenttype.cpp)
target_link_libraries(tst_contenttype PRIVATE kaftilous_core Qt6::Test)
add_test(NAME contenttype COMMAND tst_contenttype)
set_tests_properties(contenttype PROPERTIES TIMEOUT 60)

if(LIBARCHIVE_FOUND)
    qt_add_executable(tst_archives tst_archives.cpp)
    target_link_libraries(tst_archives PRIVATE kaftilous_core Qt6::Test)
    add_test(NAME archives COMMAND tst_archives)
    set_tests_properties(archives PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KAFTILOUS_TRASH_HOME_ONLY=1"
        TIMEOUT 300
    )
endif()

qt_add_executable(tst_diskusage tst_diskusage.cpp)
target_link_libraries(tst_diskusage PRIVATE kaftilous_core Qt6::Test)
add_test(NAME diskusage COMMAND tst_diskusage)
set_tests_properties(diskusage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)

qt_add_executable(tst_thumbnailer tst_thumbnailer.cpp)
target_link_libraries(tst_thumbnailer PRIVATE kaftilous_core Qt6::Test)
add_test(NAME thumbnailer COMMAND tst_thumbnailer)
set_tests_properties(thumbnailer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
