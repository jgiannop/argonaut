# argonaut_test(<name> [LIBRARY <lib>] [TIMEOUT <seconds>] [PRIVATE_BUS]
#               [ENV <VAR=value>...] [SOURCES <file>...])
#
# Builds tests/tst_<name>.cpp and registers it with CTest as <name>. Every
# test gets the safe environment below, whether or not it sets it itself:
# no display, never the trash folders of real drives, never the real keyring.
#   LIBRARY      what to link (default argonaut_core; argonaut_app for windows)
#   PRIVATE_BUS  run under dbus-run-session: tests that own a D-Bus name must
#                never do it on the desktop's bus
function(argonaut_test name)
    cmake_parse_arguments(ARG "PRIVATE_BUS" "LIBRARY;TIMEOUT" "ENV;SOURCES" ${ARGN})
    if(NOT ARG_LIBRARY)
        set(ARG_LIBRARY argonaut_core)
    endif()
    if(NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 120)
    endif()
    qt_add_executable(tst_${name} tst_${name}.cpp TestSupport.h ${ARG_SOURCES})
    target_link_libraries(tst_${name} PRIVATE ${ARG_LIBRARY} Qt6::Test)
    if(ARG_PRIVATE_BUS)
        add_test(NAME ${name} COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:tst_${name}>)
        list(APPEND ARG_ENV "ARGONAUT_PRIVATE_BUS=1")
    else()
        add_test(NAME ${name} COMMAND tst_${name})
    endif()
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ARGONAUT_TRASH_HOME_ONLY=1;ARGONAUT_KEYSTORE=file;ARGONAUT_EXTENSIONS_PATH=;ARGONAUT_THEMES_PATH=;${ARG_ENV}"
        TIMEOUT ${ARG_TIMEOUT}
    )
endfunction()

# The file engine.
argonaut_test(fileoperations TIMEOUT 300)
argonaut_test(naming)
if(LIBARCHIVE_FOUND)
    argonaut_test(archives TIMEOUT 300)
endif()

# Models and what they read.
argonaut_test(directorymodel TIMEOUT 300)
argonaut_test(diskusage)
argonaut_test(contenttype)
argonaut_test(contentsearch)
argonaut_test(thumbnailer)
argonaut_test(preview)
argonaut_test(mediainfo)
argonaut_test(git ENV GIT_CONFIG_NOSYSTEM=1)
if(TARGET argonaut-git-extension)
    argonaut_test(extensions ENV GIT_CONFIG_NOSYSTEM=1)
    target_compile_definitions(tst_extensions PRIVATE GIT_EXTENSION_FOLDER="${CMAKE_BINARY_DIR}/extensions"
        FAKE_EXTENSION="${CMAKE_CURRENT_SOURCE_DIR}/fake_extension.py")
    add_dependencies(tst_extensions argonaut-git-extension argonaut-desktop-extension argonaut-preview-extension argonaut-space-extension)
    if(TARGET argonaut-connect-extension)
        add_dependencies(tst_extensions argonaut-connect-extension)
    endif()
    if(TARGET argonaut-dashboard-extension)
        add_dependencies(tst_extensions argonaut-dashboard-extension)
    endif()
endif()

# Places, bookmarks, other programs.
argonaut_test(places)
argonaut_test(bookmarks)
argonaut_test(openwith)
argonaut_test(remote)
argonaut_test(defaultfm)
argonaut_test(desktopactions)

# Claude.
argonaut_test(claude)
argonaut_test(organizer SOURCES FakeApi.h)

# D-Bus: only ever on a private bus.
find_program(DBUS_RUN_SESSION dbus-run-session)
if(TARGET Qt6::DBus AND DBUS_RUN_SESSION)
    argonaut_test(fmservice PRIVATE_BUS)
    argonaut_test(filechooser PRIVATE_BUS)
endif()

# The real window, headless: what gets trashed, deleted, dropped or returned.
argonaut_test(mainwindow LIBRARY argonaut_app TIMEOUT 300)
# The terminal interface, drawn into a grid of cells (no terminal needed).
argonaut_test(tui LIBRARY argonaut_tui TIMEOUT 300)
target_compile_definitions(tst_tui PRIVATE FAKE_EXTENSION="${CMAKE_CURRENT_SOURCE_DIR}/fake_extension.py")
if(TARGET argonaut-tui)
    # The real program in a pseudo-terminal: the ncurses layer draws and lets go.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME tui_smoke COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tui_smoke.py $<TARGET_FILE:argonaut-tui>)
        set_tests_properties(tui_smoke PROPERTIES TIMEOUT 60)
    endif()
endif()
target_compile_definitions(tst_mainwindow PRIVATE FAKE_EXTENSION="${CMAKE_CURRENT_SOURCE_DIR}/fake_extension.py")
