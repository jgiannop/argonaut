qt_add_executable(tst_fileoperations tst_fileoperations.cpp)
target_link_libraries(tst_fileoperations PRIVATE kosmos_core Qt6::Test)

add_test(NAME fileoperations COMMAND tst_fileoperations)
# Runs headless; the test sandboxes HOME and the trash itself.
set_tests_properties(fileoperations PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KOSMOS_TRASH_HOME_ONLY=1"
    TIMEOUT 300
)

qt_add_executable(tst_directorymodel tst_directorymodel.cpp)
target_link_libraries(tst_directorymodel PRIVATE kosmos_core Qt6::Test)
add_test(NAME directorymodel COMMAND tst_directorymodel)
set_tests_properties(directorymodel PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KOSMOS_TRASH_HOME_ONLY=1"
    TIMEOUT 300
)

qt_add_executable(tst_bookmarks tst_bookmarks.cpp)
target_link_libraries(tst_bookmarks PRIVATE kosmos_core Qt6::Test)
add_test(NAME bookmarks COMMAND tst_bookmarks)
set_tests_properties(bookmarks PROPERTIES TIMEOUT 60)

qt_add_executable(tst_openwith tst_openwith.cpp)
target_link_libraries(tst_openwith PRIVATE kosmos_core Qt6::Test)
add_test(NAME openwith COMMAND tst_openwith)
set_tests_properties(openwith PROPERTIES TIMEOUT 60)

qt_add_executable(tst_contenttype tst_contenttype.cpp)
target_link_libraries(tst_contenttype PRIVATE kosmos_core Qt6::Test)
add_test(NAME contenttype COMMAND tst_contenttype)
set_tests_properties(contenttype PROPERTIES TIMEOUT 60)

if(LIBARCHIVE_FOUND)
    qt_add_executable(tst_archives tst_archives.cpp)
    target_link_libraries(tst_archives PRIVATE kosmos_core Qt6::Test)
    add_test(NAME archives COMMAND tst_archives)
    set_tests_properties(archives PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KOSMOS_TRASH_HOME_ONLY=1"
        TIMEOUT 300
    )
endif()

qt_add_executable(tst_diskusage tst_diskusage.cpp)
target_link_libraries(tst_diskusage PRIVATE kosmos_core Qt6::Test)
add_test(NAME diskusage COMMAND tst_diskusage)
set_tests_properties(diskusage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)

qt_add_executable(tst_thumbnailer tst_thumbnailer.cpp)
target_link_libraries(tst_thumbnailer PRIVATE kosmos_core Qt6::Test)
add_test(NAME thumbnailer COMMAND tst_thumbnailer)
set_tests_properties(thumbnailer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)

qt_add_executable(tst_places tst_places.cpp)
target_link_libraries(tst_places PRIVATE kosmos_core Qt6::Test)
add_test(NAME places COMMAND tst_places)
set_tests_properties(places PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

qt_add_executable(tst_defaultfm tst_defaultfm.cpp)
target_link_libraries(tst_defaultfm PRIVATE kosmos_core Qt6::Test)
add_test(NAME defaultfm COMMAND tst_defaultfm)
set_tests_properties(defaultfm PROPERTIES TIMEOUT 60)

# D-Bus: only ever on a private bus.
find_program(DBUS_RUN_SESSION dbus-run-session)
if(TARGET Qt6::DBus AND DBUS_RUN_SESSION)
    qt_add_executable(tst_fmservice tst_fmservice.cpp)
    target_link_libraries(tst_fmservice PRIVATE kosmos_core Qt6::Test)
    add_test(NAME fmservice COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:tst_fmservice>)
    set_tests_properties(fmservice PROPERTIES ENVIRONMENT "KOSMOS_PRIVATE_BUS=1" TIMEOUT 60)
endif()

qt_add_executable(tst_preview tst_preview.cpp)
target_link_libraries(tst_preview PRIVATE kosmos_core Qt6::Test)
add_test(NAME preview COMMAND tst_preview)
set_tests_properties(preview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)

qt_add_executable(tst_naming tst_naming.cpp)
target_link_libraries(tst_naming PRIVATE kosmos_core Qt6::Test)
add_test(NAME naming COMMAND tst_naming)
set_tests_properties(naming PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KOSMOS_TRASH_HOME_ONLY=1"
    TIMEOUT 60
)

qt_add_executable(tst_git tst_git.cpp)
target_link_libraries(tst_git PRIVATE kosmos_core Qt6::Test)
add_test(NAME git COMMAND tst_git)
set_tests_properties(git PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;KOSMOS_TRASH_HOME_ONLY=1;GIT_CONFIG_NOSYSTEM=1"
    TIMEOUT 120
)
