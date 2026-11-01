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

The UI is deliberately minimal: a top bar with the screen button and Show on the left and Review, Record and Settings
on the right.
The screen button looks like a dropdown (monitor icon, text, arrow) and reads "Choose a screen", or "Observing" and
the observed monitor's name. It opens live miniatures of every
screen, laid out the way the screens are arranged on the desktop, each labelled with its monitor's model name (from
the monitor itself, e.g. "Odyssey G61SH"; size and refresh rate show on hover). Clicking one starts observing it, and
the window shows only that screen, live. With nothing observed yet, the miniatures fill the window under the logo.
F3 shows the pipeline's and the recogniser's numbers over the view (hidden by default).

Live recognition (step 5 of the training loop): while a screen is observed, the recogniser runs the trained character
model (`models/character/model.onnx`) on every new captured image, on its own thread, whatever is shown: also while
minimized or with the flag off. While reviewing it pauses (the user asked, 2026-11-01): the pipeline hands it no
images, so the review's finder gets the GPU; the model stays loaded and it picks up again when the review closes.
**Show** (eye icon, right of the screen button, only while the screen
is shown) is a dropdown of flags like Unreal's view flags: **Characters** draws a violet box tagged "Character 81%"
around each character the tracker follows (saved in observex.cfg as `show-characters`). Under the flag: how
recognition is doing ("3 found · 240 images a second · 2.1 ms each"), the progress of preparing a new model for the
GPU (once per new model, 35 s on the 4090, saved as `model.engine` next to it; later starts load in 0.4 s), or why it
cannot run (e.g. no model.onnx). Hiding a flag never stops recognition: later flags (and the reactor) rely on it.
Measured on the 4090: 2.1 ms per image, all 240 images a second (2026-11-01: 4.7 ms and 210 a second at first,
3.3 ms and 235 once the network ran as a CUDA graph, 2.1 ms at 768 pixels instead of 1024); capture unchanged (239.8 images a second, 0.04 ms latency). The GPU is then nearly fully used, so other GPU work feels it: the finder took 37 ms per image instead of
5 next to it (hence the pause while reviewing), and a game does too: with the user's Lineage 2 running, about 100
images a second at 10 ms each (2026-11-01); a looks-per-second limit is the fix if the game suffers (not built).

