# observex

<img src="assets/logo.svg" width="96" alt="observex logo">

A Windows desktop app that reads the screen like a human, only much faster. It captures the screen as many times per
second as possible, recognises things like shapes, UI elements and buttons in real time, and reacts with mouse and
keyboard input. The end goal is an app that learns to play a game by watching, saves what it learned, and loads that
knowledge later to play on its own.

Speed comes first: the highest capture rate and the lowest reaction latency possible.

C++20, Direct3D 11, DXGI Desktop Duplication, CUDA and TensorRT, [Dear ImGui](https://github.com/ocornut/imgui)
(docking branch).

## Status

Screen capture, recording, reviewing, training and recognising monsters live work; learning while observing (see
[the training loop](docs/training-loop.md)) is next.

- **Screen button**: shows which screen observex is watching ("Observing Odyssey G61SH") and opens live miniatures
  of every screen, arranged as on your desktop and labelled with each monitor's name. Click one and observex watches
  it; the window shows that screen live. F3 shows the capture and recognition numbers.
- **Live recognition**: while a screen is observed, the trained monster model runs on every new image on the GPU
  (about 5 ms each on an RTX 4090, about 210 images a second), on its own thread, so capture never waits for it.
  **Show** (the eye, next to the screen button) holds flags like Unreal's view flags: **Monsters** draws a box with
  how sure the model is ("Monster 81%") around each monster it finds. Turning a flag off only hides the boxes;
  recognition keeps running. It pauses only while you review, so the review gets the GPU. The first time observex sees a new model, it prepares it for your GPU (about 35 s,
  with progress under the flag) and keeps the result for the next starts.
- **Record**: saves the observed screen as numbered JPEG images, one every 3 seconds, into a new folder per recording
  (`recordings/` next to observex.exe unless you pick another folder in Settings). Ctrl+Alt+R starts and stops it
  even while a game is in front. A 2560 x 1440 game image is about 0.7 MB, so expect about 0.85 GB per hour.
  While recording, the button shows the time, the number of images and how much disk space they take. Recording
  does not slow capture.
- **Review**: open a recording, choose what to look for (Monsters for now), and step through its images. Each thing
  observex marked shows as a box with its guess ("Monster? 81%"); the one you are answering has a white ring and the
  others fade back, so it is clear even among close monsters. Answer it with Yes / No (or the Y / N keys), and
  drag a box around anything it missed. What the model is at least 75% sure of starts as yes ("Monster 92%",
  green; it was right 97% of the time), so you only answer the unsure ones and flip the rare wrong one. Moving on
  with the Right arrow saves the image's answers, and answers carry over to the same thing on the next image.
  Answers are saved next to the images, in `monster-answers.txt`. Once a
  model is trained, opening a new recording runs it in the background, inside observex on the GPU ("Looking for
  monsters with the trained model", with progress; about 1 s to start, then about 20 ms per image), and its guesses
  appear when it is done. Before the first training there are no guesses, so you draw the boxes yourself.
- **Train**: `tools/train/` (Python, on the GPU) learns from your answers, writes the model live recognition uses,
  and writes its guesses for the next recording, which the review then shows. See
  [tools/train/README.md](tools/train/README.md).
- **Capture**: every new image of the observed screen, kept on the GPU. On a 240 Hz monitor it captures every refresh
  (about 240 images per second) and has the image 0.05 ms after the screen updates, with about 0.1 ms of CPU work
  per image.
- The capture loop runs on its own top-priority thread with its own GPU device, so the UI can never slow it down.
  It reconnects by itself after fullscreen switches, UAC prompts and the lock screen.

## Build (Windows)

Needs Visual Studio 2022 with the "Desktop development with C++" workload, and CMake 3.25 or newer.
The first configure downloads Dear ImGui, doctest and the Inter font, and, into `deps/`, NVIDIA TensorRT 11.4 and the
CUDA 13.3 runtime (about 2 GB, once; from NVIDIA's public pip server, GitHub and CUDA download site). All pinned
versions, checked by SHA256. Live recognition needs an NVIDIA GPU with a recent driver (CUDA 13).

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

## Benchmark

`observex_bench` measures the capture pipeline from a console, without opening the app window:

```bash
build/vs2022/bin/Release/observex_bench.exe --seconds 5
```

It lists the screens, captures one (`--screen N`, the primary by default) and prints images per second, merged
updates, latency and work per image once a second. To give capture something to do, it presents an invisible 2 x 2
pixel window in the screen's corner every refresh, like a game would (`--no-pulse` turns that off). `--no-preview`
leaves out the copy the UI makes to show the screen. `--record FOLDER` also saves images like the Record button
(`--every S`, one every 3 seconds like the app), to measure what recording costs. `--model models/monster` runs live
recognition alongside and prints its speed and how many monsters it found.

## UI snapshots

To check how the UI looks without the window appearing or taking focus, snapshot mode draws it in a hidden window,
plays some input, and saves a PNG:

```bash
./observex.exe --snapshot ui.png --click 1020 540 --click 120 33 --mouse 300 200
```

Each step (`--click X Y`, `--drag X1 Y1 X2 Y2` or `--key NAME`, in window pixels and ImGui key names) happens about
0.6 s after the one before, and `--mouse X Y` is where the mouse rests for the picture. `--review FOLDER` is what the
Review button opens, since snapshot mode never shows a Windows dialog. `--wait SECONDS` waits that much longer
before the picture (e.g. for the trained model's guesses). The snapshot shows your real screens.

## Layout

| Path | What it holds |
| --- | --- |
| `src/core/` | Platform-free building blocks: lock-free hand-over, statistics, marks and answers. Unit tested. |
| `src/engine/` | The real-time pipeline: GPU setup, screen capture, image hand-over, the recorder, the recogniser. No UI code. |
| `src/app/` | The window and UI: main window, theme, buttons, screen picker, screen view, review, settings, UI snapshots. |
| `bench/` | The headless capture benchmark. |
| `assets/` | Logo and app icon, made by `tools/make_logo.py`. |
| `tools/train/` | The training tool (Python): learns from review answers, writes guesses. |
| `tests/` | doctest unit tests for `src/core`. |
| `docs/` | Longer plans, such as [the training loop](docs/training-loop.md). |
| `cmake/` | Third-party libraries and their pinned versions (`TensorRT.cmake`: TensorRT and CUDA into `deps/`). |

## Next steps

- Learn to recognise: record play, review what observex found, train, repeat, until the score is good enough.
  First target: monsters in Lineage 2. The plan is in [docs/training-loop.md](docs/training-loop.md).
- Learn while observing: save a question whenever the model is unsure, and let answers update the running model
  without leaving the app.
- Track: follow each monster from image to image.
- React: press keys and click the mouse in response.
- Knowledge: save what observex learned (what to look for, how to react) and load it on start.

## Credits

The UI font is [Inter](https://rsms.me/inter/) by Rasmus Andersson, under the SIL Open Font License 1.1.
