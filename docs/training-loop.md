# The training loop

How observex learns to recognise things on screen, starting with **monsters in Lineage 2**. This is the agreed plan
(2026-10-31). Read it before working on recording, labelling, training or recognition, and update it when a
decision changes or a step gets built.

## The idea

The user plays while observex records. Afterwards observex shows the user the things it found interesting, one at
a time, and the user says what each one is. observex trains on those answers, and the next recording it guesses
better. Rounds repeat until observex finds the monsters on its own, measured by a score.

```
record  ->  propose  ->  review  ->  train  ->  detect live
  ^                                                |
  +------------------ next round ------------------+
```

1. **Record.** While the user plays, observex saves one frame per second in the background. Nothing is analysed
   during play, so the game and the capture loop are never slowed.
2. **Propose.** After the session, observex looks through the saved frames and picks out candidate objects. In the
   first round it knows nothing yet, so a general "find every distinct object" model makes the guesses (slow, but it
   runs offline). From the second round on, observex's own trained model makes them.
3. **Review.** The user asks for "the next thing you think might be a monster" and answers with one key per item:
   monster, player, NPC, or nothing (as built: one category at a time, e.g. monsters, answered yes or no). observex:
   - shows each candidate in its frame, with the box drawn on it;
   - **labels tracks, not single pictures**: the same monster in consecutive frames is followed from frame to frame,
     so one answer covers it from every angle seen;
   - **asks the most uncertain items first**, so each answer teaches the most; items it is sure about need no answer;
   - **also asks about misses**: now and then it shows a whole frame and asks the user to click any monster it did
     not mark.
4. **Train.** A separate Python tool trains the detector on all answers so far, on the RTX 4090 (about 15 minutes
   for 120 images), and writes a model file.
5. **Detect live.** observex runs the model on every captured frame through TensorRT (measured: 4.7 ms per frame on
   the 4090, RF-DETR Medium at 1024). The next recording uses it to propose, so each round asks fewer questions.

**When is it done?** A small set of the user's answers is kept aside and never trained on. After each round,
observex reports a score on it, e.g. "finds 94% of monsters, 2 false alarms per 100 frames". The user decides
when the score is good enough.

## Decisions and why

- **Full colour, no outline step.** The model sees the normal colour image. Colour costs almost nothing extra
  (only the model's first layer reads it), and it is evidence: creatures differ from grass, rock and sky in colour
  and texture. Reducing the image to outlines first would throw that away and add noise: foliage, shadows, particle
  effects and lighting all make edges, and a monster's outline merges with the ground. The model learns its own
  edge and shape detectors in its first layers, better than a fixed outline filter.
  To stop it leaning on colour alone (e.g. "monsters are things in green forests"), training randomly changes
  brightness, contrast and hue, which also covers Lineage 2's day and night.
- **Single frames plus tracking, not a video model.** The detector looks at one frame at a time, so it can react the
  frame a monster appears. A light tracker (matching boxes between frames, microseconds) then:
  - ignores one-frame flickers (a detection must persist to count), which removes most false alarms;
  - gives each monster an identity, a direction and a speed, which the playing logic will need;
  - spreads one review answer across all frames of a monster (see Review).
  A model that reads several frames at once could use motion such as a run cycle, but it costs more per frame,
  needs a few frames before it can decide, and in Lineage 2 the camera moves almost constantly, so everything moves
  on screen. Many monsters also stand still. Revisit only if the score shows still frames are not enough; the
  cheap first step would be giving the detector the previous frame as extra input.
- **Speed levers, in order of effect:** run only on the game's 3D view (crop out fixed UI panels), choose the input
  resolution (far monsters are small, so maybe 1280 wide rather than 640), choose the model size (nano or small),
  and use FP16 (later maybe INT8 or FP8). Colour versus greyscale is not a speed lever. Measured 2026-11-01 (step
  4): in Python, Small, Medium and Large cost the same at 1024; 768 pixels and FP16 each save about a third to a
  half, so resolution and FP16 are the levers, and Medium (the most accurate) costs nothing extra.
- **The image never leaves the GPU.** The captured Direct3D 11 texture is shared with CUDA once, and TensorRT reads
  it every frame directly. Only the small list of boxes comes back to the CPU.
- **Recognition runs beside capture, not inside it** (2026-11-01). The model takes about 5 ms, longer than a 240 Hz
  frame, so it cannot sit in the capture loop. The pipeline hands each new image to the recogniser's own thread and
  GPU device (an `ImageChannel`, like the preview; 0.09 ms), and the recogniser always takes the newest one. Capture
  stays at every refresh.
- **The model file is ONNX, the engine is built on the user's PC** (2026-11-01). `export.py` writes `model.onnx`
  with half-precision weights; TensorRT turns it into an engine for this exact GPU and driver the first time
  observex starts with a new model (35 s on the 4090) and saves it next to the model as `model.engine`; later starts
  load it in 0.4 s. TensorRT's DLLs are not part of the build output: CMake downloads them once into `deps/` from
  NVIDIA's public places (its pip server, its GitHub headers, CUDA's redistributable parts), because NVIDIA's own
  TensorRT download needs an account login. observex loads them at run time.