Recording (step 1 of the training loop): **Record** (or Ctrl+Alt+R, a system-wide hotkey that works while the game
has focus or observex is minimized) saves the observed screen into a new folder named by date and time, e.g.
`recordings/2026-10-31_23-05-12/000001.jpg`, 1 image every 3 seconds (fixed, `kRecordingInterval` in App.cpp: the
user asked for no rate settings, then for 1 every 3 s instead of skipping near-copy images in the review,
2026-11-01). Only images the screen actually updated are saved. Starting and stopping writes `recording.observex`
into the folder (`core/Recordings`: `key=value` lines `started`, `seconds`, `screen`, `size`, `every`, `images`,
written through a temporary file); the review lists only folders that have one (the user asked, 2026-11-01:
choosing a folder felt unnatural). The 11 recordings made before got theirs from a one-off script (`started` from
the folder name, the rest from the images and the live log).
While recording, the button reads "Stop", the time, the number of images and their size on disk (the Recorder sums
each file's size; shown in 1024s, as File Explorer does). JPEG quality 85: about 0.7 MB per 2560 x 1440 Lineage 2
image, so about 0.85 GB per hour. **Settings** (gear) holds the recordings folder (default: `recordings` next to
observex.exe; folder picker, Open, Use default) and the live log flag (below). Settings live in `observex.cfg` next to the exe. Both it and
`recordings/` are gitignored.
Recording costs capture nothing measurable (bench: 238.7 and 239.3 images/s recording, 238.2 and 239.2 not).
Live log (the user asked, 2026-11-01, to show Claude what they see live: sureness that is high at times and low at
others, one-image blips): while recording, `engine/LiveLog` writes `character-live.txt` into the recording's folder:
a `look` line for every image the recogniser looked at (screen image number, time, everything found at least 10%
sure with its box; the app draws 30% and up), `saved` lines tying each saved JPEG to its screen image number, and
`mark` lines. **Ctrl+Alt+M** (second system-wide hotkey) marks a moment: a numbered `mark` line, and the image on
screen is saved right then (`Pipeline::recordNow`; the 3 s rhythm counts on from it); the Record button shows the
mark count. The user then says "at mark 3 ..." and Claude reads the log: follow each thing from look to look
(match boxes), plot its sureness over time, find one-look blips, and line them up with the saved images. About 85
MB per hour; lines are formatted outside a lock and buffered (1 MB), marks flushed. The file's header explains the
lines. A Settings checkbox, "Log what the model sees while recording" (`live-log` in observex.cfg, on by
default; the user asked for it), turns the log off from the next recording; with it off Ctrl+Alt+M does nothing
and the Record tooltip only mentions images. Image numbers come from `ImageStamp` (the pipeline's count and the time the screen showed it), which every
`ImageChannel` hand-over now carries.

Review (step 2 of the training loop, `app/Review`): **Review** lists the recordings (the recordings folder's
subfolders with a `recording.observex`, newest first: when, e.g. "Today, 12:56"; length; images; how many are
reviewed per category; read once per opening), then "What are we looking for?" (categories in `kCategories`; only Characters for now), then shows the recording's
first image not reviewed yet, filling the view. The finder's guesses are drawn as marks with a tag ("Character? 81%");
the selected mark gets Yes / No buttons under it (Y / N keys; answering moves to the next unanswered mark), a white
ring around it, and the other marks fade to 35% (`kOtherMarks`; the user could not tell which of several close
characters was selected, 2026-11-01). A click
picks the mark under it (the smallest there); a drag of at least 6 screen pixels each way draws a missed one (yes;
Remove / Del deletes it) wherever it starts, also inside a bigger mark (the user could not draw a character behind
their own character's mark, 2026-11-01). The bar's middle shows back / "Image 5 of 340 · 11
reviewed" / forward (Left / Right keys). Moving forward needs every mark answered (the user asked,
2026-11-01): forward is greyed, its tooltip says how many are left, and Right picks the first unanswered one. Then
the image is "reviewed" and the answers are saved. Answers carry forward: a guess on the next image overlapping an answered mark by at least
half (`carryAnswers`) gets its answer, tagged "copied". Then guesses at least 65% sure start as yes (`presumeSure`, `kPresumeYes`; green, tagged "Character 92%"): the
user only answers the unsure ones and flips wrong ones (the user asked, 2026-11-01: reviewing took too long; on the
user's first 64 images of a new place, 223 of 230 guesses at 75% were characters, 97%; 75% became 65% when the model
moved to 768 pixels, where it is less sure: the same 196 of 238 held-out characters pre-answered, 3 wrong).
There is no Skip: the user chose recording 1 image every 3 s over skipping near copies in the review. No setting
for a review step (the user removed one before).
Files, in the recording's folder, in the text format of `core/Marks.h`: `<category>-guesses.txt` (from the finder,
read only) and `<category>-answers.txt` (written by review; only reviewed images, rewritten through a temporary file
on every change). The finder (`engine/Finder`): when a review opens a recording with images not reviewed yet, and
`models/<category>/model.onnx` exists, and the guesses are missing or older than it, it runs the model over the
recording inside observex, on its own thread (below-normal priority) and GPU device, with the same `Detector` as
live recognition: read each JPEG, upload it, detect, then write the guesses through a temporary file. No Python
(the user asked, 2026-11-01: guess.py took about 5 s just to start). The review opens at once with "Looking for
characters with the trained model / Image N of M" at the top (or "Loading the model…", or the preparing progress for a
new model), and loads the guesses when it is done; the image shown gets them unless the user already answered or
drew on it. Same guesses as Python's guess.py (40 of 40 marks, sureness within 0.002 on average, boxes within 2
pixels); about 1 s to start, then about 20 ms per image (JPEG reading included). Models are found by
`modelFolder()` (`app/Settings`): `models/<category>` in the project folder, found by going up from observex.exe
to the folder holding `tools/train/guess.py`, else next to observex.exe.
With no trained model, the review starts empty and says so. While reviewing, the
pipeline's preview is off and keyboard navigation of buttons is off (the review owns the arrows, Tab and letters).
The Inter font has no arrow glyphs: write "Left / Right", not arrow characters. The Review button's icon is a
picture (a tick looked like "confirm something", the user said).

