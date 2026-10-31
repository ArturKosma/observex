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
5. **Never pop the app up on the user's screen for testing.** Do not launch `observex.exe`, and do not drive its
   window with the mouse or keyboard, to check things: it steals focus while the user works. Measure with
   `observex_bench` (console only) and unit tests, and check how the UI looks with snapshot mode
   (`observex.exe --snapshot`, which never shows the window). When something needs the real window, ask the user
   to look.

## Current state

The UI is deliberately minimal: a top bar with the screen button on the left and Review, Record and Settings on the
right.
The screen button looks like a dropdown (monitor icon, text, arrow) and reads "Choose a screen", or "Observing" and
the observed monitor's name. It opens live miniatures of every
screen, laid out the way the screens are arranged on the desktop, each labelled with its monitor's model name (from
the monitor itself, e.g. "Odyssey G61SH"; size and refresh rate show on hover). Clicking one starts observing it, and
the window shows only that screen, live. With nothing observed yet, the miniatures fill the window under the logo.
F3 shows the pipeline's numbers over the view (hidden by default).

Recording (step 1 of the training loop): **Record** (or Ctrl+Alt+R, a system-wide hotkey that works while the game
has focus or observex is minimized) saves the observed screen into a new folder named by date and time, e.g.
`recordings/2026-10-31_23-05-12/000001.jpg`, 1 image per second (fixed, `kRecordingRate` in App.cpp: the user asked
for no rate settings; neighbouring images would be near copies). Only images the screen actually updated are saved.
While recording, the button reads "Stop", the time and the number of images. JPEG quality 85: about 0.7 MB per
2560 x 1440 Lineage 2 image, so about 2.6 GB per hour. **Settings** (gear) holds only the recordings folder (default:
`recordings` next to observex.exe; folder picker, Open, Use default). Settings live in `observex.cfg` next to the
exe. Both it and `recordings/` are gitignored.
Recording costs capture nothing measurable (bench: 238.7 and 239.3 images/s recording, 238.2 and 239.2 not).

Review (step 2 of the training loop, `app/Review`): **Review** asks for a recording's folder (Windows folder picker),
then "What are we looking for?" (categories in `kCategories`; only Monsters for now), then shows the recording's
first image not reviewed yet, filling the view. The finder's guesses are drawn as marks with a tag ("Monster? 81%");
the selected mark gets Yes / No buttons under it (Y / N keys; answering moves to the next unanswered mark). Dragging
on the image draws a missed one (yes; Remove / Del deletes it). The bar's middle shows back / "Image 5 of 340 · 11
reviewed" / forward (Left / Right keys). Moving forward makes the image "reviewed": unanswered marks become no and
the answers are saved. Answers carry forward: a guess on the next image overlapping an answered mark by at least
half (`carryAnswers`) gets its answer, tagged "copied". Review shows every image, in order.
Files, in the recording's folder, in the text format of `core/Marks.h`: `<category>-guesses.txt` (from the finder,
read only) and `<category>-answers.txt` (written by review; only reviewed images, rewritten through a temporary file
on every change). No finder exists yet, so for now the review starts empty and says so. While reviewing, the
pipeline's preview is off and keyboard navigation of buttons is off (the review owns the arrows, Tab and letters).
The Inter font has no arrow glyphs: write "Left / Right", not arrow characters. The Review button's icon is a
picture (a tick looked like "confirm something", the user said).

Look: Inter font (regular and medium, built into the exe), a dark neutral palette with one violet accent
(`#8B7BFF`), rounded corners, a dark title bar in the same colours, and the observex logo (a ring with a pupil
glancing up and right, on a dark rounded tile) as the app icon.

