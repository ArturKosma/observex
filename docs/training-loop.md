# The training loop

How observex learns to recognise things on screen, starting with **characters in Lineage 2**: any kind (monsters,
other players, NPCs, the user's own too); telling the kinds apart is the second step. Until the evening of
2026-11-01 the category was called monsters and players did not count (the user renamed it then). This is the agreed
plan (2026-10-31). Read it before working on recording, labelling, training or recognition, and update it when a
decision changes or a step gets built.

## The idea

The user plays while observex records. Afterwards observex shows the user the things it found interesting, one at
a time, and the user says what each one is. observex trains on those answers, and the next recording it guesses
better. Rounds repeat until observex finds the characters on its own, measured by a score.

```
record  ->  propose  ->  review  ->  train  ->  detect live
  ^                                                |
  +------------------ next round ------------------+
```

1. **Record.** While the user plays, observex saves one frame every 3 seconds in the background. Nothing is analysed
   during play, so the game and the capture loop are never slowed.
2. **Propose.** After the session, observex looks through the saved frames and picks out candidate objects. In the
   first round it knows nothing yet, so a general "find every distinct object" model makes the guesses (slow, but it
   runs offline). From the second round on, observex's own trained model makes them.
3. **Review.** The user asks for "the next thing you think might be a character" and answers with one key per item:
   monster, player, NPC, the user's own character, or nothing (as built: one category at a time, e.g. characters,
   each mark answered with its kind or no; kinds since 2026-11-02). observex:
   - shows each candidate in its frame, with the box drawn on it;
   - **labels tracks, not single pictures**: the same character in consecutive frames is followed from frame to frame,
     so one answer covers it from every angle seen;
   - **asks the most uncertain items first**, so each answer teaches the most; items it is sure about need no answer;
   - **also asks about misses**: now and then it shows a whole frame and asks the user to click any character it did
     not mark.
4. **Train.** A separate Python tool trains the detector on all answers so far, on the RTX 4090 (about 15 minutes
   for 120 images), and writes a model file.
5. **Detect live.** observex runs the model on every captured frame through TensorRT (measured: 3.1 ms per frame on
   the 4090, RF-DETR Medium trained and run at 1024; run at 768 from 2026-11-01 to 2026-11-03, 2.1 ms). The next recording uses it to propose, so each round asks fewer questions.

**When is it done?** A small set of the user's answers is kept aside and never trained on. After each round,
observex reports a score on it, e.g. "finds 94% of characters, 2 false alarms per 100 frames". The user decides
when the score is good enough.

## Decisions and why

