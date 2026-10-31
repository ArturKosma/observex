# Third-party libraries. CMake downloads them on the first configure; each one is pinned
# to a release and checked against its SHA256.
include(FetchContent)

# Dear ImGui, docking branch: the UI. It has no CMake build of its own, so the library is defined below.
FetchContent_Declare(imgui
    URL      https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
    URL_HASH SHA256=90ded916bd57db2e0e171b6b098940a47c6f5042725dcdc67fb19940ca8bfdcc)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dwmapi)
set_target_properties(imgui PROPERTIES FOLDER "third_party")

# doctest: unit tests.
if(OBSERVEX_BUILD_TESTS)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(doctest
        URL      https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
    FetchContent_MakeAvailable(doctest)
    if(TARGET doctest_with_main)
        set_target_properties(doctest_with_main PROPERTIES FOLDER "third_party")
    endif()
endif()

# Inter, the UI font (SIL Open Font License 1.1), Latin subset from Fontsource. Built into observex.exe.
set(OBSERVEX_FONT_DIR ${CMAKE_BINARY_DIR}/fonts)
set(INTER_URL https://cdn.jsdelivr.net/fontsource/fonts/inter@5.2.8)
file(DOWNLOAD ${INTER_URL}/latin-400-normal.ttf ${OBSERVEX_FONT_DIR}/Inter-Regular.ttf TLS_VERIFY ON
    EXPECTED_HASH SHA256=27396a89e10048c8e76096b1b4a5500bdc54d3b04d200f267ed6bf0d5466b2ba)
file(DOWNLOAD ${INTER_URL}/latin-500-normal.ttf ${OBSERVEX_FONT_DIR}/Inter-Medium.ttf TLS_VERIFY ON
    EXPECTED_HASH SHA256=3073f87cb6eac0ac47fee30af580ba12f4bedeb62d1e1254c07dca7b2ad607e7)