- **Show flags, like Unreal's view flags** (the user asked, 2026-11-01). The Show dropdown says what is drawn over
  the observed screen (for now: Monsters). A flag only hides or shows; recognition goes on either way, also while
  the window is minimized or a review covers the view.
- **Models:** RF-DETR or D-FINE (Apache 2.0, no strings attached). Ultralytics YOLO26 is as fast but AGPL, which
  requires observex to stay open source under the AGPL or a paid licence. Pick by measuring on the 4090.
- **Two languages, one boundary.** observex stays C++ (capture, recording, review screen, live detection). Training
  is a Python tool in `tools/` that observex starts after a review; the boundary is the model file.
- **The game can label for free.** Lineage 2 marks monsters itself: the target window shows a name and HP bar,
  names can show above heads, and pointing at a monster highlights it. These can confirm or create labels without
  the user. To check on the exact client version used.
- **Recordings stay out of the repo.** Frames show the user's screen and grow by gigabytes per hour; they go to a
  data folder outside the repo, chosen in the app.

## Build order

Each step can be tried on its own.

1. **Recording**: save frames (and later the user's clicks and keys) to the data folder, without slowing capture.
   **Built (frames).** Record button or Ctrl+Alt+R; one folder per recording, named by date and time, holding
   `000001.jpg`, `000002.jpg`, ...; 1 image per second, fixed (the user asked for no rate setting; neighbouring
   images would be near copies); only images the screen actually updated. JPEG quality 85: about 0.7 MB per
   2560 x 1440 Lineage 2 image, about 2.6 GB per hour. No measurable cost to capture. Clicks and keys: not yet.
2. **Review screen**: step through candidates and answer with one key; show the score.
   **Built (answering).** Review button, then the recording's folder, then the category ("What are we looking
   for?"; only Monsters for now). Each image shows the finder's marks with how sure it is; the user answers yes or
   no per mark (click, or Y / N), drags a box around anything missed, and moves on with the Right arrow. Moving on
   makes the image *reviewed*: unanswered marks count as no, and everything in the category on it is marked yes.
   Answers carry forward to marks on the next image that overlap an answered one by at least half (marked
   "copied"). Every image is shown, in order.
   **How to draw a box** (2026-11-01, after checking every answer; mixed habits hurt the model more than anything
   else): one box per monster, tight around its body only, never the name and level above it. Every visible monster
   counts, far-off ones too. A monster that is fully hidden (behind the player, the chat or a wall), or of which only
   the name shows, gets no box. Treasure chests are monsters; players never are. Two overlapping monsters get two
   boxes. A "no" on a mark that sits on a real monster teaches the opposite: draw the right box instead.
   Not yet: tracks (one answer for a monster across many images), most-uncertain-first order, and the score.
   Files, in each recording's folder, plain text (`src/core/Marks.h`), for the finder and the training tool:
   ```
   # <category>-guesses.txt, written by the finder: how sure it is (0 to 1), then x y width height in pixels
   image 000001.jpg
   0.81 150 312 34 42
   # <category>-answers.txt, written by review: only reviewed images
   image 000001.jpg
   yes 150 312 34 42
   no 1408 384 51 79
   ```
3. **First-round proposer**: the general object finder for round one. **Probably not needed** (2026-10-31): the
   user hand-labels the first few dozen images (drawing every monster), the first model is trained on those, and
   that model is the finder from round two on. Build a general finder only if hand-labelling turns out too slow.
4. **Training tool**: Python, trains the detector and exports the model file.
   **Built** (`tools/train/`, its README has the commands). `train.py` collects every recording with
   `<category>-answers.txt`, keeps the last fifth of each recording aside for the score (the last part, not random
   images: neighbouring images are near copies and would flatter it), fine-tunes RF-DETR Medium (pretrained on
   everyday photos; its 80 kinds replaced by the one category) at 1024 x 1024 for 40 passes, with random colour
   changes on the training images, and saves
   `models/<category>/model.pth` and `model.json` (settings, counts, score). `guess.py RECORDING` writes the model's
   guesses as `<category>-guesses.txt` for the review, down to 30% sure. The review runs it by itself in the
   background (`app/Finder`) when a recording's guesses are missing or older than the model, and shows its progress
   (the user asked, 2026-11-01; until step 5 lets observex run the model itself). Python stays in a private `.venv`
   in `tools/train/`; packages pinned in `requirements.txt`.
   Learned on the way: RF-DETR's automatic batch size never finishes on Windows (the NVIDIA driver moves GPU memory
   into normal RAM instead of failing, so the "until it does not fit" search runs on); the batch is fixed at 4,
   16 images per learning step. On the 4090: about 8 s per pass over 120 images, about 7.6 GB of GPU memory;
   prediction in Python 48 ms per image (file reading included).
   **First model (2026-11-01):** 120 hand-labelled images from 3 recordings, one place each (Elven Ruins, Ruins of
   Agony, Cruma Marshlands), 298 monsters; 96 trained on, 24 held out (52 monsters). 100 passes in 13 min; the best
   was pass 26, it levelled off after about 20. Held-out score by how sure a mark must be: 30%: finds 45 of 52, 10
   false alarms; 40%: 43 of 52 (83%), 4 false alarms; 50%: 37 of 52, 3 false alarms (in 24 images). The held-out
   images come from the same places it trained on, so a new place may score lower; training on two places and
   scoring on the third would measure that. Checked in the review: guesses show as marks on images it never saw.
   **Second model (night of 2026-11-01, Claude alone, no game):** 5 recordings, 165 images.
   - *Answers checked one by one.* Most of the first ones boxed the name and level along with the body (Elven Ruins,
     Elven Fortress), some boxes were loose (Ruins of Agony, 2 in Cruma), some monsters had two boxes, some had
     only their name showing, far-off monsters and chests were missed, and three "no" marks in Elven Fortress sat
     on real orcs. All redrawn to the rule in step 2: 411 to 421 monsters, 211 boxes changed. The user's original
     files stay next to the new ones as `monster-answers.original.txt`; every change is logged.
   - *Two tests.* (1) A place it never saw: train on four recordings, score the fifth, for each of the five
     (AP50, 423 monsters). (2) Places it knows: train on the first 70% of each recording, score the last 30%.
   - *Results, place never seen:* Small with the old answers AP50 0.62 (finds 59% at 40% sure, 86 false alarms);
     the same with checked answers 0.69 (68%); plus colour changes 0.73; Medium instead of Small 0.74 (72%, 75 false
     alarms). No better: Large 0.69, 1280 pixels 0.72, zoom changes 0.70, 80 passes 0.73 (every best pass came
     before 35), 768 pixels 0.74 (a tie: fewer far-off monsters found, more in Agony). *Places it knows:* AP50
     0.76 to 0.87, finds 78% to 82% at 40% sure, false alarms 28 to 17.
   - *Final model:* Medium, 1024, colour, 40 passes, 6 minutes, 12.6 GB of GPU memory, on 132 images (357
     monsters). On the 33 held-out images (64 monsters), at 40% sure: finds 58 (91%), 2 false alarms; at 30%: 60
     (94%), 5. The first model on the same images: 45 (70%), 11 false alarms. What it still misses: see-through
     Spartoi in Ruins of Agony (seen at 18% to 34% sure), an eye peeking out of a tree; one false alarm is a rock.
     The weakest place by far is Ruins of Agony when it is not trained on (18 of 70): more reviewed recordings,
     Agony above all, will help most.
   - *Speed* (RTX 4090, Python, image already in memory): Small, Medium and Large all about 26 ms at 1024 (the
     resolution sets the cost, not the size); 768 pixels 18 ms; half precision, compiled, 14 ms with the same
     guesses (`guess.py` uses it). Run trainings one at a time: two at once filled the 24 GB, Windows moved GPU
     memory into normal RAM, each ran twice as slowly and one hung.
   Exporting: `export.py` writes `models/<category>/model.onnx` (half-precision weights, full-precision input and
   output) for live detection; `train.py` runs it at the end. Not yet: starting training from observex's UI.
