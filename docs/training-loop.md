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

1. **Record.** While the user plays, observex saves a few frames per second in the background. Nothing is analysed
   during play, so the game and the capture loop are never slowed.
2. **Propose.** After the session, observex looks through the saved frames and picks out candidate objects. In the
   first round it knows nothing yet, so a general "find every distinct object" model makes the guesses (slow, but it
   runs offline). From the second round on, observex's own trained model makes them.
3. **Review.** The user asks for "the next thing you think might be a monster" and answers with one key per item:
   monster, player, NPC, or nothing. observex:
   - shows each candidate in its frame, with the box drawn on it;
   - **labels tracks, not single pictures**: the same monster in consecutive frames is followed from frame to frame,
     so one answer covers it from every angle seen;
   - **asks the most uncertain items first**, so each answer teaches the most; items it is sure about need no answer;
   - **also asks about misses**: now and then it shows a whole frame and asks the user to click any monster it did
     not mark.
4. **Train.** A separate Python tool trains the detector on all answers so far, on the RTX 4090 (about an hour),
   and writes a model file.
5. **Detect live.** observex runs the model on every captured frame through TensorRT, about 0.5 ms per frame on the
   4090 (estimate, to be measured). The next recording uses it to propose, so each round asks fewer questions.

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
  and use FP16 (later maybe INT8 or FP8). Colour versus greyscale is not a speed lever.
- **The image never leaves the GPU.** The captured Direct3D 11 texture is shared with CUDA once, and TensorRT reads
  it every frame directly. Only the small list of boxes comes back to the CPU.
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

Each step can be tried on its own. Status: not started.

1. **Recording**: save frames (and later the user's clicks and keys) to the data folder, without slowing capture.
2. **Review screen**: step through candidates and answer with one key; show the score.
3. **First-round proposer**: the general object finder for round one.
4. **Training tool**: Python, trains the detector and exports the model file.
5. **Live detection**: TensorRT in the pipeline, plus the tracker; draw the boxes in the observed view.

## Open questions

- Which Lineage 2: official (Classic, Essence or the main game) or a private server? The interface and on-screen
  markers differ. Bots break the rules on official servers and accounts get banned; on a private server it depends
  on its rules.
- Where the data folder lives, and how many frames per second to record.
- Which general object finder for round one (e.g. Segment Anything in its automatic mode).

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
