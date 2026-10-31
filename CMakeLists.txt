cmake_minimum_required(VERSION 3.25)

project(observex
    VERSION 0.1.0
    DESCRIPTION "Desktop observability and log viewer"
    LANGUAGES C CXX)

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

# Core: data model and log sources. No UI or windowing code, so it is easy to test.
find_package(Threads REQUIRED)
add_library(observex_core STATIC
    src/core/DemoSource.cpp
    src/core/LogEntry.cpp
    src/core/LogFilter.cpp
    src/core/LogStore.cpp)
target_include_directories(observex_core PUBLIC src)
target_link_libraries(observex_core
    PUBLIC Threads::Threads
    PRIVATE observex_warnings)

# App: the desktop window and its ImGui panels.
add_executable(observex
    src/app/App.cpp
    src/app/LogPanel.cpp
    src/app/main.cpp)
target_link_libraries(observex PRIVATE observex_core imgui observex_warnings)
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
        tests/DemoSourceTests.cpp
        tests/LogEntryTests.cpp
        tests/LogFilterTests.cpp
        tests/LogStoreTests.cpp
        tests/TextTests.cpp
        tests/test_main.cpp)
    target_link_libraries(observex_tests PRIVATE observex_core doctest::doctest observex_warnings)
    add_test(NAME observex_tests COMMAND observex_tests)
endif()