Look: Inter font (regular and medium, built into the exe), a dark neutral palette with one violet accent
(`#8B7BFF`), rounded corners, a dark title bar in the same colours, and the observex logo (a ring with a pupil
glancing up and right, on a dark rounded tile) as the app icon.

Measured with `observex_bench` (RTX 4090, 2560 x 1440 at 240 Hz, Release, a game-like source presenting every
refresh, with the UI's preview copy included): 239.7 images/s (every refresh), 0.05 ms from screen update to image,
about 0.1 ms of CPU work per image.

Training (step 4 of the training loop, `tools/train/`, Python): `train.py` fine-tunes RF-DETR Medium on every
recording's `<category>-answers.txt` (last fifth of each recording held out for the score) and saves
`models/<category>/model.pth` (gitignored), then `model.onnx` for live recognition (`export.py`, half-precision
weights; run it again by hand to re-export a model). `guess.py RECORDING` writes `<category>-guesses.txt` with the
Python model (half precision); observex no longer runs it (its own finder does the same, see above), but it stays as
the Python reference to check observex's results against.
Its own Python in `tools/train/.venv` (gitignored; setup in `tools/train/README.md`). Training runs on the GPU for
about 12 minutes on 398 images: run it in the background, never in the foreground, and never two trainings at once
(two filled the 24 GB, Windows spilled GPU memory into normal RAM, both slowed to half and one hung). Keep the batch
size fixed (see the README).
Answers follow the box rule in docs/training-loop.md, step 2 (body only, every visible character, none for hidden
ones; monsters, other players and NPCs are characters, the user's own never is). It trains at 1024 pixels and observex runs it at 768 (`LIVE_RESOLUTION` in train.py; export.py, guess.py
and the score use it): a third less time live, nearly the same answers; training at 768 was worse (2026-11-01).
Current model (fifth, 2026-11-01 22:41, 15 recordings, the first with other players and NPCs as characters), run
at 768: on 142 held-out images (494 characters) at 50% sure (where the tracker starts) it finds 435 with 40 false
alarms, the fourth model 276 and 16; AP50 0.669 → 0.919. Cave of Trials 10 → 23 of 30. About 5 of its false alarms
are the user's own character, which it now sometimes takes for another player (details in docs/training-loop.md,
step 4).

Tracker (`core/Tracker`, built 2026-11-01; the user saw sureness swing and one-image blips, see the live log):
follows what the model finds from look to look, so each character keeps one steady box and an identity (id, speed).
A new thing starts being followed only from a box at least 50% sure (`kStartSure`) and counts once found on 3
looks in a row; once followed, boxes from 30% keep it (pairs with a sure box are matched first). One that is not
found is held where its
speed takes it, as long as it was seen, up to 150 ms; matching is nearest-first to where each is heading (reach
grows 0.5 sides per 100 ms unseen), or a box lying 80% inside the other (cut by the screen edge); `sure` is smoothed
over about 60 ms. On the user's fight log (17.5 s, `observex_bench --track`): boxes appearing or vanishing 21.1 ->
3.8 a second, none shown for a single look, 99.6% of what the model saw still covered, a new character shows at most
~20 ms later at 100 looks/s; 0.2 microseconds per look. The rules were picked by replaying the live logs in Python
(Python and C++ give identical numbers); calm logs barely change (they were already steady). Remaining churn is
crowds and close-ups, which only more labelled images of those fix. The live log stays raw (what the model saw).
The 50% start rule (2026-11-01; the user saw a 30-40% box blink on a static ornament, and parts of a close
succubus shown as characters): boxes appearing or vanishing a second, fight log 3.8 -> 2.7, the 12-56 log 6.0 ->
3.0, the 13-03 log 3.2 (40.5 raw); calm logs unchanged (0.3 to 0.6). Measured and not kept: a tracker rule
merging parts into the whole (worse), and dropping boxes lying inside a bigger one (small gain, hides characters
standing in front of others).

