# observex

A desktop observability tool, starting as a live, filterable log viewer.
C++20, [Dear ImGui](https://github.com/ocornut/imgui) (docking branch), GLFW and OpenGL 3.

## Status

Early scaffold. What works today:

- **Log store**: thread-safe and bounded (200,000 entries by default). Any thread can write to it.
- **Demo source**: a background thread writing fake entries (1 to 5,000 per second), so there is live data before real sources exist.
- **Log panel**: level toggles with counts, text search over source and message, auto-scroll, and a table that stays fast with many entries.
- Dockable panels that can be dragged out into their own windows. The layout is saved to `imgui.ini` next to the executable.

## Build (Windows)

Needs Visual Studio 2022 with the "Desktop development with C++" workload, and CMake 3.25 or newer.
The first configure downloads GLFW, Dear ImGui and doctest (pinned versions, checked by SHA256).

```bash
cmake --preset vs2022
```

```bash
cmake --build --preset debug
```

```bash
ctest --preset debug
```

```bash
build/vs2022/bin/Debug/observex.exe
```

Use `--preset release` for an optimised build. You can also open the folder in Visual Studio (it reads `CMakePresets.json`), or open `build/vs2022/observex.sln` after configuring.

Debug builds keep a console window for error output. Release builds are a plain windowed app.

## Layout

| Path | What it holds |
| --- | --- |
| `src/core/` | Data model and sources: `LogEntry`, `LogStore`, `LogFilter`, `DemoSource`. No UI code. |
| `src/app/` | The desktop app: window and ImGui setup (`App`), the Log panel (`LogPanel`). |
| `tests/` | doctest unit tests for `src/core`. |
| `cmake/Dependencies.cmake` | Third-party libraries and their pinned versions. |

## Ideas for next steps

- Real sources: tail a log file, listen on UDP/TCP, read Unreal Engine log output.
- Select rows and copy them.
- Charts of entries per second by level (ImPlot).
- Save and reopen captured sessions.
