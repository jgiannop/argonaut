cmake_minimum_required(VERSION 3.16)

project(kaftilous VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest) # adds the BUILD_TESTING option (ON by default)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
endif()
qt_standard_project_setup()

# Non-GUI core shared by the app and the tests: the folder model and the
# file operations engine (copy/move/trash/undo, clipboard).
qt_add_library(kaftilous_core STATIC
    src/DirectoryModel.h
    src/DirectoryModel.cpp
    src/Trash.h
    src/Trash.cpp
    src/FileJob.h
    src/FileJob.cpp
    src/FileOperations.h
    src/FileOperations.cpp
)
target_include_directories(kaftilous_core PUBLIC src)
target_link_libraries(kaftilous_core PUBLIC Qt6::Widgets)

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
    src/Thumbnailer.h
    src/Thumbnailer.cpp
    src/OperationsPanel.h
    src/OperationsPanel.cpp
    src/Toast.h
    src/Toast.cpp
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

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