Not built yet: react (mouse and keyboard), and the knowledge file (what to look for and how to react,
saved and loaded).

**Next: learn while observing** (agreed with the user, 2026-11-01, one step at a time; step 6 of the training
loop): questions saved while observing when the model is unsure, then a background trainer that refits the running
TensorRT engine so answers change the boxes live.

**The training loop.** The first target game is Lineage 2, and the first thing to recognise is characters (any kind:
monsters, other players, NPCs, not the user's own; telling them apart comes second; the category was "monsters"
until the evening of 2026-11-01). The agreed
plan (record, propose, review, train, detect live; full colour; single frames plus tracking; build order and
open questions) is in [docs/training-loop.md](docs/training-loop.md). Read it before any work on recording,
labelling, training or recognition, and keep its status and decisions current.

## Architecture

Threads, each with its own Direct3D 11 device on the GPU that drives the primary monitor (observex lists and
captures only the screens on that GPU):

- **Pipeline thread** (`engine/Pipeline`, time-critical priority): `ScreenCapture` waits for a new screen image (DXGI
  Desktop Duplication; the image stays on the GPU), then copies it into the recognition `ImageChannel` (first, so the
  recogniser gets it at once), into the preview `ImageChannel` while the UI shows it, and into the recording
  `ImageChannel` when a recording image is due (`setRecordingInterval`, seconds between images; `recordNow` for a
  mark). Each hand-over carries an `ImageStamp` (image number, time shown). React will run here. Once a second it
  hands `PipelineStats` to the UI.
- **Recogniser thread** (`engine/Recognizer`, above-normal priority, while a screen is observed): waits on the
  recognition channel's event, copies the newest image into the `Detector`'s input texture and runs it: a compute
  shader squeezes the image to the model's size (768 x 768) into a buffer shared with CUDA, TensorRT runs the network,
  and only the candidate boxes come back. The network and the copies back run as one CUDA graph, recorded on the
  first look (one launch instead of hundreds: 5.56 -> 4.68 ms per image on recording files, same guesses; the
  Finder gets it too). TensorRT's highest builder optimisation level (5) was tried: slower (4.86 ms) and 141 s to
  prepare, so the default stays. Those at least 30% sure go through the `Tracker` (timed by when the
  screen showed the image). Hands `Found` (what it follows, surest first) and `RecognizerStats` to the UI.
  TensorRT is loaded at run time from `deps/` (no import libraries); the engine for this GPU is built once per new
  model.onnx and cached as `model.engine` (one Detector loads or builds at a time; a second waits, then loads it).
  Paused while reviewing (the pipeline stops handing it images). While recording it also writes each look into the
  live log (`setLog`), down to 10% sure.
- **Finder thread** (`engine/Finder`, below-normal priority, only while a review needs guesses): its own GPU device
  and `Detector`; reads the recording's JPEGs and writes `<category>-guesses.txt`.
- **Recorder thread** (`engine/Recorder`, below-normal priority, only while recording): its own GPU device reads the
  recording channel into a CPU-readable texture and writes numbered JPEGs (`engine/ImageFile`, WIC). Slow disks or
  encoding can never hold up capture; images that arrive while one is saving replace each other. Each saved image
  gets a `saved` line in the live log.
