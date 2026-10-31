# observex

Desktop observability tool (live log viewer for now). C++20, Dear ImGui docking branch, GLFW, OpenGL 3, CMake.

## Build and test

Configuring, building and running the tests here is fine.

- Configure: `cmake --preset vs2022` (Visual Studio 2022 generator, output in `build/vs2022`)
- Build: `cmake --build --preset debug` (or `release`)
- Test: `ctest --preset debug`
- Run: `build/vs2022/bin/Debug/observex.exe`

## Layout

- `src/core/`: data model and sources. Must not include ImGui, GLFW or OpenGL headers, so it stays testable.
- `src/app/`: window, ImGui setup and panels.
- `tests/`: doctest tests for `src/core`. Add tests with any change to core.
- `cmake/Dependencies.cmake`: third-party code via FetchContent. Pin every dependency to a release and its SHA256.

## Conventions

- Namespace `observex`. Types `PascalCase`, functions and variables `camelCase`, members end in `_`, constants `kName`.
- Format with `.clang-format` (clang-format ships with Visual Studio under `VC/Tools/Llvm/bin`).
- New source files must be added to the target lists in `CMakeLists.txt`.
- Our targets build with `/W4 /permissive-`; keep them warning-free.
- Code that touches ImGui or OpenGL runs on the main thread only. Other threads talk to the UI through `LogStore`.