Measured with `observex_bench` (RTX 4090, 2560 x 1440 at 240 Hz, Release, a game-like source presenting every
refresh, with the UI's preview copy included): 239.7 images/s (every refresh), 0.05 ms from screen update to image,
about 0.1 ms of CPU work per image.

Not built yet: recognise (find things in the image), react (mouse and keyboard), and the knowledge file (what to look
for and how to react, saved and loaded).

**Next: the training loop.** The first target game is Lineage 2, and the first thing to recognise is monsters. The
agreed plan (record, propose, review, train, detect live; full colour; single frames plus tracking; build order and
open questions) is in [docs/training-loop.md](docs/training-loop.md). Read it before any work on recording,
labelling, training or recognition, and keep its status and decisions current.

## Architecture

Two threads, each with its own Direct3D 11 device on the GPU that drives the primary monitor (observex lists and
captures only the screens on that GPU):

- **Pipeline thread** (`engine/Pipeline`, time-critical priority): `ScreenCapture` waits for a new screen image (DXGI
  Desktop Duplication; the image stays on the GPU), then recognise and react will run on it, then it copies the image
  into the preview `ImageChannel` while the UI shows it, and into the recording `ImageChannel` when a recording image
  is due (`setRecordingRate`, images per second). Once a second it hands `PipelineStats` to the UI.
- **Recorder thread** (`engine/Recorder`, below-normal priority, only while recording): its own GPU device reads the
  recording channel into a CPU-readable texture and writes numbered JPEGs (`engine/ImageFile`, WIC). Slow disks or
  encoding can never hold up capture; images that arrive while one is saving replace each other.
- **UI thread** (`app/App`, the main thread): `MainWindow` (Win32 window, swap chain, hotkey), ImGui, `Theme`
  (fonts, colours, logo), `Widgets` (`BarButton`, the top-bar button look), `ScreenPicker` (the miniatures),
  `ScreenImage` (a screen image the UI can draw at any size: it keeps a mip chain so shrunken views look smooth, and
  draws opaque), `Settings` (observex.cfg, folder picker) and `Review` (the review screen; it reads each image with
  `engine/ImageFile` on the UI thread, 11 to 17 ms for a 2560 x 1440 JPEG, and keeps marks in `core/Marks`).

The threads share data only through lock-free hand-overs, so the UI and the recorder can never slow the pipeline:

- `TripleBuffer<T>` (core): newest-value hand-over for stats.
- `ImageChannel` / `ImageReader` (engine): three shared GPU textures rotated by a TripleBuffer; keyed mutexes order
  the two devices' GPU work. The reader copies each new image out right away and hands the texture straight back.
  One channel for the UI preview, one for the recorder.
- Atomics for state, the preview on/off flag and the recording rate.

Lifecycle: the UI thread owns the pipeline. Picking a screen starts it on that screen; a display mode change restarts
it on the same screen. Capture interruptions (fullscreen switch, UAC prompt, lock screen) are recovered inside the
pipeline thread. The picker captures the screens itself only while it is shown (on the UI device, never waiting), and
not the observed one, whose miniature reuses the observed image. A recording belongs to `App` (`Recording`: folder,
start time, images saved); a pipeline restart only pauses it (the `Recorder` is destroyed before the pipeline stops
and made again after, numbering on in the same folder). Picking a screen, a disconnected screen or a save error
ends it.

Planned next, inside `engine/`, called from the pipeline loop where `Pipeline::run` marks the spot: a recogniser (a
TensorRT detector reading the captured texture through CUDA, plus a tracker, reading back only the boxes; see
[docs/training-loop.md](docs/training-loop.md)), a reactor (`SendInput`), and a knowledge file.

## What speed means in practice

- Measure, don't guess. `observex_bench` runs the real pipeline headless and prints images/s, merged updates,
  latency and work per second. Only trust Release builds for numbers. Run it after any engine or preview change.
- The hot path (capture, recognise, react) does no per-frame heap allocation, no disk I/O, no string formatting, no
  exceptions, and never waits on a lock the UI can hold.
- Keep images on the GPU and read back only small results, never whole frames.
- The UI only shows what the pipeline produces. Anything only the UI needs (like the preview copy) must cost nothing
  when it is not shown.
- Every GPU device that must not be held up by another gets an adapter object of its own (`findOnOwnAdapter`).
  Devices made from one shared `IDXGIAdapter` hold each other up: capture fell from 240 to 24 images/s next to a
  device presenting every refresh.

## Build and test

Configuring, building and running the tests and the bench here is fine. Do not launch the app (rule 5).

- Configure: `cmake --preset vs2022` (Visual Studio 2022 generator, output in `build/vs2022`)
- Build: `cmake --build --preset debug` (or `release`)
- Test: `ctest --preset debug`
- Bench: `build/vs2022/bin/Release/observex_bench.exe [--screen N] [--seconds S] [--no-pulse] [--no-preview]
  [--record FOLDER [--per-second N]]`. Lists the screens and measures one (the primary by default). Its "pulse" is an
  invisible 2 x 2 pixel, click-through, never-focused window in the screen's corner that presents every refresh like
  a game, so the screen has something to capture. `--record` saves images like the Record button does (into a
  scratch folder, never the repo), so its cost shows in the numbers.
- The app: `observex.exe` in the project folder. The Release build writes it there (gitignored). The Debug build
  stays in `build/vs2022/bin/Debug/`.
- UI snapshot: `observex.exe --snapshot <file.png> [--click X Y | --drag X1 Y1 X2 Y2 | --key NAME]...
  [--mouse X Y] [--review FOLDER]`. Draws the UI in a hidden window (it never appears or takes focus), plays each
  input step in order about 0.6 s apart (clicks, drags, key presses named as ImGui names them: `Y`, `RightArrow`,
  `Delete`, ...), rests the mouse at `--mouse` for hover effects, saves the frame as a PNG and quits. Points are
  window pixels, as in the PNG (take one snapshot first to find them). It really observes and records what you
  click. Snapshot mode never shows a Windows dialog: the Review button opens `--review FOLDER` instead of the folder
  picker. Use a build with no console window: the Release exe, or, while the user has `observex.exe` open (then the
  Release link fails), `cmake --build build/vs2022 --config RelWithDebInfo` and
  `build/vs2022/bin/RelWithDebInfo/observex.exe`. Save snapshots and test recordings outside the repo, since they
  show the user's screens. Snapshot mode does not read or write observex.cfg, never registers the hotkey, and
  records into `recordings/` next to the PNG.

## Layout

- `src/core/`: platform-free building blocks (`TripleBuffer`, `StatWindow`, `Marks`: boxes, answers, the marks file
  format, carrying answers forward). No Windows or GPU headers. Unit tested.
- `src/engine/`: the real-time pipeline and the recorder (`Gpu`, `ScreenCapture`, `ImageChannel`, `Pipeline`,
  `Recorder`, `ImageFile`). Windows and Direct3D 11, no ImGui.
- `src/app/`: the window and UI (`App`, `MainWindow`, `Theme`, `Widgets`, `ScreenPicker`, `ScreenImage`, `Settings`,
  `Review`, `Snapshot`, `main`), plus
  `observex.rc.in` and `Resources.h` (icon, fonts and version info built into the exe).
- `bench/`: `observex_bench`, the headless pipeline benchmark.
- `assets/`: the logo (`logo.svg`, `logo.png`) and the app icon (`observex.ico`), all made by `tools/make_logo.py`.
  Change the logo there and rerun it.
- `tests/`: doctest tests for `src/core`. Add tests with any change to core.
- `docs/`: plans that span many sessions. `training-loop.md` is how observex learns to recognise things.
- `cmake/Dependencies.cmake`: Dear ImGui and doctest via FetchContent, and the Inter font files. Pin every dependency
  to a release and its SHA256.

## Conventions

- Namespace `observex`. Types `PascalCase`, functions and variables `camelCase`, members end in `_`, constants `kName`.
- Format with `.clang-format` (clang-format ships with Visual Studio under `VC/Tools/Llvm/bin`).
- New source files must be added to the target lists in `CMakeLists.txt`.
- Our targets build with `/W4 /permissive-`; keep them warning-free.
- Setup code reports errors by throwing (`check()` in `engine/Gpu.h`); the hot path returns status codes instead.
- ImGui and the UI's GPU device are used on the UI thread only. The pipeline never touches UI objects.
- UI colours and sizes come from `app/Theme`; scale pixel sizes by the DPI scale.
