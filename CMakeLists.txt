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

# File operations engine (copy/move/trash/undo, clipboard), shared by the
# app and the tests.
qt_add_library(kaftilous_fileops STATIC
    src/FileJob.h
    src/FileJob.cpp
    src/FileOperations.h
    src/FileOperations.cpp
)
target_include_directories(kaftilous_fileops PUBLIC src)
target_link_libraries(kaftilous_fileops PUBLIC Qt6::Widgets)

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
    src/FileItemDelegate.h
    src/FileItemDelegate.cpp
    src/Thumbnailer.h
    src/Thumbnailer.cpp
    src/OperationsPanel.h
    src/OperationsPanel.cpp
    src/Toast.h
    src/Toast.cpp
)

target_link_libraries(kaftilous PRIVATE kaftilous_fileops Qt6::Widgets)

# Windows: no console window. macOS: build a .app bundle.
set_target_properties(kaftilous PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