5. **Live detection**: TensorRT on every captured image, plus the tracker; draw the boxes in the observed view.
   **Built (detector and boxes), 2026-11-01.** `engine/Detector` runs `model.onnx` through TensorRT 11.4 (FP16):
   a compute shader squeezes the captured image to 1024 x 1024 (bilinear, ImageNet colours, as RF-DETR does) into a
   buffer CUDA shares with Direct3D 11, TensorRT runs the network on it, and only the 300 candidate boxes come back
   to the CPU. `engine/Recognizer` runs it on its own thread (above normal priority) and its own GPU device, on the
   newest captured image, and hands the boxes to the UI lock-free. Starting it on a screen loads the saved engine in
   0.4 s; a new model is first prepared for the GPU (35 s on the 4090, progress shown). In the app: the **Show**
   dropdown (eye icon, right of the screen button) has a **Monsters** flag (saved in observex.cfg as
   `show-monsters`), and under it how recognition is doing ("3 found · 210 images a second · 4.7 ms each", or the
   preparing progress, or why it cannot run). With the flag on, each monster found at least 30% sure gets a violet
   box tagged "Monster 81%". F3 adds the recogniser's numbers.
   - *Same answers as Python:* on the 33 held-out images, the 65 monsters Python finds at 30% are all found; 2 more
     sit right at the cutoff (30.3%, 30.5%); sureness differs by 0.003 on average (0.017 at most), boxes by 4 pixels
     at most.
   - *Speed (RTX 4090, Release):* 4.7 to 5.0 ms per image from captured image to boxes (Python half precision:
     14 ms); about 210 of the 240 images a second; capture unchanged with it running (239.7 images a second, 0.05 ms
     latency; the hand-over costs 0.09 ms of pipeline work per image).
   - *The GPU is busy:* at 210 images a second the model keeps the GPU nearly fully used. A game on the same GPU
     will have to share it; if its frame rate drops, a limit on looks per second is the fix (not built yet).
   Not yet: the tracker; making it faster (well under the 4.2 ms of one 240 Hz frame: CUDA graphs, cropping the
   game's fixed UI panels, 768 pixels); the playing logic.
6. **Learn while observing** (the user's idea, 2026-11-01; agreed order). The model already runs while the user
   watches the game; when it is unsure about something, it saves a question for the user to answer later, and each
   answer improves the model without leaving the app, so the user can walk back to the same place and see the
   change. Training is the slow part (minutes on the GPU), not swapping the weights: TensorRT can *refit* an engine
   with new weights of the same network in about a second, without rebuilding it.
   1. Live detection with show flags. **Built** (step 5).
   2. Questions while observing: images where the model is unsure (marks between about 20% and 60% sure, or flicker
      between frames) are saved with its guesses, to answer later in the review.
   3. A background trainer: after answers, fine-tune from the current model in the background, export, and refit the
      running engine, so the boxes change live.

## Open questions

- Who labels long sessions. The user suggested Claude could label instead of them. Claude reads these frames well
  (it can tell gremlins from the Defender guard and the player), but its boxes are only roughly placed, and through
  a chat it takes many seconds per image. Likely split: a finder model draws precise boxes; Claude (through the API,
  in a tool, at a cost per image) or the user answers yes or no; the user spot-checks. To decide with step 3.

- Which Lineage 2: the first recordings (2026-10-31) show a private Interlude server (L2 Reborn, "Reborn Eternal
  Interlude x10"). Not yet confirmed by the user as the target. There, monsters show "Level: N" and their name above
  their heads, and the targeted one also gets red markers beside its name and the target window at the top; these
  are the free labels described above. A private server's own rules decide whether bots are allowed.
- Data folder: decided. `recordings/` next to observex.exe by default, changeable in Settings; 1 image per second.
- Round one: decided for now. The user draws the boxes on the first few dozen images themselves (varied places,
  distances and monster kinds, every monster on each image), then the first model is trained. A general object
  finder (e.g. Segment Anything) only if that proves too slow.

## Reference numbers (from research, 2026-10-31, not measured here)

| Method | Time per image |
| --- | --- |
| Pixel checks and template matching in a compute shader | microseconds |
| Small detector, TensorRT FP16 (YOLO26s on an RTX 5060 Ti) | about 1 ms; the 4090 should be roughly twice as fast |
| RF-DETR nano / D-FINE nano, TensorRT FP16 on a T4 | 2.3 ms / 2.1 ms |
| OCR, PaddleOCR with TensorRT FP16 on a laptop RTX 4060 | about 12 ms (DirectML: about 67 ms) |
| OmniParser V2 (parses every UI element) on an RTX 4090 | about 0.8 s |
| Vision-language models (Claude, GPT, Qwen-VL) | 0.5 s to several seconds |

At 240 Hz one frame lasts 4.2 ms, so live recognition has to fit well inside that; the slow models are only for
offline work such as proposing and labelling.