- **UI thread** (`app/App`, the main thread): `MainWindow` (Win32 window, swap chain, system-wide hotkeys: Ctrl+Alt+R, Ctrl+Alt+M), ImGui, `Theme`
  (fonts, colours, logo, the boxes and tags drawn over screen images, shared by the review and the live view), `Widgets` (`BarButton`, the top-bar button look), `ScreenPicker` (the miniatures),
  `ScreenImage` (a screen image the UI can draw at any size: it keeps a mip chain so shrunken views look smooth, and
  draws opaque), `Settings` (observex.cfg, folder picker) and `Review` (the review screen; it reads each image with
  `engine/ImageFile` on the UI thread, 11 to 17 ms for a 2560 x 1440 JPEG, and keeps marks in `core/Marks`).

The threads share data only through lock-free hand-overs, so the UI and the recorder can never slow the pipeline:

- `TripleBuffer<T>` (core): newest-value hand-over for stats.
- `ImageChannel` / `ImageReader` (engine): three shared GPU textures rotated by a TripleBuffer; keyed mutexes order
  the two devices' GPU work. The reader copies each new image out right away and hands the texture straight back.
  One channel for the UI preview, one for the recorder, one for the recogniser (its reader also gets an event, so it
  sleeps until an image arrives).
- Atomics for state, the preview and recognition on/off flags, the recording rate and the model preparing progress.

Lifecycle: the UI thread owns the pipeline. Picking a screen starts it on that screen; a display mode change restarts
it on the same screen. Capture interruptions (fullscreen switch, UAC prompt, lock screen) are recovered inside the
pipeline thread. The picker captures the screens itself only while it is shown (on the UI device, never waiting), and
not the observed one, whose miniature reuses the observed image. A recording belongs to `App` (`Recording`: folder,
start time, images saved, its `RecordingInfo`); a pipeline restart only pauses it (the `Recorder` is destroyed before the pipeline stops
and made again after, numbering on in the same folder). Picking a screen, a disconnected screen or a save error
ends it. The `Recognizer` lives with the observed screen: made in `App::observe`, destroyed in `stopObserving`
before the pipeline frees its channel (its destructor also cancels preparing a model).

Planned next, inside `engine/`: a reactor (`SendInput`) and a knowledge file; see
[docs/training-loop.md](docs/training-loop.md).

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

