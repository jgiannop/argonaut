cmake_minimum_required(VERSION 3.16)

project(kaftilous VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable(kaftilous
    src/main.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

target_link_libraries(kaftilous PRIVATE Qt6::Widgets)

# Windows: no console window. macOS: build a .app bundle.
set_target_properties(kaftilous PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
