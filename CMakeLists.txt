cmake_minimum_required(VERSION 3.25)

project(observex
    VERSION 0.2.0
    DESCRIPTION "Reads the screen in real time and reacts like a human player"
    LANGUAGES CXX)

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

# Warning flags for our own targets only (third-party code is left alone).
add_library(observex_warnings INTERFACE)
if(MSVC)
    target_compile_options(observex_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(observex_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# Core: platform-free building blocks (log store, lock-free hand-over, statistics). Unit tested.
add_library(observex_core STATIC
    src/core/LogEntry.cpp
    src/core/LogFilter.cpp
    src/core/LogStore.cpp)
target_include_directories(observex_core PUBLIC src)
target_link_libraries(observex_core PRIVATE observex_warnings)

# Engine: the real-time loop (screen capture on its own thread and GPU device). No UI code.
add_library(observex_engine STATIC
    src/engine/Gpu.cpp
    src/engine/Pipeline.cpp
    src/engine/PreviewChannel.cpp
    src/engine/ScreenCapture.cpp)
target_compile_definitions(observex_engine PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(observex_engine
    PUBLIC observex_core d3d11 dxgi
    PRIVATE observex_warnings)

# App: the window and its ImGui panels.
add_executable(observex
    src/app/App.cpp
    src/app/CapturePanel.cpp
    src/app/LogPanel.cpp
    src/app/MainWindow.cpp
    src/app/main.cpp)
target_link_libraries(observex PRIVATE observex_engine imgui observex_warnings)
target_compile_definitions(observex PRIVATE OBSERVEX_VERSION="${PROJECT_VERSION}")

# Debug builds keep a console window for stderr; other configs are a plain windowed app.
set_target_properties(observex PROPERTIES
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:observex>)
if(MSVC)
    # Windowed builds still start at main() instead of WinMain().
    target_link_options(observex PRIVATE $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>)
endif()
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT observex)

if(OBSERVEX_BUILD_TESTS)
    enable_testing()
    add_executable(observex_tests
        tests/LogEntryTests.cpp
        tests/LogFilterTests.cpp
        tests/LogStoreTests.cpp
        tests/StatWindowTests.cpp
        tests/TextTests.cpp
        tests/TripleBufferTests.cpp
        tests/test_main.cpp)
    target_link_libraries(observex_tests PRIVATE observex_core doctest::doctest observex_warnings)
    add_test(NAME observex_tests COMMAND observex_tests)
endif()