- Configure: `cmake --preset vs2022` (Visual Studio 2022 generator, output in `build/vs2022`). The first configure
  downloads TensorRT 11.4's DLLs (about 2 GB, NVIDIA's pip server), its headers (NVIDIA's GitHub) and the CUDA 13.3
  runtime parts into `deps/` (gitignored, kept across clean builds), each checked by SHA256 (`cmake/TensorRT.cmake`).
- Build: `cmake --build --preset debug` (or `release`)
- Test: `ctest --preset debug`
- Bench: `build/vs2022/bin/Release/observex_bench.exe [--screen N] [--seconds S] [--no-pulse] [--no-preview]
  [--record FOLDER [--every S]] [--model FOLDER]`. Lists the screens and measures one (the primary by default).
  Its "pulse" is an invisible 2 x 2 pixel, click-through, never-focused window in the screen's corner that presents
  every refresh like a game, so the screen has something to capture. `--record` saves images like the Record button
  does (into a scratch folder, never the repo), so its cost shows in the numbers; with `--model` too it writes the
  live log like the app and marks once halfway (the way to test the log headless). `--model models/character` runs the
  recogniser too and prints its numbers (recognised/s, ms per image, found). `observex_bench --model FOLDER --images
  FOLDER --out FILE` runs the C++ detector over a folder of JPEGs and writes its guesses in the marks format (to
  compare with Python's `guess.py`). `observex_bench --track LOG` replays a live log (a recording's
  `character-live.txt`) through the C++ tracker and prints boxes appearing or vanishing per second with and without
  it, things followed, and its cost per look; no GPU. Use it after any tracker change.
- The app: `observex.exe` in the project folder. The Release build writes it there (gitignored). The Debug build
  stays in `build/vs2022/bin/Debug/`.
- UI snapshot: `observex.exe --snapshot <file.png> [--click X Y | --drag X1 Y1 X2 Y2 | --key NAME]...
  [--mouse X Y] [--recordings FOLDER] [--wait SECONDS]`. Draws the UI in a hidden window (it never appears or takes
  focus), plays each input step in order about 0.6 s apart (clicks, drags, key presses named as ImGui names them: `Y`,
  `RightArrow`, `Delete`, ...), rests the mouse at `--mouse` for hover effects, waits `--wait` more seconds (at
  least; e.g. for the finder; frames take longer than 16 ms, so the real wait is longer), saves the frame as a PNG
  and quits. Points are
  window pixels, as in the PNG (take one snapshot first to find them). It really observes and records what you
  click. Snapshot mode never shows a Windows dialog. `--recordings FOLDER` is the folder the Review list reads
  (default `recordings/` next to the PNG); point it at a copy, since answers in a review are saved. Use a build with no console window: the Release exe, or, while the user has `observex.exe` open (then the
  Release link fails), `cmake --build build/vs2022 --config RelWithDebInfo` and
  `build/vs2022/bin/RelWithDebInfo/observex.exe`. Save snapshots and test recordings outside the repo, since they
  show the user's screens. Snapshot mode does not read or write observex.cfg, never registers the hotkey, and
  records into `recordings/` next to the PNG.

## Layout

- `src/core/`: platform-free building blocks (`TripleBuffer`, `StatWindow`, `Marks`: boxes, answers, the marks file
  format, carrying answers forward; `Recordings`: the `recording.observex` file and listing recordings; `Detection`,
  `Tracker`: following found things from look to look). No Windows or GPU headers. Unit tested.
- `src/engine/`: the real-time pipeline, the recorder and the recogniser (`Gpu`, `ScreenCapture`, `ImageChannel`,
  `Pipeline`, `Recorder`, `ImageFile`, `Detector`, `Recognizer`, `Finder`). Windows, Direct3D 11, CUDA and TensorRT,
  no ImGui.
- `src/app/`: the window and UI (`App`, `MainWindow`, `Theme`, `Widgets`, `ScreenPicker`, `ScreenImage`, `Settings`,
  `Review`, `Snapshot`, `main`), plus
  `observex.rc.in` and `Resources.h` (icon, fonts and version info built into the exe).
- `bench/`: `observex_bench`, the headless pipeline benchmark.
- `assets/`: the logo (`logo.svg`, `logo.png`) and the app icon (`observex.ico`), all made by `tools/make_logo.py`.
  Change the logo there and rerun it.
- `tools/train/`: the training tool (Python: `marks.py` reads and writes the marks format of `core/Marks.h`,
  `train.py`, `guess.py`, `export.py`). The trained models go to `models/` (gitignored): `model.pth` (Python),
  `model.onnx` (live recognition) and `model.engine` (made by observex for this GPU).
- `tests/`: doctest tests for `src/core`. Add tests with any change to core.
- `docs/`: plans that span many sessions. `training-loop.md` is how observex learns to recognise things.
- `cmake/Dependencies.cmake`: Dear ImGui and doctest via FetchContent, and the Inter font files. Pin every dependency
  to a release and its SHA256. `cmake/TensorRT.cmake`: TensorRT and the CUDA runtime into `deps/`.

## Conventions

- Namespace `observex`. Types `PascalCase`, functions and variables `camelCase`, members end in `_`, constants `kName`.
- Format with `.clang-format` (clang-format ships with Visual Studio under `VC/Tools/Llvm/bin`).
- New source files must be added to the target lists in `CMakeLists.txt`.
- Our targets build with `/W4 /permissive-`; keep them warning-free.
- Setup code reports errors by throwing (`check()` in `engine/Gpu.h`); the hot path returns status codes instead.
- ImGui and the UI's GPU device are used on the UI thread only. The pipeline never touches UI objects.
- UI colours and sizes come from `app/Theme`; scale pixel sizes by the DPI scale.
