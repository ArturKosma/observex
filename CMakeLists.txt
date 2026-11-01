cmake_minimum_required(VERSION 3.25)

project(observex
    VERSION 0.3.0
    DESCRIPTION "Reads the screen in real time and reacts like a human player"
    LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "observex is Windows-only: it uses DXGI Desktop Duplication and Direct3D 11.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(OBSERVEX_BUILD_TESTS "Build the observex unit tests" ON)

include(cmake/Dependencies.cmake)
include(cmake/TensorRT.cmake)

# Warning flags for our own targets only (third-party code is left alone).
add_library(observex_warnings INTERFACE)
if(MSVC)
    target_compile_options(observex_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(observex_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# Core: platform-free building blocks (lock-free hand-over, statistics, marks on images). Unit tested.
add_library(observex_core STATIC
    src/core/Marks.cpp)
target_include_directories(observex_core PUBLIC src)
target_link_libraries(observex_core PRIVATE observex_warnings)

# Engine: the real-time loop (screen capture on its own thread and GPU device), the recorder and the recogniser
# (a trained model on the GPU through TensorRT). No UI code.
add_library(observex_engine STATIC
    src/engine/Detector.cpp
    src/engine/Gpu.cpp
    src/engine/ImageChannel.cpp
    src/engine/ImageFile.cpp
    src/engine/Pipeline.cpp
    src/engine/Recognizer.cpp
    src/engine/Recorder.cpp
    src/engine/ScreenCapture.cpp)
target_compile_definitions(observex_engine PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(observex_engine
    PUBLIC observex_core d3d11 dxgi
    PRIVATE windowscodecs d3dcompiler observex_tensorrt observex_warnings)

# App: the window and the UI. The icon and fonts are built into the exe (see src/app/observex.rc.in).
set(OBSERVEX_SOURCE_DIR ${CMAKE_SOURCE_DIR})
configure_file(src/app/observex.rc.in ${CMAKE_BINARY_DIR}/observex.rc @ONLY)
add_executable(observex
    src/app/App.cpp
    src/app/Finder.cpp
    src/app/MainWindow.cpp
    src/app/Review.cpp
    src/app/ScreenImage.cpp
    src/app/ScreenPicker.cpp
    src/app/Settings.cpp
    src/app/Snapshot.cpp
    src/app/Theme.cpp
    src/app/Widgets.cpp
    src/app/main.cpp
    ${CMAKE_BINARY_DIR}/observex.rc)
target_link_libraries(observex PRIVATE observex_engine imgui dwmapi observex_warnings)

# Debug builds keep a console window for stderr; other configs are a plain windowed app.
# The Release build goes straight into the project folder, so C:\_ai\observex\observex.exe is always the newest.
set_target_properties(observex PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:observex>)
if(MSVC)
    # Windowed builds still start at main() instead of WinMain().
    target_link_options(observex PRIVATE $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>)
endif()
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT observex)

# Bench: measures the pipeline from a console, without the app window (see bench/Bench.cpp).
add_executable(observex_bench bench/Bench.cpp)
target_link_libraries(observex_bench PRIVATE observex_engine dwmapi observex_warnings)

if(OBSERVEX_BUILD_TESTS)
    enable_testing()
    add_executable(observex_tests
        tests/MarksTests.cpp
        tests/StatWindowTests.cpp
        tests/TripleBufferTests.cpp
        tests/test_main.cpp)
    target_link_libraries(observex_tests PRIVATE observex_core doctest::doctest observex_warnings)
    add_test(NAME observex_tests COMMAND observex_tests)
endif()
