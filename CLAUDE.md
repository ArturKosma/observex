# observex

## What it is

A Windows desktop app that reads the screen the way a human does, only much faster. It captures the screen as many
times per second as possible, picks out interesting things in real time (shapes, UI elements, buttons), and reacts by
pressing keys and clicking the mouse.

The end goal is an app that plays a game. It learns how to play while it watches, saves what it learned to disk, and
later loads that knowledge so it knows what to look for and how to react.

## Rules from the user (apply to every change)

1. **Speed is everything.** observex must be the fastest program possible at reading the screen (as many Hz as
   possible) and at reacting (as little latency as possible). Speed comes before every other concern. That is why it
   is C++.
2. **No diff pages for this project.** The global rule to generate diff pages after file changes does not apply here.
   The user does not read this project's code, so report what changed and how it behaves, not code.
3. **Keep an AAA program structure.** Use design patterns where they fit. Work spread over many prompts tends to drift
   into hacks. Whenever the overall design should change, propose the restructure (like a planned rewrite that removes
   hacks) instead of patching around it.
4. **Keep it ultra simple.** No deep abstraction layers. A simpler program is easier to iterate on. Rules 3 and 4
   together mean a clean, clear structure that stays shallow.

## Current state

Capture works. The pipeline grabs every new image of the primary monitor and the UI shows a live preview and speed
numbers. Measured on the user's PC (RTX 4090, 2560 x 1440 at 240 Hz, Release build): 239 images/s (every refresh),
0.05 ms from screen update to image, under 0.1 ms of CPU work per image including the preview copy.

Not built yet: recognise (find things in the image), react (mouse and keyboard), and the knowledge file (what to look
for and how to react, saved and loaded).

## Architecture

Two threads, each with its own Direct3D 11 device on the GPU that drives the primary monitor:

- **Pipeline thread** (`engine/Pipeline`, time-critical priority): `ScreenCapture` waits for a new screen image (DXGI
  Desktop Duplication; the image stays on the GPU), then recognise and react will run on it, then it copies the image
  into the `PreviewChannel` if the UI is showing it. Once a second it hands `PipelineStats` to the UI.
- **UI thread** (`app/App`, the main thread): `MainWindow` (Win32 window and swap chain), ImGui, and the panels.
  `CapturePanel` shows state, stats and the preview; `LogPanel` shows the log.

The two threads share data only through lock-free hand-overs, so the UI can never slow the pipeline:

- `TripleBuffer<T>` (core): newest-value hand-over for stats.
- `PreviewChannel` / `PreviewReader` (engine): three shared GPU textures rotated by a TripleBuffer; keyed mutexes
  order the two devices' GPU work.
- Atomics for state and the preview on/off flag.
- `LogStore` takes a lock, so the pipeline logs only rare events (start, interruptions, errors), never per image.

Lifecycle: the UI thread owns the pipeline. It starts it, stops it, and restarts it when the display mode changes.
Capture interruptions (fullscreen switch, UAC prompt, lock screen) are recovered inside the pipeline thread.

Planned next, inside `engine/`, called from the pipeline loop where `Pipeline::run` marks the spot: a recogniser (GPU
compute on the image, reading back only small results), a reactor (`SendInput`), and a knowledge file.

## What speed means in practice

- Measure, don't guess. Every stage reports its timing in the Capture panel. Only trust Release builds for numbers.
- The hot path (capture, recognise, react) does no per-frame heap allocation, no disk I/O, no string formatting, no
  exceptions, and never waits on a lock the UI can hold.
- Keep images on the GPU and read back only small results, never whole frames.
- The UI only shows what the pipeline produces. Anything only the UI needs (like the preview copy) must cost nothing
  when it is not shown.

## Build and test

Configuring, building, running the tests and launching the app here is fine.

- Configure: `cmake --preset vs2022` (Visual Studio 2022 generator, output in `build/vs2022`)
- Build: `cmake --build --preset debug` (or `release`)
- Test: `ctest --preset debug`
- Run: `observex.exe` in the project folder. The Release build writes it there (gitignored); use Release to judge
  speed. The Debug build stays in `build/vs2022/bin/Debug/`.

## Layout

- `src/core/`: platform-free building blocks (`LogStore`, `LogFilter`, `TripleBuffer`, `StatWindow`, `Text`). No
  Windows or GPU headers. Unit tested.
- `src/engine/`: the real-time pipeline (`Gpu`, `ScreenCapture`, `PreviewChannel`, `Pipeline`). Windows and
  Direct3D 11, no ImGui.
- `src/app/`: the window and UI (`App`, `MainWindow`, `CapturePanel`, `LogPanel`, `main`).
- `tests/`: doctest tests for `src/core`. Add tests with any change to core.
- `cmake/Dependencies.cmake`: Dear ImGui and doctest via FetchContent. Pin every dependency to a release and its
  SHA256.

## Conventions

- Namespace `observex`. Types `PascalCase`, functions and variables `camelCase`, members end in `_`, constants `kName`.
- Format with `.clang-format` (clang-format ships with Visual Studio under `VC/Tools/Llvm/bin`).
- New source files must be added to the target lists in `CMakeLists.txt`.
- Our targets build with `/W4 /permissive-`; keep them warning-free.
- Setup code reports errors by throwing (`check()` in `engine/Gpu.h`); the hot path returns status codes instead.
- ImGui and the UI's GPU device are used on the UI thread only. The pipeline never touches UI objects.