- **Full colour, no outline step.** The model sees the normal colour image. Colour costs almost nothing extra
  (only the model's first layer reads it), and it is evidence: creatures differ from grass, rock and sky in colour
  and texture. Reducing the image to outlines first would throw that away and add noise: foliage, shadows, particle
  effects and lighting all make edges, and a character's outline merges with the ground. The model learns its own
  edge and shape detectors in its first layers, better than a fixed outline filter.
  To stop it leaning on colour alone (e.g. "characters are things in green forests"), training randomly changes
  brightness, contrast and hue, which also covers Lineage 2's day and night.
- **Single frames plus tracking, not a video model.** The detector looks at one frame at a time, so it can react the
  frame a character appears. A light tracker (matching boxes between frames, microseconds) then:
  - ignores one-frame flickers (a detection must persist to count), which removes most false alarms;
  - gives each character an identity, a direction and a speed, which the playing logic will need;
  - spreads one review answer across all frames of a character (see Review).
  A model that reads several frames at once could use motion such as a run cycle, but it costs more per frame,
  needs a few frames before it can decide, and in Lineage 2 the camera moves almost constantly, so everything moves
  on screen. Many characters also stand still. Revisit only if the score shows still frames are not enough; the
  cheap first step would be giving the detector the previous frame as extra input.
- **Speed levers, in order of effect:** run only on the game's 3D view (crop out fixed UI panels), choose the input
  resolution (far characters are small, so maybe 1280 wide rather than 640), choose the model size (nano or small),
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
  the observed screen (for now: Characters). A flag only hides or shows; recognition goes on either way, also while
  the window is minimized. It pauses only while reviewing (the user asked, 2026-11-01), so the review's finder gets
  the GPU.
- **No Python inside observex** (2026-11-01). The review's finder runs the same TensorRT model as live recognition,
  in observex, instead of starting `guess.py` (about 5 s just to start Python). Python stays for training only.
- **Models:** RF-DETR or D-FINE (Apache 2.0, no strings attached). Ultralytics YOLO26 is as fast but AGPL, which
  requires observex to stay open source under the AGPL or a paid licence. Pick by measuring on the 4090.
- **Two languages, one boundary.** observex stays C++ (capture, recording, review screen, live detection). Training
  is a Python tool in `tools/` that observex starts after a review; the boundary is the model file.
- **The game can label for free.** Lineage 2 marks characters itself: the target window shows a name and HP bar,
  names can show above heads, and pointing at a character highlights it. These can confirm or create labels without
  the user. To check on the exact client version used.
- **Recordings stay out of the repo.** Frames show the user's screen and grow by gigabytes per hour; they go to a
  data folder outside the repo, chosen in the app.

## Build order

Each step can be tried on its own.

1. **Recording**: save frames (and later the user's clicks and keys) to the data folder, without slowing capture.
   **Built (frames).** Record button (and Ctrl+Alt+R until 2026-11-12); one folder per recording, named by date and time, holding
   `000001.jpg`, `000002.jpg`, ...; 1 image every 3 seconds, fixed (the user asked for no rate setting; it was 1 per
   second until 2026-11-01, when the user chose fewer images over skipping near copies in the review); only images
   the screen actually updated. JPEG quality 85: about 0.7 MB per 2560 x 1440 Lineage 2 image, about 0.85 GB per
   hour. No measurable cost to capture. Each folder holds a `recording.observex` (started, length, screen, size,
   rate, kind, images; since 2026-11-07 also when a review last changed its answers), written at start and stop, so
   the review can list recordings. Since 2026-11-02 Record asks
   which kind: every 3 s or only unsure (step 6.2). Since 2026-11-03 both stop by themselves after 110 images, with
   a progress bar and the time left (Auto, 60 every 3 s then 50 only unsure, was removed then). Clicks and keys:
   not yet.
2. **Review screen**: step through candidates and answer with one key; show the score.
   **Built (answering).** Review button, then a recording from the list (newest first, 2026-11-01; it was a
   folder picker), then the category ("What are we looking for?"; only Characters for now). Each image shows the
   finder's marks with how sure it is; the user answers yes or no per mark (click, or Y / N), drags a box around
   anything missed (also inside a bigger mark), and moves on with the Right arrow, once every mark is answered
   (2026-11-01; unanswered marks used to count as no). Moving on makes the image *reviewed*: everything in the
   category on it is marked yes.
   Answers carry forward to marks on the next image that overlap an answered one by at least half (marked
   "copied"). Every image is shown, in order.
   **Faster review (2026-11-01, the user found a 205-image recording too slow):** guesses at least 75% sure start
   as yes ("Character 92%", green), so the user answers only the unsure ones, flips wrong ones and draws misses. Why
   75%: on the user's first 64 reviewed images of a new place, 223 of 230 guesses at 75% or more were characters
   (97%), 21 of 26 at 70% to 75% (81%), about half at 50% to 70%; the model missed 21 of 312 characters. A Skip key
   (move on without saving) was tried and dropped the same day: the user preferred recording 1 image every 3 s,
   so there are fewer near copies to review in the first place. When the model moved to 768 pixels (step 5,
   *Faster*), it became less sure overall, so 75% became 65%: on the 80 held-out images (238 characters), 65% at 768
   pre-answers 196 with 3 wrong, as 75% did at 1024 (196, 1 wrong); 75% at 768 only 171. Back at 1024 (2026-11-03, the ninth model, step 4), 65% became 70%: on its 211 held-out images (1103 characters) 70% pre-answers 854 right ones and 34 that are not characters, against 825 and 42 at 768 and 65%. Not fully automatic learning (training on its own guesses): it would learn its wrong sure guesses as right and its
   misses as "not a character", exactly the cases it needs to learn; the unsure half is a coin flip only a person (or
   the game's own labels, below) can settle.
   **How to draw a box** (2026-11-01, after checking every answer; mixed habits hurt the model more than anything
   else): one box per character, tight around its body only, never the name and level above it. Every visible
   character counts, far-off ones too. A character that is fully hidden (behind another, the chat or a wall),
   or of which only the name shows, gets no box. Monsters (treasure chests too), other players, NPCs and the
   user's own character are all characters. The user's own counts since late on 2026-11-01 (the user asked: it looks
   like other players, and "no" on it confused the model): its 16 old "no" marks became tight yes boxes and 705
   reviewed images got a box on it, the old files kept as `character-answers.before-own.txt`; where it is fully
   hidden (61 images, mostly behind big monsters or in bright spells) it has none. Players count since earlier that
   evening: the 10 old "no" marks on other players became tight yes boxes, the old files kept as
   `character-answers.before-players.txt`.
   **Tight to the outline: tried and undone** (2026-11-02, the user asked: boxes dragged by hand under the name tag were
   often 1.5 to 3 times the character): SAM 2.1 (Meta's Segment Anything, hiera-large, Apache 2.0) fitted every yes box to the
   outline of what it holds. It is asked on a close-up with the box as drawn and with it 8% smaller; the fit counts
   only if SAM is at least 70% sure, the two answers agree (overlap 85%), it stays inside the box grown 6%, it keeps
   at least 40% of the box's width and height (not a target ring, a head or a weapon) and it does not take in another
   marked character. The user's own boxes from the night before could also grow downwards (some were cut at the
   waist). 2,234 of 4,113 boxes changed (they kept a median 55% of the area drawn; 8 grown ones that looked doubtful
   on the check sheets were kept as they were); the others stayed as drawn (SAM unsure or unsteady: spell glows,
   target rings, crowds, blur). The model trained on them was worse everywhere (seventh model, step 4), so the
   answers went back to the boxes as drawn the same day (the user asked). SAM fitting each box drawn in the review was
   built that day too and removed with it: the user does not want it while labelling, and fitted boxes among
   hand-drawn ones would mix the two styles again. Two overlapping characters get two boxes. A "no" on a mark that sits on a real character teaches the opposite: draw the right box instead. But a mark
   around part of a character whose whole body already has a yes mark is a no (2026-11-01, a Crypt Horror with a
   whole and a half mark): only yes marks are training boxes, so the no teaches "one box, not its parts", while two
   yeses would teach the split seen on a close succubus.
   **Kinds** (2026-11-02, the user asked, after research on labelling): a yes now says which kind of character it
   is: Monster (key 1), Player (2), NPC (3) or You, the user's own character (4); N is no, Y is yes of the kind shown.
   Each kind has its own colour (green, blue, amber, pink), so a wrong one stands out. Why: the bot attacks monsters,
   not players or NPCs, and must know which one is itself; and teaching kinds tends to help finding characters at all
   (a plain yes/no lets the model get by on shortcuts and mixes look-alikes up; papers: Tian et al. CVPR 2015,
   fine-grained training 2025). The own character is a kind of its own, not a player: merging later is one line in
   training, splitting later means checking every box again. Kinds carry forward with answers ("You · copied").
   Images answered before kinds hold plain `yes` marks: their kind is a suggestion, they count as not reviewed, the
   review opens at the first of them, the user fixes the wrong kinds (copied from the image before when the same
   thing is there) and moves on with Right, which checks every kind on the image. A one-off script suggested kinds in
   all 19 recordings (old files kept as `character-answers.before-kinds.txt`): 879 boxes as You (721 known from the
   own-character pass; in 2026-11-01_22-57-56 and the two 2026-11-02 recordings, where the user drew their own
   character, the yes box nearest the screen middle, right on 702 of 712 test images), the 10 players from the
   players pass, the other 3,631 as Monster. The user then checked every image's kinds, and since 2026-11-03 the
   live model learns them (the ninth, step 4); live, how sure it is of a character is how sure it is of one of its
   two surest kinds.
   **Undo and last reviewed** (2026-11-07, the user asked): Ctrl+Z undoes the last answer, kind, drawn or removed
   box, or move to another image (a Right that made the image reviewed is one step), last first, back to exactly how
   the image was (the answers file is saved again); one history per opening, no redo. Closing a review that changed
   the answers file writes `reviewed-<category>=<time>` into `recording.observex`, and the list shows "Last
   reviewed today, 10:48" on that recording.
   Not yet: tracks (one answer for a character across many images), most-uncertain-first order, and the score.
   Files, in each recording's folder, plain text (`src/core/Marks.h`), for the finder and the training tool:
   ```
   # <category>-guesses.txt, written by the finder: how sure it is (0 to 1), then x y width height in pixels
   image 000001.jpg
   0.81 150 312 34 42
   # <category>-answers.txt, written by review: only reviewed images; a yes is its kind
   image 000001.jpg
   monster 150 312 34 42
   own 1250 700 60 150
   no 1408 384 51 79
   # answered before kinds, kind not checked yet (optionally a suggested kind after "yes")
   yes 150 312 34 42
   yes player 915 323 22 49
   ```
3. **First-round proposer**: the general object finder for round one. **Probably not needed** (2026-10-31): the
   user hand-labels the first few dozen images (drawing every character), the first model is trained on those, and
   that model is the finder from round two on. Build a general finder only if hand-labelling turns out too slow.
4. **Training tool**: Python, trains the detector and exports the model file.
   **Built** (`tools/train/`, its README has the commands). `train.py` collects every recording with
   `<category>-answers.txt`, keeps the last fifth of each recording aside for the score (the last part, not random
   images: neighbouring images are near copies and would flatter it), fine-tunes RF-DETR Medium (pretrained on
   everyday photos; its 80 kinds replaced by the one category) at 1024 x 1024 (run live at 1024 too; at 768 from 2026-11-01 to 2026-11-03) for 40 passes, with random colour
   changes on the training images, and saves
   `models/<category>/model.pth` and `model.json` (settings, counts, score). `--kinds` (prepared 2026-11-02, live
   since the ninth model, 2026-11-03) learns the four kinds as classes from the images with checked kinds, and
   trains on images with a rare kind more often (`REPEAT_RARE`); live, the Show flag Kinds then tags each box with
   its kind. `guess.py RECORDING` writes the model's
   guesses as `<category>-guesses.txt` for the review, down to 30% sure. The review used to run it by itself; since
   step 5 it runs the same model inside observex instead (`engine/Finder`: same guesses, about 1 s to start instead
   of 5, about 20 ms per image), and `guess.py` stays as the Python reference. Python stays in a private `.venv`
   in `tools/train/`; packages pinned in `requirements.txt`.
   Learned on the way: RF-DETR's automatic batch size never finishes on Windows (the NVIDIA driver moves GPU memory
   into normal RAM instead of failing, so the "until it does not fit" search runs on); the batch is fixed at 4,
   16 images per learning step. On the 4090: about 8 s per pass over 120 images, about 7.6 GB of GPU memory;
   prediction in Python 48 ms per image (file reading included).
   **First model (2026-11-01):** 120 hand-labelled images from 3 recordings, one place each (Elven Ruins, Ruins of
   Agony, Cruma Marshlands), 298 characters; 96 trained on, 24 held out (52 characters). 100 passes in 13 min; the best
   was pass 26, it levelled off after about 20. Held-out score by how sure a mark must be: 30%: finds 45 of 52, 10
   false alarms; 40%: 43 of 52 (83%), 4 false alarms; 50%: 37 of 52, 3 false alarms (in 24 images). The held-out
   images come from the same places it trained on, so a new place may score lower; training on two places and
   scoring on the third would measure that. Checked in the review: guesses show as marks on images it never saw.
   **Second model (night of 2026-11-01, Claude alone, no game):** 5 recordings, 165 images.
   - *Answers checked one by one.* Most of the first ones boxed the name and level along with the body (Elven Ruins,
     Elven Fortress), some boxes were loose (Ruins of Agony, 2 in Cruma), some characters had two boxes, some had
     only their name showing, far-off characters and chests were missed, and three "no" marks in Elven Fortress sat
     on real orcs. All redrawn to the rule in step 2: 411 to 421 characters, 211 boxes changed. The user's original
     files stay next to the new ones as `character-answers.original.txt`; every change is logged.
   - *Two tests.* (1) A place it never saw: train on four recordings, score the fifth, for each of the five
     (AP50, 423 characters). (2) Places it knows: train on the first 70% of each recording, score the last 30%.
   - *Results, place never seen:* Small with the old answers AP50 0.62 (finds 59% at 40% sure, 86 false alarms);
     the same with checked answers 0.69 (68%); plus colour changes 0.73; Medium instead of Small 0.74 (72%, 75 false
     alarms). No better: Large 0.69, 1280 pixels 0.72, zoom changes 0.70, 80 passes 0.73 (every best pass came
     before 35), 768 pixels 0.74 (a tie: fewer far-off characters found, more in Agony). *Places it knows:* AP50
     0.76 to 0.87, finds 78% to 82% at 40% sure, false alarms 28 to 17.
   - *Final model:* Medium, 1024, colour, 40 passes, 6 minutes, 12.6 GB of GPU memory, on 132 images (357
     characters). On the 33 held-out images (64 characters), at 40% sure: finds 58 (91%), 2 false alarms; at 30%: 60
     (94%), 5. The first model on the same images: 45 (70%), 11 false alarms. What it still misses: see-through
     Spartoi in Ruins of Agony (seen at 18% to 34% sure), an eye peeking out of a tree; one false alarm is a rock.
     The weakest place by far is Ruins of Agony when it is not trained on (18 of 70): more reviewed recordings,
     Agony above all, will help most.
   - *Third model (2026-11-01, 09:45, the user's new reviews):* 8 recordings, 398 reviewed images (3 new places:
     Ruins of Despair, 205 images reviewed with the 75% pre-answer; a grassy field with Werewolf Hunters; a rocky
     path; 953 of the new 1,050 characters were the model's boxes the user kept, 97 drawn). Same settings; 318 images
     (1,233 characters) to train on, 16 s per pass, 12 minutes. Both models scored on the same 80 held-out images
     (238 characters), which neither trained on: AP50 0.920 → 0.960; at 40% sure finds 192 (81%) → 219 (92%), false
     alarms 16 → 14; at 75% (what the review pre-answers) 117 (49%) → 196 (82%), 1 wrong either way. Old places
     alone: unchanged (58 of 64, 2 false alarms). Ruins of Despair: 76% → 92% at 40%, 43% → 87% at 75% (partly
     because it now knows the place). Of the 14 false alarms, 7 are characters missing from the answers (e.g. a
     Veelan Bugbear Warrior by the posts in images 196 to 202 of 2026-11-01_08-48-08), 3 are marked characters with a
     box of another size, 1 a dying character, and 3 are players (the user's own character, another player twice):
     the only real mistakes. observex's TensorRT copy: 218 of 238, 15 false alarms at 40%; 5.1 ms per image live.
     The second model is kept in `models/character/previous-2026-11-01-0621/`.
   - *Fourth model (2026-11-01, 13:33, the user asked to retrain on 3 new recordings):* 11 recordings, 375 images
     (1,353 characters) to train on, 15 minutes; 94 held out (273 characters). Three held-out yes boxes cover the
     minimap (2026-11-01_13-03-47 images 45 to 47: the third model's 79% guess, pre-answered yes), so the score
     leaves them out (270). At 768, third → fourth: AP50 0.900 → 0.895; at 30% sure 234 and 29 false alarms →
     232 and 28; at 50% (where the tracker starts) 220 and 17 → 227 and 10; at 65% (pre-answered) 208 and 10 →
     212 and 5. Kept for the 50% and up gains. Per place: the new Cave of Trials (dark, see-through ghosts and
     Ghost Guardians, 13-03-47) finds only 10 of 30 held-out characters with either model, but false alarms there
     went 9 → 0 at 50%; more recordings of it will help most. observex's TensorRT copy gives the same counts
     (227 and 10 at 50%), 3.1 ms per image on files. The third model is kept in
     `models/character/previous-2026-11-01-0944/`.
   - *Fifth model (2026-11-01, 22:41, the user asked to retrain on 4 new recordings, and renamed the category from
     monsters to characters: other players and NPCs count, the user's own character never):* 15 recordings, 566
     images (2,231 characters) to train on, 23 minutes (the game was running on the same GPU); 142 held out (494).
     Before training, the 10 old "no" marks on other players (MpOLaRiS in 2026-11-01_01-14-08, Nojiko and
     JohnnyeSinsOLD in 2026-11-01_09-28-21) became tight yes boxes; the NPC "no" marks were all loose boxes next to
     tight yes ones, so they stayed. Both models on the same 142 held-out images, at 768, fourth → fifth: AP50
     0.669 → 0.919; with at most 20 false alarms 280 → 417 found; at 50% 276 and 16 → 435 and 40; at 30% 303 and 44
     → 445 and 62. Per place at 50%: Cave of Trials (13-03) 10 → 23 of 30, the town and Gremlins (21-26) 15 → 111
     of 127, 21-56 12 → 40 of 42. Of the 40 false alarms at 50%: about 5 are the user's own character (new: it now
     sometimes takes it for another player), 3 are other players not marked in those images (MpOLaRiS in 01-14
     images 27 and 28, JohnnyeSinsOLD behind the inventory in 09-28 image 14), about 17 are real characters with a
     box of another size (Ghost Guardians in their glow, double boxes), the rest corpses, carts, UI glow and floor.
     observex's TensorRT copy: 6 ms per image on files with the game running. The fourth model is kept in
     `models/character/previous-2026-11-01-1333/`.
   - *Sixth model (2026-11-02, 00:16, the user asked to retrain on 2 new recordings, with the user's own character
     counted from now on):* 17 recordings, 683 images to train on, 39 minutes (SAM was tightening boxes on the same
     GPU); 171 held out (790 characters, 141 of them the user's own). Fifth → sixth on them, at 768: AP50 0.753 →
     0.908, at 50% 553 → 689 found (false alarms 44 → 54). The user's own character: 0 → 122 of 141 found at 50%.
     The other characters alone: AP50 0.911 → 0.905, at 50% 553 → 567 of 649 found, false alarms 44 → 54 (learning
     the user's own did not cost the others). The fifth model is kept in `models/character/previous-2026-11-01-2241/`.
   - *Seventh model, on the tightened boxes (2026-11-02, 00:42, the user asked to see whether the tight boxes pay
     off):* the same images, 25 minutes. Worse everywhere, so the sixth stayed live. Each scored in its own box style
     on the same 171 held-out images (790 characters), at 768: AP50 0.908 (sixth, boxes as drawn) vs 0.712 (seventh,
     tightened); at 50% sure 689 found and 54 false alarms vs 592 and 149; even at a strict 75% overlap the sixth
     found more (474 vs 358). It lost on both kinds of box: the characters SAM tightened 91% vs 79% found, and the
     365 it left as drawn (the same truth for both) 82% vs 70%. Likely why: half the boxes became tight and half
     stayed loose, by SAM's sureness, which the model cannot see, so it learned neither style well (the box rule's
     lesson again: mixed habits hurt most). The seventh model is kept in `models/character/tight-2026-11-02-0045/`;
     the answers went back to the boxes as drawn and the sixth model is current.
   - *Eighth model (2026-11-02, 23:13, the user asked to retrain with the learning rate lowered toward the end):*
     19 recordings (the orc place and the spider), 754 images to train on (3628 characters), 37 minutes; the learning
     rate now falls along a cosine to nothing by the last pass (`LR_SCHEDULE`; RF-DETR's default step comes after 100
     passes, so in 40 it never fell). 189 held out (892 characters): the sixth's 171 plus 18 orc images. Sixth →
     eighth at 768, all 189: AP50 0.913 → 0.924, at 50% sure 780 → 775 found, false alarms 64 → 47. The sixth's 171:
     AP50 0.908 → 0.918, 689 → 683 found, false alarms 54 → 45. The 18 orc images: 91 → 92 found, false alarms
     10 → 2 (it trained on the first four fifths of that place). Strict overlap (AP50:95 0.556 → 0.547) favours the
     sixth, whose own boxes the orc answers are (pre-answered yes keeps the model's box: 98% overlap). One run, so the
     gain mixes the schedule and the new images; a difference this size is near run-to-run noise. The sixth model is
     kept in `models/character/previous-2026-11-02-0016/`. The RF-DETR docs suggest 50 to 100 passes for 500 to 2000
     images (we run 40); tested with the ninth: no better.
   - *Ninth model, the first with kinds (2026-11-03, 06:30; the user checked the kinds of every reviewed image and
     asked, before a night away, to retrain, compare and find what else improves it):* 20 recordings (the NPC town
     2026-11-02_23-46-18 is new), 845 images to train on (4,241 characters: 2,781 monsters, 822 the user's own, 545
     NPCs, 93 players), 211 held out (1,103), 35 minutes. Eleven trainings that night, each scored on the same 211
     held-out images at 768, each thing read once, as observex reads it (AP50, then found and false alarms at 50%):
     - *Kinds against one category*, the same recipe: AP50 0.872 against 0.879 (seed 1: 0.872 against 0.879), at
       50% 894 and 70 against 893 and 78; at 1024 0.899 against 0.901. On places it trained on, kinds cost about
       nothing. Of those found, the right kind on 97%: monsters 99%, the user's own 100%, NPCs 86%, players 4 of 12
       (only 16 players in the held-out images).
     - *How sure, with kinds.* Read by its surest kind alone, a kinds model lost things it could not tell apart: it
       scores each kind on its own, so an NPC it half takes for a monster is 40% one and 35% the other. Since this
       model, how sure it is of a character is how sure it is of one of its two surest kinds (`eitherKind` in
       core/Detection, 1 - (1 - first)(1 - second)): AP50 0.870 → 0.872 here, 0.879 → 0.897 on the new place below.
     - *A new place* (2026-11-02_23-46-18 kept out of training whole: 110 images, 558 characters, 372 of them NPCs and
       49 players, more than the rest of the recordings together): one category AP50 0.923 (469 found at 50%, 45
       false alarms); kinds by the surest kind 0.879 (297), with eitherKind 0.897 (400, 36), and with rare kinds
       repeated 0.912 (459, 50). At 1024: 0.936 against 0.933. There the kind is right only about half the time: it
       takes the new place's NPCs for monsters (184 of 290 found) and its players for monsters or NPCs. The kind gap
       (`kKindsClose`) flags 68 of those 230 as unsure, so Only unsure records them for the review. (The live eighth
       looks best there on strict overlap, AP50:95 0.843: the user reviewed that recording from its guesses, so its
       boxes are the answers.)
     - *Rare kinds repeated* (`REPEAT_RARE` in train.py, LVIS's repeat-factor sampling: images with a player count
       about 2.6 times, with an NPC 1.6; 845 → 1,026 images a pass): AP50 0.873, false alarms at 30% 156 → 135, the
       right kind 96.6% → 97.1% (at 1024 97.9%; NPCs right 94 → 100). Kept.
     - *No better:* 80 passes (AP50 0.871; at 50% more found, 919, but more false alarms, 80, and looser boxes, AP75
       0.534 → 0.502), training at 1280 (0.854; run at 1024 0.883 against 0.899), and Large (0.867, against
       Medium's 0.872 with the same recipe; run at 1024 0.896 against 0.899).
     - *Running at 1024 instead of 768* is the biggest lever: AP50 0.873 → 0.902, at 50% 892 → 939 found with fewer
       false alarms (69 → 61), the right kind 97.9%; 960 is nearly as good (0.895), 1152 no better (0.898). It costs
       about 1.6 times the GPU time: live on the desktop 2.0 → 3.2 ms per image (still all 240 images a second; with
       the game on the same GPU, where a look took about 10 ms, fewer looks a second). The user switched to
       it the same morning (`LIVE_RESOLUTION`; live 3.1 ms per image, all 240 images a second), and the review's
       pre-answer moved from 65% to 70% (854 right and 34 not characters, against 825 and 42 at 768 and 65%).
     - *Against the eighth* on the same 211 images (it trained on none of them): AP50 0.874 against 0.873, at 50% 886
       and 49 against 892 and 69, at 65% (pre-answered) 808 and 31 against 825 and 42. Most of the extra false
       alarms are in 2026-11-01_08-48-08: far-off things the eighth was 15% to 20% sure of; some are characters
       missing from the answers (a creature on the scaffold in image 204, a figure in the fog in 176), some are not
       (a stump with an axe in 200, a shape on a roof in 167).
     - observex's TensorRT copy gives the same counts (at 768 896 and 66 at 50%; at 1024 935 and 64, against
       Python's 939 and 61), 6.8 ms per image on files while a training ran. Only unsure, with the mirror test (0.17)
       and the kind gap (0.2) kept, saved 134 of the 211 held-out images at 768, which held 82% of its mistakes and 24
       of its 26 wrong kinds. At 1024 the model sees the mirrored image more alike (median 0.255 → 0.197) and 0.17
       saved only 118 (56%: 72% of its mistakes, 16 of its 20 wrong kinds), so the mirror cut-off became 0.11: 138
       (65%), 81% and 19 of 20. The eighth model is kept in
       `models/character/previous-2026-11-02-2313/` (with its dataset, training folder and engine).
   - *Tenth model (2026-11-07, 12:04; the user reviewed three of the 2026-11-06 recordings and asked to retrain):*
     the ninth's recipe on 24 recordings, 1197 images to train on, 299 held out, 46 minutes. On those 299 (neither
     trained on any) AP50 0.884 → 0.899, at 50% 1644 → 1710 found (false alarms 107 → 132), the right kind 75% → 96%;
     the gains all on the new recordings. Talking Island 140 → 157 of 166, kind 43% → 98% (the ninth had 93 players
     to learn from, the tenth 1607). The ninth is kept in `models/character/previous-2026-11-03-0630/`.
   - *Eleventh and Large (the night of 2026-11-08, the user asleep; the eleventh live from 09:50 that morning):* the tenth's
     recipe on 26 recordings (the two of 2026-11-07 added), 1373 images to train on, 343 held out (2183
     characters), 54 minutes; and the same with `--model large`, 53 minutes. On those 343: tenth AP50 0.906, at 50%
     1887 found / 136 false alarms, right kind 95.0%; eleventh 0.908, 1893 / 138, 94.8%; Large 0.907, 1883 / 118,
     94.6%. All within run-to-run noise. Where the new images are, the eleventh gets the kind right on 95% instead of
     82% (2026-11-07: the user's party taken for monsters 29 → 5 times), a little worse elsewhere (the 11-03 and 11-06
     places 94% → 92%). Large is as fast live (TensorRT on files: 4.5 against 4.7 ms per image) and trades 10 found
     for 20 fewer false alarms. The tenth's held-out curve was flat after pass ~19 of 40. So size, passes and recipe
     are not the lever now; more reviewed places are. The user switched to the eleventh in the morning (live check:
     its saved engine loads in 0.4 s and gives the same guesses as in this test). The tenth is kept in
     `models/character/previous-2026-11-07-1204/` (with its dataset, training folder, engine, and its export from
     before the features output in `before-features/`).
   - *Twelfth (2026-11-08 16:20-17:31, live from 20:25 that evening):* the first with all six kinds (Can't tell and
     Dead), after 92 corpses in 13 recordings were relabelled dead the same afternoon; 30 recordings, 1725 images to
     train on, 431 held out (2959 characters), 70 minutes. Against the eleventh on those 431, corpses and can't-tells
     left out: AP50 0.928 → 0.937, at 50% 2586 / 151 → 2604 / 164 (13 called dead, mostly unmarked corpses), at 65%
     2430 / 90 → 2458 / 89, right kind 95.1% → 96.4%; more eager under 50% (at 30% 307 → 422 false alarms: second
     boxes on found characters, a few repeated junk spots). Corpses at 50%: 12 of 20 dead, 5 monster, 3 missed; 3
     living monsters called dead (2 walking keltirs). As fast. The eleventh is kept in
     `models/character/previous-2026-11-08-0202/` with its kind update.
   - *Speed* (RTX 4090, Python, image already in memory): Small, Medium and Large all about 26 ms at 1024 (the
     resolution sets the cost, not the size); 768 pixels 18 ms; half precision, compiled, 14 ms with the same
     guesses (`guess.py` uses it). Run trainings one at a time: two at once filled the 24 GB, Windows moved GPU
     memory into normal RAM, each ran twice as slowly and one hung.
   Exporting: `export.py` writes `models/<category>/model.onnx` (half-precision weights, full-precision input and
   output) for live detection; `train.py` runs it at the end. Not yet: starting training from observex's UI.
5. **Live detection**: TensorRT on every captured image, plus the tracker; draw the boxes in the observed view.
   **Built (detector and boxes), 2026-11-01.** `engine/Detector` runs `model.onnx` through TensorRT 11.4 (FP16):
   a compute shader squeezes the captured image to the model's size, 1024 x 1024 (768 from 2026-11-01 to 2026-11-03; bilinear, ImageNet colours, as RF-DETR does) into a
   buffer CUDA shares with Direct3D 11, TensorRT runs the network on it, and only the 300 candidate boxes come back
   to the CPU. `engine/Recognizer` runs it on its own thread (above normal priority) and its own GPU device, on the
   newest captured image, and hands the boxes to the UI lock-free. Starting it on a screen loads the saved engine in
   0.4 s; a new model is first prepared for the GPU (35 s on the 4090, progress shown). In the app: the **Show**
   dropdown (eye icon, right of the screen button) has a **Characters** flag (saved in observex.cfg as
   `show-characters`), and under it how recognition is doing ("3 found · 210 images a second · 4.7 ms each", or the
   preparing progress, or why it cannot run). With the flag on, each character found at least 30% sure gets a violet
   box tagged "Character 81%". F3 adds the recogniser's numbers.
   - *Same answers as Python:* on the 33 held-out images, the 65 characters Python finds at 30% are all found; 2 more
     sit right at the cutoff (30.3%, 30.5%); sureness differs by 0.003 on average (0.017 at most), boxes by 4 pixels
     at most.
   - *Speed (RTX 4090, Release):* 4.7 to 5.0 ms per image from captured image to boxes (Python half precision:
     14 ms); about 210 of the 240 images a second; capture unchanged with it running (239.7 images a second, 0.05 ms
     latency; the hand-over costs 0.09 ms of pipeline work per image).
   - *The GPU is busy:* at 210 images a second the model keeps the GPU nearly fully used. Other GPU work slows: the
     review's finder took 37 ms per image instead of 5 next to it, so recognition now pauses while reviewing (the
     user asked). A game on the same GPU will have to share it too; if its frame rate drops, a limit on looks per
     second is the fix (not built yet).
   - *The review's finder* (`engine/Finder`) runs the same Detector over a recording, in observex, instead of
     Python's `guess.py`: the same 40 guesses on a 30-image recording (sureness within 0.002 on average, boxes
     within 2 pixels), about 1 s to start instead of 5.
   - *Live log* (2026-11-01; the user sees sureness high at some moments, 30% to 40% at others, and one-image
     blips, and wanted to show Claude): while recording, every look goes into `character-live.txt` (everything at
     least 10% sure), tied to the saved images by screen image number, with Ctrl+Alt+M marks until 2026-11-12 (a Settings
     checkbox, on by default, turns it off). On a 15 s test the same character ranged from 20% to 88% sure (median
     84%). The user's first fight log (17.5 s): far characters standing apart hold 85-90% (the model's ceiling is
     about 90%); the small dark Nightmare Weaver wanders 40-85%; close-ups cut by the screen edge and piles of
     characters swing 20-90% and blink (box count changing on about a third of looks).
   - *Tracker* (`core/Tracker`, 2026-11-01): follows each character from look to look (identity, speed, smoothed
     sureness). A new one starts only from a box at least 50% sure, then counts after 3 looks in a row; a missed one is held where it is heading, as long as it
     was seen, up to 150 ms. On that fight log, boxes appearing or vanishing went from 21.1 to 3.8 a second, with
     none shown for one look and 99.6% of what the model saw still covered; 0.2 microseconds per look. Rules
     chosen by replaying the live logs (`observex_bench --track LOG`). What is left is crowds and close-ups: only
     8% of the labelled characters are 250 px tall or more, so recordings of fights up close help the model most.
     The 50% start (2026-11-01, for a box blinking at 30-40% on a static ornament and parts of a close succubus):
     fight log 3.8 -> 2.7 changes a second, the 12-56 log 6.0 -> 3.0, the 13-03 log 3.2 (40.5 raw).
   - *Faster* (2026-11-01; the user wants room for more recognitions: walls, ground, text, party members): the
     network now runs as one CUDA graph, 4.7 -> 3.3 ms per image live (235 of the 240 images a second), the same
     guesses. TensorRT's builder level 5 was slower. Then 768 pixels instead of 1024 (the user agreed): the model
     still trains at 1024 and is run at 768 (`LIVE_RESOLUTION` in train.py), 3.3 -> 2.1 ms per image live, all
     240 images a second, the GPU about half busy. On the 80 held-out images (238 characters), AP50 0.960 -> 0.957;
     at 30% sure (what the tracker takes) 222 found, 18 false alarms -> 220 and 14; at 40% 219 and 14 -> 214 and
     9. Retrained at 768 it did worse (AP50 0.931, 215 and 31 at 30%), so it was not kept. Back to 1024 on 2026-11-03 (the ninth model, step 4): with more and harder images it scores clearly better there (AP50 0.873 → 0.902), 2.0 → 3.1 ms per image live, still all 240 a second. observex's TensorRT
     copy gives the same counts as Python. Left: FP8 or INT8 (needs calibration images, may cost accuracy). For
     more categories, one model with more classes costs
     about the same per image as one class; surfaces (walls, ground) need a segmentation head instead of boxes,
     and text only needs reading inside small regions (name tags), not the whole screen.
   Not yet: the playing logic.
6. **Learn while observing** (the user's idea, 2026-11-01; agreed order). The model already runs while the user
   watches the game; when it is unsure about something, it saves a question for the user to answer later, and each
   answer improves the model without leaving the app, so the user can walk back to the same place and see the
   change. Training is the slow part (minutes on the GPU), not swapping the weights: TensorRT can *refit* an engine
   with new weights of the same network in about a second, without rebuilding it.
   1. Live detection with show flags. **Built** (step 5).
   2. Questions while observing: images where the model is unsure (marks between about 20% and 60% sure, or flicker
      between frames) are saved with its guesses, to answer later in the review.
      **Built (2026-11-02), as a kind of recording.** The user worried plain Yes / No boxes on every 3 s image teach
      too little; the answer was to ask about more of what the model gets wrong. Record asks "Every 3 s" or "Only
      unsure": only the image right after a look with 2 or more things 30% to 65% sure (what the review pre-answers
      no further, so it asks about them), at most one every 3 s. The live logs were replayed first (11 logs, 27
      minutes), counting unsure marks per saved image:
      - every 3 s: 20.2 images a minute, 1.0 unsure marks each
      - 1 or more unsure: 19.2, 1.6 (the model is unsure of something on nearly every look)
      - 2 or more: 16.7, 2.4; 5% of the images more than 6 s apart, the longest 16 s
      - 3 or more: 13.6, 3.3; 11% more than 6 s apart, up to 30 s
      Two it is: about 2.4 times the questions per image, at much the same pace. It is still Yes / No on boxes; what
      changes is which images the review gets. Flicker (a thing sure on one look and gone on the next) is not used
      yet. Tested with the bench (`--unsure`): on a busy screen one image every 3.0 s, each after a look with 3 or 4
      unsure; on screens with nothing unsure, none.
      **Which signal finds the mistakes (2026-11-02, the user asked for a better way to record).** On the 233
      reviewed images of recordings the fifth and sixth models were live in (they never trained on them): the live
      model's mistakes at 50% sure against the answers were 71 false alarms (36 of them 65%+ sure, above the unsure
      band) and 286 misses (178 with nothing seen there at all). Keeping half the images, ranked by a signal, keeps
      this share of the mistakes:
      - random: 47% (false alarms 38%, sure ones 44%, misses 50%, unseen misses 50%)
      - unsure things: 64% (70%, 42%, 62%, 47%)
      - the model disagreeing with itself on the mirrored image (as CALD, Yu et al. 2022, picks images): 64% (82%,
        72%, 59%, 46%): the only signal that finds the sure false alarms (the rocks)
      - no better than random: flicker in the last second, the biggest sureness swing, a new-looking place (32 x 18
        thumbnail distance to earlier recordings). Near copies of the image before (a quarter of all 3 s steps) hold
        as many mistakes per character as the rest (0.62 vs 0.58), so skipping them gains nothing.
      Misses with nothing seen are found by no signal: only plain timed images and the user's drawing find them (on
      these recordings; on the first new place the mirror test found most of them, see Auto below).
      Scripts: the session scratchpad's rename/signals.py, novelty.py, flip.py.
      **The mirror test replaced the unsure count (2026-11-02, the user said "Yes, build it").** For the eighth
      model on its 189 held-out images (rename/flip8.py, combo.py), keeping half: mirror 71% of the mistakes (sure
      false alarms 60%), unsure things 68% (43%), random 52% (50%); mixing both (rank average or the higher rank) did
      no better than the mirror alone (70%, 67%), on the fifth and sixth's live recordings too (63%, 65% against
      64%). Keeping a third the two were level (51%). So Only unsure now asks, when an image is due, whether the
      mirrored look disagrees by at least 0.17 (the eighth's median; 0.11 since the switch to 1024 pixels, 2026-11-03, below; the fifth and sixth's on their live recordings
      was 0.21): yes saves the next image, no skips that step, so about half the 3 s steps save. The C++ test agrees
      with Python's on the same images (correlation 0.97, the same answer on 98%; bench median 0.167, 94 of 189
      saved). Its answers go into the live log as `mirror` lines, so the cut-off can be retuned on real recordings.
      **Unsure which kind (the user asked, 2026-11-03: "the spread of % between kinds" as a trigger).** Built before
      any model was trained with kinds, then measured on the ninth (step 4). With a kinds model the Detector keeps each
      thing's two surest kinds, and Only unsure also saves when either look holds a thing at least 30% sure whose two
      kinds are less than 0.2 apart (`kKindsClose`, e.g. Player 55% and You 40%): margin sampling, the classic
      uncertainty signal for telling classes apart, which the mirror test misses when the model is torn the same way
      in both looks. And the mirror test compares how sure each look was of the same kind, so a Monster in one look
      and a Player in the other counts as disagreement (CALD scores class consistency too). In the review, a guess
      whose kinds are that close is not pre-answered; its tag names both ("Monster 55% or Player 40%?"). Both make
      more images pass. Measured on the ninth's 211 held-out images at 768: the kind gap alone flags 48 (23%), with 11 of its
      26 wrong kinds; the mirror test alone 130 (62%), with 22; together 134 (64%), with 82% of its mistakes and 24
      of the 26 (a random 64% would hold 64%). On a new place, where kinds go wrong most, the gap flags 68 of 230
      wrong kinds. Both cut-offs kept at 768 (at the ninth's own mirror median, 0.255, 54% would be saved, with 75%): the
      user wants more unsure images, not fewer. At 1024 (live since 2026-11-03) the mirror cut-off became 0.11, to
      save as many (65%, holding 81% of its mistakes and 19 of its 20 wrong kinds; step 4). Still once per 3 s step: a torn moment between two steps is not
      seen.
      **Auto (the user asked, 2026-11-02; removed 2026-11-03, the user asked):** was the routine for a new place:
      60 images every 3 s (meant as the only images that show what the model misses altogether), then 50 only
      unsure, then it stopped. The user's first new place (2026-11-02_23-46-18, the eighth live; the session
      scratchpad's auto_phases.py) showed that reason did not hold there: the mirror test (0.17) would have kept 38
      of the 60 every-3 s images, holding all 23 false alarms (10 of them 65%+ sure), 31 of the 35 misses and 15 of
      the 18 with nothing seen; the 22 skipped were quiet (2.3 characters each against 4.7, 4 misses in all). In
      its only-unsure part 50 of 74 due images were saved. Misses with nothing seen came in busy scenes, which the
      model also sees differently mirrored, unlike on the fifth and sixth's recordings above (one recording, 18
      such misses). So per image to review, only unsure finds more of every kind of mistake, at about 8 minutes of
      walking for 110 images instead of 5.5. Now both kinds stop after 110 images (`kRecordImages`) and the Stop
      button shows "38 of 110 images · about 4 min left" with a progress bar. Every 3 s stays for an unbiased look
      now and then, since what only unsure skips is never saved. 12 to 15 new places then make the 1,200 to 1,500
      more images the research suggested. Since 2026-11-12 (the user asked) only unsure records for one minute,
      asking once a second and saving only when unsure (at most 60 images, none while the model is sure).
   3. A background trainer: after answers, fine-tune from the current model in the background, export, and refit the
      running engine, so the boxes change live.
   4. **Learning in the review (the user asked, 2026-11-08: "better auto-guesses with each picture in a review").**
      **Built**, a first, review-local form of learning while answering. A party member was taken for a monster on
      about 40 images in a row; carrying answers forward missed it once it moved. Now, entering each new image of a
      review re-fits only the model's kind classifier (4 x 256 weights and 4 biases, from the model's own, pulled back
      towards them) to every guess answered so far in that recording, in about 17 ms at 2,000 answers. The boxes, and
      how sure a guess is of a character, stay the model's. Nothing is saved: the next review starts from the model
      again, and only training (step 4) makes it last. Details in CLAUDE.md, under Review.
      Pre-answering needs the model and the re-fit to agree at 78%, or the re-fit alone at 90% (85% until the
      afternoon of 2026-11-08, the user's pick after the balancing test below), with a pull of 0.003
      (the night of 2026-11-08; first 95% and 0.01, which learned little). Simulated in Python as whole reviews with
      answers carried forward, guesses pre-answered right, wrong, and asked: the tenth on the two 2026-11-07
      recordings it never trained on 880, 38, 206 against 807, 38, 279 for the model alone; the ninth on 4 places it
      had never seen 1440, 107, 1821 against 1049, 950, 1369 (it pre-answered many wrong kinds there). Replayed in C++
      on 2026-11-07_12-57-55: 438, 28, 153 against 386, 23, 210; Python on the same images 442, 26, 152 against 385,
      23, 212.
      What it cannot do: find what the model misses altogether, or drop a box; only training changes those.
   5. **The same for the whole model, between trainings (tested 2026-11-08, the user asked whether the review's
      learning could make the whole model better; built the same day as the kind update, details in CLAUDE.md under
      Live recognition).** The tenth's 300 candidates and their features on the
      eleventh's training and held-out images (the tenth never trained on the two 2026-11-07 recordings in them),
      its kind classifier re-fitted to all the answers, scored on the 343 held-out images as models are compared:
      - Re-fitting the classifier for everything (how sure of a character too) moves found and false alarms
        together: like the review (guesses at 30% and up) the right kind on 2026-11-07 82% -> 88-91% but false
        alarms 136 -> 188 and up; with every candidate, fewer false alarms and fewer found (1772-1863 against 1887).
        AP50 never better than within noise, so not that.
      - Re-fitting only which kind (how sure of a character stays the model's; learned from the yes answers, 300
        steps, no pull needed with ~9,000 answers; 1.2 s on one CPU thread): found and false alarms unchanged, the
        right kind 95.0% -> 96.1%; on 2026-11-07 82% -> 93% (players taken for monsters 29 -> 10; the eleventh,
        trained on them, 95% and 5), elsewhere the same or 1 point better. From the guesses at 30% and up only (what
        the finder keeps features for): 96.0%, 92%. The 2026-11-07 answers alone reach 95% there but forget the
        rest (older places 97% -> 77%), so it fits all answers.
      - On the eleventh right after its training it gains nothing (94.8% -> 94.8-95.1%): it helps only with
        answers given since the last training. Scripts in session eb3320e13e239d79's scratchpad, headfit/.
      - Simulated as built (pick+own: how sure of a character stays the network's, the kind and its sureness from
        the update): on the tenth's 343 held-out images found / false alarms unchanged (1887 / 136), the right kind
        95.0% -> 95.9%, on 2026-11-07 82% -> 92%; pre-filled at 78% about as before (1384 right / 66 wrong / 762
        asked against 1370 / 67 / 775); images unsure of a kind 89 -> 46. The review's re-fit started from it, on
        2026-11-07_13-09-21 with an update that knew 2026-11-07_12-57-55: 453 / 10 / 41 against 438 / 12 / 54.
        Right after a training it was a little worse in the review (26 -> 32 wrong), so it is made only from answers
        newer than the model and ignored once the model is newer.
      - **Built** (2026-11-08): after each review closes, observex re-fits which kind on every reviewed answer's
        features (300 steps, no pull, from model.json's classifier, never from the last update), saves it as
        `models/<category>/kinds-update.json`, and live recognition, the finder and the review use it until the
        next export. While a review is open a refresher re-makes the other reviewed recordings' stale guesses and
        features on the GPU, which the review frees. Measured in C++ with the tenth on copies of the 27 reviewed
        recordings: refreshed in 46 s (1,826 images), 11,083 answers fitted in 485 ms (PyTorch 1.2 s for ~9,000),
        weights within 1.3e-5 of a PyTorch fit, the same kind as Python's on 99.8% of the held-out guesses (the rest
        from the half-precision engine); about +0.05 ms per look (4.46 against 4.41 ms), sureness and boxes
        identical. The review's replay of 2026-11-07_13-09-21 in C++, from an update made without it: 451 / 13 / 39
        against 438 / 13 / 52.
      - **Fixed** (2026-11-08): the review's own re-fit paired an answer with a guess only by the same box, so after
        a model change (guesses made again) a partly reviewed recording's answers barely counted. It now pairs them
        by overlap like the kind update (`addAnsweredGuesses`): 40 answered images of 2026-11-07_13-09-21 with the
        eleventh, learned from 2 answers before, 215 now.
      - **Fixed** (2026-11-08, from the review log of 2026-11-06_21-59-42): an answer carried forward goes by screen
        position, so after the camera moved a "no" on a lantern or on a box over two characters landed on a real
        player the model was 96-100% sure of, and it was saved as no without the user seeing it (000024, 000085). A
        "no" no longer carries onto a guess the review would start as yes; it is asked. Replay of that review with
        learning and carrying: 1141 / 34 / 357 before, 1138 / 32 / 362 now (the 3 fewer right are duplicate boxes on
        the user's own character, where the copied "no" was right).
      - **Tested, not built** (2026-11-08): a balanced re-fit, answers of rare kinds counting more (the user's answers
        are about 3 in 4 players, and in that review the re-fit pulled real NPCs to player). Replayed on all 29 reviewed
        recordings from the model's own classifier (not the kind update, which had learned these answers), pre-filled
        right / wrong / asked: as built 11126 / 339 / 2416; balanced halfway (each answer counts (N / (C n))^0.5)
        10809 / 287 / 2785; fully 10300 / 261 / 3320; only the kinds balanced, no as before, 10753 / 320 / 2808. NPCs
        pre-filled as players 16 -> 11 at best. Raising kPresumeLearned does the same or better: 90% 290 / 2751, 95%
        252 / 3247 (NPCs as players 11). So balancing is only a roundabout "more careful"; how careful is
        kPresumeLearned's to set. Asking the less sure of two pre-filled guesses that overlap (duplicates) changed
        2 to 7 wrong: the wrong pre-fills answered no (235 of 339) are mostly boxes over two characters or part of
        one, and things like lanterns, not duplicates.
      - **Not done** (2026-11-08, the user's choice): running the model at more pixels for far-off characters and
        groups. On the eleventh's held-out images (2183 marked), far-off ones under 40 px tall were found 54% at 1024,
        58% at 1280, 61% at 1536, about the same in all (86.8%, 87.5%, 87.0%), and boxes over two or more characters
        30, 28, 24, for about 1.3 and 1.8 times the time. The user cannot tell such blobs apart either, so instead:
      - **Built** (2026-11-08, the user's idea): "can't tell". Live, a character the model is under 60% sure of the
        kind of is tagged "Character?" in silver (`cantTellKind`), for the bot's rules ("something far off, go and
        see"). In the review, "Can't tell" (key 5) answers a character too far off to tell, saved as `unknown`, which
        training learns as a fifth kind (`KINDS`); until then the review's learning and the kind update leave those
        answers out rather than take them for no, and the eleventh still fits the review's kinds (`kindsFit`).
      - **Built** (2026-11-08, the user asked after their review of 2026-11-06_21-09-47): "Dead" (key 6, saved as
        `dead`), a sixth kind for corpses, which were answered no until then. In that review corpses were 6 of the 8
        things the model was sure of (60%+) that were no characters, shown live as "Monster 89%" or "Player 78%",
        and 3 of its 8 wrong pre-fills. Why a kind and not no: the model sees a corpse as a character anyway, and the
        bot is to know it is dead. Why no second model to tell dead from alive: the kind classifier already is one,
        on the network's own features. Tested first (scratchpad dead/ of that session): 34 corpses from 10 recordings
        (found by eye among confident no's that touch no character) against 5296 living monsters, a logistic
        regression on the eleventh's features, one recording held out at a time: catching 50% / 80% / 90% of the
        held-out corpses calls 0.6% / 1.8% / 2.6% of living monsters dead; the box's width / height alone 7% / 19% /
        25%. Some werewolves answered no were kneeling, likely dying: left out, the user's rule for those is open.
        Older recordings' corpses stay no unless re-answered.

## Open questions

- Who labels long sessions: decided (2026-11-01). The user labels, in the review, with the model's boxes
  pre-answered at 75%. Their answers are the data as given: no AI pass that cleans or second-guesses them (an
  in-app "check your answers against the new model" was proposed and declined).

- Which Lineage 2: the first recordings (2026-10-31) show a private Interlude server (L2 Reborn, "Reborn Eternal
  Interlude x10"). Not yet confirmed by the user as the target. There, monsters show "Level: N" and their name above
  their heads (the "Level:" line is this server's own feature, not on every server: nothing should rely on it), and the targeted one also gets red markers beside its name and the target window at the top; these
  are the free labels described above. A private server's own rules decide whether bots are allowed.
- Data folder: decided. `recordings/` next to observex.exe by default, changeable in Settings; 1 image every 3 seconds.
- Round one: decided for now. The user draws the boxes on the first few dozen images themselves (varied places,
  distances and character kinds, every character on each image), then the first model is trained. A general object
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
