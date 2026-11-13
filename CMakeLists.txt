cmake_minimum_required(VERSION 3.16)

project(kaftilous VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest) # adds the BUILD_TESTING option (ON by default)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Xml)
if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
endif()
qt_standard_project_setup()

# Non-GUI core shared by the app and the tests: the folder model and the
# file operations engine (copy/move/trash/undo, clipboard).
qt_add_library(kaftilous_core STATIC
    src/DirectoryModel.h
    src/DirectoryModel.cpp
    src/DiskUsageModel.cpp
    src/Preview.h
    src/Preview.cpp
    src/DefaultFileManager.h
    src/DefaultFileManager.cpp
    src/RecentFiles.h
    src/RecentFiles.cpp
    src/Starred.h
    src/Starred.cpp
    src/Thumbnailer.h
    src/Thumbnailer.cpp
    src/DiskUsageModel.h
    src/Format.h
    src/ContentType.h
    src/ContentType.cpp
    src/Trash.h
    src/Trash.cpp
    src/Bookmarks.h
    src/Bookmarks.cpp
    src/OpenWith.h
    src/OpenWith.cpp
    src/FileJob.h
    src/FileJob.cpp
    src/FileOperations.h
    src/FileOperations.cpp
)
target_include_directories(kaftilous_core PUBLIC src)
target_link_libraries(kaftilous_core PUBLIC Qt6::Widgets Qt6::Xml)

# "Show in folder" for other apps (org.freedesktop.FileManager1), Linux only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.4 REQUIRED COMPONENTS DBus)
    target_sources(kaftilous_core PRIVATE src/FileManagerService.h src/FileManagerService.cpp)
    target_link_libraries(kaftilous_core PUBLIC Qt6::DBus)
    target_compile_definitions(kaftilous_core PUBLIC KAFTILOUS_DBUS=1)
endif()

# Compress / Extract: needs libarchive (optional, like the terminal).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBARCHIVE IMPORTED_TARGET libarchive)
endif()
if(LIBARCHIVE_FOUND)
    target_sources(kaftilous_core PRIVATE src/FileJobArchive.cpp)
    target_compile_definitions(kaftilous_core PUBLIC KAFTILOUS_ARCHIVES=1)
    target_link_libraries(kaftilous_core PUBLIC PkgConfig::LIBARCHIVE)
    message(STATUS "Compress / Extract: enabled (libarchive ${LIBARCHIVE_VERSION})")
else()
    message(STATUS "Compress / Extract: disabled (install libarchive-dev to enable)")
endif()

qt_add_executable(kaftilous
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/Theme.h
    src/Theme.cpp
    src/HeaderBar.h
    src/HeaderBar.cpp
    src/PathBar.h
    src/PathBar.cpp
    src/PreferencesDialog.h
    src/PreferencesDialog.cpp
    src/FileFilterProxy.h
    src/FileFilterProxy.cpp
    src/FileItemDelegate.h
    src/FileItemDelegate.cpp
    src/OperationsPanel.h
    src/OperationsPanel.cpp
    src/Toast.h
    src/Toast.cpp
    src/ConflictDialog.h
    src/ConflictDialog.cpp
    src/OpenWithDialog.h
    src/OpenWithDialog.cpp
    src/PropertiesDialog.h
    src/CompressDialog.cpp
    src/QuickLook.cpp
    src/QuickLook.h
    src/UsageDelegate.cpp
    src/UsageDelegate.h
    src/CompressDialog.h
    src/PropertiesDialog.cpp
)

target_link_libraries(kaftilous PRIVATE kaftilous_core Qt6::Widgets)

# Embedded terminal: needs libvterm and a Unix pseudo-terminal (forkpty).
# Without them the app builds normally, just without the terminal panel.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(VTERM IMPORTED_TARGET vterm)
endif()
if(VTERM_FOUND AND UNIX)
    target_sources(kaftilous PRIVATE src/TerminalWidget.h src/TerminalWidget.cpp)
    target_compile_definitions(kaftilous PRIVATE KAFTILOUS_TERMINAL=1)
    target_link_libraries(kaftilous PRIVATE PkgConfig::VTERM)
    find_library(UTIL_LIBRARY util)
    if(UTIL_LIBRARY)
        target_link_libraries(kaftilous PRIVATE ${UTIL_LIBRARY})
    endif()
    message(STATUS "Embedded terminal: enabled (libvterm ${VTERM_VERSION})")
else()
    message(STATUS "Embedded terminal: disabled (install libvterm-dev to enable)")
endif()

# Windows: no console window. macOS: build a .app bundle.
set_target_properties(kaftilous PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

# ------------------------------------------------------------- install ---
# `cmake --install build/release` (the release preset installs into ~/.local):
# the program, its menu entry and icon. Uninstall: `cmake --build build/release
# --target uninstall` (run `kaftilous --restore-nautilus` first if it's the default).
include(GNUInstallDirs)
install(TARGETS kaftilous RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} BUNDLE DESTINATION .)
if(UNIX AND NOT APPLE)
    # Absolute path: D-Bus and the desktop start it without the user's PATH.
    set(KAFTILOUS_EXE "${CMAKE_INSTALL_FULL_BINDIR}/kaftilous")
    configure_file(data/kaftilous.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/kaftilous.desktop @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/kaftilous.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/kaftilous.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    # Let menus and "Open With" see the new entry right away.
    install(CODE [[
        find_program(UPDATE_DESKTOP_DATABASE update-desktop-database)
        if(UPDATE_DESKTOP_DATABASE AND "$ENV{DESTDIR}" STREQUAL "")
            execute_process(COMMAND ${UPDATE_DESKTOP_DATABASE} "${CMAKE_INSTALL_PREFIX}/share/applications")
        endif()
    ]])
endif()
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/install_manifest.txt
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Uninstall.cmake
    COMMENT "Removing the installed files")

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
