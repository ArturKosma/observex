# observex

A Windows desktop app that reads the screen like a human, only much faster. It captures the screen as many times per
second as possible, recognises things like shapes, UI elements and buttons in real time, and reacts with mouse and
keyboard input. The end goal is an app that learns to play a game by watching, saves what it learned, and loads that
knowledge later to play on its own.

Speed comes first: the highest capture rate and the lowest reaction latency possible.

C++20, Direct3D 11, DXGI Desktop Duplication, [Dear ImGui](https://github.com/ocornut/imgui) (docking branch).

## Status

Screen capture works; recognising and reacting are next.

- **Capture**: every new image of the primary monitor, kept on the GPU. On a 240 Hz monitor it captures every refresh
  (about 239 images per second) and has the image 0.05 ms after the screen updates, with under 0.1 ms of CPU work
  per image.
- **Capture panel**: live preview of what observex sees, plus capture rate, latency, work per image and merged updates.
- **Log panel**: the app's own events, with level toggles and text search.
- The capture loop runs on its own top-priority thread with its own GPU device, so the UI can never slow it down.
  It reconnects by itself after fullscreen switches, UAC prompts and the lock screen.

## Build (Windows)

Needs Visual Studio 2022 with the "Desktop development with C++" workload, and CMake 3.25 or newer.
The first configure downloads Dear ImGui and doctest (pinned versions, checked by SHA256).

```bash
cmake --preset vs2022
```

```bash
cmake --build --preset release
```

```bash
ctest --preset release
```

```bash
./observex.exe
```

The Release build writes `observex.exe` straight into the project folder. Use `--preset debug` for a debug build
(in `build/vs2022/bin/Debug/`); it keeps a console window and uses the Direct3D debug layer when installed.
You can also open the folder in Visual Studio (it reads `CMakePresets.json`), or open `build/vs2022/observex.sln`
after configuring.

## Layout

| Path | What it holds |
| --- | --- |
| `src/core/` | Platform-free building blocks: log store, lock-free hand-over, statistics. Unit tested. |
| `src/engine/` | The real-time pipeline: GPU setup, screen capture, preview hand-over to the UI. No UI code. |
| `src/app/` | The window and UI: main window, Capture panel, Log panel. |
| `tests/` | doctest unit tests for `src/core`. |
| `cmake/Dependencies.cmake` | Third-party libraries and their pinned versions. |

## Next steps

- Recognise: find shapes, UI elements and buttons in each image on the GPU, reading back only the results.
- React: press keys and click the mouse in response.
- Knowledge: save what observex learned (what to look for, how to react) and load it on start.
