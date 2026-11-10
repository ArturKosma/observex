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
recognition is doing ("3 found · 240 images a second · 3.1 ms each"), the progress of preparing a new model for the
GPU (once per new model, 35 s on the 4090, saved as `model.engine` next to it; later starts load in 0.4 s), or why it
cannot run (e.g. no model.onnx). **Kinds** (the user asked, 2026-11-02; `show-kinds`, on by default) tags each
box with its kind in the review's colours ("Monster 74%", green; Player blue, NPC amber, You pink; once a model is
trained with them, Dead brick red) when the model
was trained with kinds (the ninth, live since 2026-11-03, is); with a one-category model it is dimmed with a note. How: `train.py --kinds` writes `"kinds"` into
model.json (core/ModelInfo reads it), the Detector takes each candidate's surest kind slot (`Detection::kind`,
`kindSure`) and the next surest (`secondKind`, `second`; for telling when it is unsure which kind, see Only unsure
and the review), and how sure it is of a character is how sure it is of one of the two (`eitherKind`:
1 - (1 - first)(1 - second); 2026-11-03: the surest kind alone lost things it could not tell apart, e.g. an NPC
at monster 40% and NPC 35%; on a recording held out of training whole, 400 instead of 297 found at 50%), and the Tracker keeps each thing's kind as the one it was found as most,
weighted by sureness, over about 300 ms (`kSmoothKindMs`, a first guess to measure on a kinds model), so it does
not flicker between kinds. A one-category model is read as before (guesses byte-identical on the eighth's held-out
images). Since 2026-11-08 the live log records each thing's kinds (`kinds` lines, below), so `kSmoothKindMs` can be measured on the next logs. **Can't tell** (the user asked, 2026-11-08: far-off
characters are a few pixels the user cannot tell either, and "a character, can't tell which" is what the bot's rules
need to go and look closer): a thing the model is under 60% sure of the kind of (`kTellKind`, core/Detection:
`cantTellKind`), or whose kind is the model's own "unknown" once trained with it, is tagged "Character? 90%" in
silver (`theme::kUnknown`; the % is how sure of a character). Measured on two reviewed recordings' live output
(1873 found, kind update and all): at least 60% sure, the kind is right 98-99% of the time at every size; under it are
27 of the 29 wrong kinds on small ones (under 60 px tall), but also 152 of their 347 right ones (most far-off ones).
The live log needs no flag of its own: its `kinds` lines hold how sure of each kind. Hiding a flag never stops
recognition: later flags (and the reactor) rely on it.
**The kind update** (2026-11-08; the user asked whether the review's learning could make the whole model better;
`engine/KindUpdater`): each time a review closes (Close review or picking a screen; not when observex closes), a
background thread re-fits the kind classifier to every reviewed answer of every recording at once and writes
`models/<category>/kinds-update.json`. It starts from model.json's classifier, never from the last update, and takes
300 steps with no pull (`KindLearner::kUpdateSteps`, `kUpdatePull`). Targets: each guess on an image whose kinds are
all checked, matched to a yes answer by overlap (at least 0.5, or the user's tighter box around it since 2026-11-08,
see Box nudge; surest guess first, each answer once; `matchGuesses`) is
that kind, an unmatched one is no; one matched to a kind the model does not know yet (a "can't tell" or "dead" before
a model trained with it) is left out, never taken for no. The model fits the review's kinds when it knows their first ones in
order (`kindsFit`), so a model trained before "can't tell" came keeps its update and the review's learning. The live recogniser then reloads it on its own thread (`Recognizer::reloadKinds`,
`Detector::setKindUpdate`); the finder's Detector loads it too. How sure of a character stays the network's, so what
is found, its boxes and its order never change; only which kind and how sure of it come from the update (`pickKinds`:
the kind is the update's surest, the second its next, each `sure` x its share of the update's kinds). Under Kinds a
dim line then says "Kinds also learned from 11,083 of your answers". It is made only when some answers are newer
than model.onnx and ignored once a newer model.onnx is exported (right after a training it gained nothing and was a
little worse in the review: 26 -> 32 wrong); it fits all answers, since the newest alone forget older places (97% ->
77%). It needs every reviewed recording's guesses and features from the current model; while a review is open the
refresher makes them (Files, below). Python (the tenth, its 343 held-out images): found and false alarms unchanged
(1887 / 136), the right kind 95.0% -> 95.9%, on the new place of 2026-11-07 82% -> 92% (party players taken for
monsters 29 -> about 11). C++ (the tenth on scratch copies): 11,083 answers from 27 recordings, fit 485 ms (Release,
one thread); the head matches a PyTorch fit within 1.3e-5. One more copy back per look: 4.46 against 4.41 ms per
image on the held-out images (`--images`, within noise); sureness and boxes identical, the surest kind changed on 87
of 2212 guesses; images the mirror test finds unsure of a kind 126 -> 94, so Only unsure saves a little less (260 ->
254 of 343).
Measured on the 4090: 3.1 ms per image at 1024 pixels, all 240 images a second (2026-11-03; 2026-11-01: 4.7 ms
and 210 a second at first, 3.3 ms and 235 once the network ran as a CUDA graph, 2.1 ms at 768 pixels, the live size
until 2026-11-03); capture unchanged (239.8 images a second, 0.04 ms latency). The GPU is then nearly fully used, so other GPU work feels it: the finder took 37 ms per image instead of
5 next to it (hence the pause while reviewing), and a game does too: with the user's Lineage 2 running, about 100
images a second at 10 ms each (2026-11-01); a looks-per-second limit is the fix if the game suffers (not built).

Recording (step 1 of the training loop): **Record** (or Ctrl+Alt+R, a system-wide hotkey that works while the game
has focus or observex is minimized) saves the observed screen into a new folder named by date and time, e.g.
`recordings/2026-10-31_23-05-12/000001.jpg`, 1 image every 3 seconds (fixed, `kRecordingInterval` in App.cpp: the
user asked for no rate settings, then for 1 every 3 s instead of skipping near-copy images in the review,
2026-11-01). Only images the screen actually updated are saved. The Record button first asks "What do we record?"
(the user asked, 2026-11-02; `RecordKind` in core/Recordings.h):
- **Every 3 s**.
- **Only unsure**. When an image is due the pipeline asks (`core/Question`, `Pipeline::question`,
  `setRecordOnlyQuestions`); the recogniser looks at its next image once more, mirrored left to right
  (`Detector::detect(..., mirrored)`: the squeeze shader writes the columns backwards, the boxes are turned back),
  after handing the UI what it found (about 5 ms, once per 3 s), and answers yes when `mirrorDisagreement`
  (core/Mirror: each thing at least 30% sure in either view counts how far its sureness is from that of the most
  overlapping thing, IoU 0.3 or more, in the other view; halved) is at least `Recognizer::kMirrorDisagrees` (0.11
  since the live model moved to 1024 pixels, 2026-11-03; 0.17 before, the eighth model's median on its 189 held-out
  images). With a model trained with kinds it also answers yes when
  either look is unsure which kind something is (the user asked, 2026-11-03: the spread between kinds as a trigger):
  a thing at least 30% sure whose two surest kinds are less than `kKindsClose` (0.2, core/Detection: `kindsClose`,
  `closestKinds`) apart, e.g. Player 55% and You 40%. And the mirror test then compares how sure each
  view was of the same kind (`sureOf`: a Monster 90% in one view and a Player 90% in the other adds 0.85). Both make
  more images pass on a kinds model. The ninth at 768 (2026-11-03) kept 0.17 and 0.2: on its 211 held-out images they
  saved 134 (64%), which held 82% of its mistakes at 50% sure and 24 of its 26 wrong kinds. At 1024 it sees the
  mirrored image more alike (median 0.255 → 0.197) and 0.17 saved only 118 (56%), so the cut-off became 0.11: 138
  (65%), holding 81% of its mistakes and 19 of its 20 wrong kinds (the user wants more unsure images, not fewer). `--images` below prints how many each
  would save; a one-category model answers exactly as before (same median and count on the eighth's held-out
  images). Yes saves the next new image; no skips that 3 s step. So about two thirds of the steps save with the
  ninth, about 13 images a minute (half with the eighth). On its held-out images the eighth's kept half held 71% of the
  model's mistakes (a random half 52%, the half with the most things 30 to 65% sure 68%) and 60% of its false
  alarms 65% sure or more (43%): the mirror test also catches mistakes it is sure of (consistency-based active
  learning, CALD). Mixing in the unsure-things count did no better. The C++ test matches Python's (`--images`
  below: correlation 0.97, the same answer on 98%). Each answer is a `mirror` line in the live log, to retune the
  cut-off from real recordings. (The first Only unsure, built earlier on 2026-11-02, saved after looks with 2 or
  more things 30 to 65% sure; replaced the same day.)

Both stop by themselves after 110 images (the user asked, 2026-11-03; `kRecordImages`, `recordingDone`: the main
loop stops by the saved count, marks included, and a label then says it is done), or when the user stops them.
Auto (60 images every 3 s, then 50 only unsure, then it stopped; 2026-11-02) was removed then, as the user asked: on
the user's first new place (2026-11-02_23-46-18) the eighth's mirror test would have kept 38 of the 60 every-3 s
images, and those held all 23 of its false alarms, 31 of its 35 misses and 15 of the 18 with nothing seen; the 22
skipped were quiet (2.3 characters each against 4.7). So per image to review, only unsure finds more of every kind
of mistake; it only takes longer to walk (110 images in about 8 minutes instead of 5.5). Every 3 s stays for an
unbiased look now and then: what only unsure skips is never saved, so only those show whether it is still quiet.

While reviewing, recognition is off, so only unsure saves nothing then. Ctrl+Alt+R starts the kind picked last
(`record-kind` in observex.cfg); with the model failed it records every 3 s. Starting and stopping writes
`recording.observex` into the folder (`core/Recordings`: `key=value` lines `started`, `seconds`, `screen`, `size`,
`every`, `kind` (`every` or `unsure`; `auto`, the removed kind, reads as `unsure`; the first Only unsure ones say
`unsure=1`), `images`, and `reviewed-<category>` once a review changed answers (see Review), written through a
temporary file; the review's list says "only unsure" on those); the review
lists only
folders that have one (the user asked, 2026-11-01:
choosing a folder felt unnatural). The 11 recordings made before got theirs from a one-off script (`started` from
the folder name, the rest from the images and the live log).
While recording, the button reads "Stop", the time, "38 of 110 images" and about how long is left ("about 4 min
left", `recordingSecondsLeft`: exact for every 3 s; for only unsure at its own pace once it has 5 images, before that
at a new place's, two of three due images saved), with a progress bar along the button's bottom
(`BarButton::progress`), so the user knows how much more to walk around. Its tooltip gives the size on disk (the
Recorder sums each file's size; shown in 1024s, as File Explorer does). JPEG quality 85: about 0.7 MB per 2560 x 1440 Lineage 2
image, so about 0.85 GB per hour. **Settings** (gear) holds the recordings folder (default: `recordings` next to
observex.exe; folder picker, Open, Use default) and the live log flag (below). Settings live in `observex.cfg` next to the exe. Both it and
`recordings/` are gitignored.
Recording costs capture nothing measurable (bench: 238.7 and 239.3 images/s recording, 238.2 and 239.2 not).
Live log (the user asked, 2026-11-01, to show Claude what they see live: sureness that is high at times and low at
others, one-image blips): while recording, `engine/LiveLog` writes `character-live.txt` into the recording's folder:
a `look` line for every image the recogniser looked at (screen image number, time, everything found at least 10%
sure with its box; the app draws 30% and up), for a model with kinds a `kinds` line right after each (since
2026-11-08: each thing's two surest kinds and how sure of each, in the look's order; the format is `core/LiveLines`,
shared by the writer and `--track`), `saved` lines tying each saved JPEG to its screen image number,
`mirror` lines (each Only unsure answer: image number and how differently it saw it mirrored; with kinds, also how
close any thing's two surest kinds were), and `mark` lines. **Ctrl+Alt+M** (second system-wide hotkey) marks a moment: a numbered `mark` line, and the image on
screen is saved right then (`Pipeline::recordNow`; the 3 s rhythm counts on from it); the Record button shows the
mark count. The user then says "at mark 3 ..." and Claude reads the log: follow each thing from look to look
(match boxes), plot its sureness over time, find one-look blips, and line them up with the saved images. About 85
MB per hour; lines are formatted outside a lock and buffered (1 MB), marks flushed. The file's header explains the
lines. A Settings checkbox, "Log what the model sees while recording" (`live-log` in observex.cfg, on by
default; the user asked for it), turns the log off from the next recording; with it off Ctrl+Alt+M does nothing
and the Record tooltip only mentions images. Image numbers come from `ImageStamp` (the pipeline's count and the time the screen showed it), which every
`ImageChannel` hand-over now carries.

Review (step 2 of the training loop, `app/Review`): **Review** lists the recordings (the recordings folder's
subfolders with a `recording.observex`, newest first: a picture of the first image (the user asked, 2026-11-02;
`app/Thumbnails`: read 320 pixels wide, one per frame and only for rows in view, about 6 ms each for a 2560 x 1440
JPEG, kept until observex closes; WIC's JPEG shrinking saves little, 9.6 -> 5.8 ms); when, e.g. "Today, 12:56";
length; images; how many are reviewed per category; read once per opening), then "What are we looking for?" (categories in `kCategories`; only Characters for now), then shows the recording's
first image not reviewed yet, filling the view. The finder's guesses are drawn as marks with a tag ("Character? 81%";
from a model with kinds "Monster? 50%", or both kinds when it is unsure which: "Monster 55% or Player 40%?");
the selected mark gets a button per kind under it, then No (keys 1 Monster, 2 Player, 3 NPC, 4 You, 5 Can't tell, 6
Dead, N no; Y is yes of the kind shown; answering moves to the next unanswered mark, or to none once all on the
image are answered, so every mark shows at full strength and no buttons cover it: after nearly every last answer, 131
of 143 on 2026-11-08, the user clicked beside the marks for that), a white
ring around it, and the other marks fade to 35% (`kOtherMarks`; the user could not tell which of several close
characters was selected, 2026-11-01). Since 2026-11-07 (the user found small characters hard to see) the selected
mark's outline and ring are thin (1.5 px) and sit just outside its box, nothing covers the box (no tint), and the
other marks, tags included, are not drawn inside the ring (clipped around it), except the one under the mouse,
which a click would pick. A tag goes above its box, or below it when the box touches the image's top edge (in the
review and live; until 2026-11-08 it went inside the box there and hid a small one completely), and the answer
buttons go below the box and its tag, or above both if there is no room. Near the mouse the other marks fade (the user asked, 2026-11-08: small characters close together were hidden under each other's tags until one was picked): within 24 px of the mouse tags show at 8% and boxes at 30%, fully again from 96 px, smoothly between (`kNearMouse`, `kFarFromMouse`, `kTagNearMouse`, `kBoxNearMouse` in Review.cpp); the mark under the mouse and the selected one stay as they are. Only while the mouse is over the image. The key hints at the bottom drop from the left when they would overlap the status text. Kinds (the user asked, 2026-11-02; `kCharacterKinds` in Review.h, colours in
Theme: monster green, player blue, NPC amber, the user's own pink, can't tell silver, dead brick red): a yes mark's
tag is its kind ("Monster", "You · copied"), and the answers file says the kind (`monster`, `player`, `npc`, `own`,
`unknown`, `dead`; the ids and their order are core/Marks' `kCharacterKindIds`, one list for the review, the bench
and train.py's `KINDS`).
**Can't tell** (2026-11-08, the user asked): a character too far off to tell which kind, instead of a guess or a no
(a no would teach the model it is no character); a kind of its own (`kUnknownKind`), so a model trained
with it says "can't tell" where the user would. **Dead** (2026-11-08, the user asked after the review of
2026-11-06_21-09-47, where corpses were 6 of the 8 things the model was sure of that were no characters, shown live
as "Monster 89%"): a corpse, answered no or monster before that day (the same day 92 such answers in 13 recordings
became dead, found by ranking every guess with that test's classifier and checked by eye; each changed file's old
answers are kept as character-answers.before-dead.txt; corpses the review never asked about are still unmarked, and
corpses behind the chat or hotbar and kneeling or dying poses were left as they were). Not no, because the
model sees a corpse as a character anyway and the bot is to know it is dead (no target; a kill, loot). Tested first on
the eleventh's own features (the numbers its kind classifier reads): 34 corpses from 10 recordings against 5296
living monsters, a recording held out at a time, catch 80% of the held-out corpses while calling 1.8% of living
monsters dead (the box's width / height alone: 19%), so the kind classifier can learn it with no second model.
New kinds go last, so a model trained before keeps its places (`kindsFit`). Images answered before kinds hold
`yes` marks (maybe `yes own`: a suggestion) whose kind is not checked: they count as not reviewed (the list says
"kinds to check on 30"), the review opens at the first of them, kinds checked on the image before carry onto the same
things (`carryKinds`), and Right checks every kind on the image. Since 2026-11-09 a reviewed image is one that is
`fullyAnswered`: kinds checked and no mark without an answer; a guess line written into an answers file later (the
sweep below) makes the image "to check" again, the review picks that guess and waits for its answer, and training,
the kind update and the review's re-fit leave such images out until it is answered (`images_to_answer` in
model.json). **Shift+Right** (or Shift+D) moves on as Right does, then on past reviewed images to the next one to
check (from the first again after the last); the bar shows "103 reviewed · 7 to check", the key hints name it, and
the Review list shows "Characters: 7 to check, 103 of 110 reviewed". Training still treats every kind as one class
(details and why in docs/training-loop.md step 2). A click
picks the mark under it (the smallest there); a drag of at least 6 screen pixels each way draws a missed one (yes;
Remove / Del deletes it) wherever it starts, also inside a bigger mark (the user could not draw a character behind
their own character's mark, 2026-11-01). A drawn box stays exactly as drawn: SAM fitting each drawn box tight was
built and removed on 2026-11-02 (the user does not want it while labelling; mixing fitted and hand-drawn boxes hurt
the model, see docs/training-loop.md step 2). The bar's middle shows back / "Image 5 of 340 · 11
reviewed" / forward (Left / Right keys). Moving forward needs every mark answered (the user asked,
2026-11-01): forward is greyed, its tooltip says how many are left, and Right picks the first unanswered one. Then
the image is "reviewed" and the answers are saved. Answers carry forward: a guess on the next image overlapping an answered mark by at least
half (`carryAnswers`) gets its answer and kind, tagged "copied"; but no answer carries onto a guess the review would start as yes of another kind, nor a "no" onto one it would start as yes, nor a "no" the user drew again tighter (see Box nudge) (the camera moved and a "no" on a lantern landed on a player the model was 100% sure of, saved as no unseen, 2026-11-08; the same evening the user's character stood on a monster, the monster's "Monster" landed on the user's character the model was 94% sure of and was copied on for 13 images until the user went back): that guess is asked instead ("asked, though sure enough to pre-fill: the image before had another answer here" in the log). Across the reviews to then the sure kind was right 8 times of 12 where the two disagreed, the copied one 4, so neither is pre-filled. Then guesses the model is at least 78% sure of the kind of start as yes, of that kind (a monster with a one-category model, by how sure of a character; `presumedKind`, `presumeKinds`, `kPresumeYes`; green, tagged "Monster 92%"; the % on every guess's tag is how sure of the kind shown), except those whose two surest kinds are close (`kindsClose`), which wait for the user: the
user only answers the unsure ones and flips wrong ones (the user asked, 2026-11-01: reviewing took too long; on the
user's first 64 images of a new place, 223 of 230 guesses at 75% were characters, 97%; 75% became 65% when the model
moved to 768 pixels, where it is less sure: the same 196 of 238 held-out characters pre-answered, 3 wrong; 70% when
it moved back to 1024 on 2026-11-03: 854 of the ninth's 1103 held-out characters pre-answered and 34 things that are
not characters, against 825 and 42 at 768 and 65%; since 2026-11-07 by how sure of the kind, the user asked to
pre-answer only when very sure, kind included, then picked 78%: on the tenth's 299 held-out images 1308 of 2015
guesses pre-answered, 56 wrong, 21 of them the wrong kind, against 1594 with 99 wrong, 33 the wrong kind, by 70% of
a character; 85% of the kind: 899 with 30 wrong, 12 the wrong kind).
**Learning while reviewing** (the user asked, 2026-11-08: "better auto-guesses with each picture in a review"; a
party member was taken for a monster on about 40 images in a row). Entering an image not reviewed yet re-fits the
model's kind classifier (`core/KindLearner`) to every guess answered so far in the recording, if the answers changed.
Only the last step from a guess's features to its kinds learns (4 x 256 weights and 4 biases). Each fit starts from the
kind classifier in use (since 2026-11-08 the kind update when there is a valid one, else model.json's;
`kindHeadToUse`) and is pulled back towards it (60 steps of Adam, pull 0.003, as in the Python simulation); the model
side of the pre-answer rule below is that classifier too. Starting from an update that knew 2026-11-07_12-57-55, a
Python replay of the review of 2026-11-07_13-09-21 pre-filled right / wrong / asked 453 / 10 / 41 against 438 / 12 /
54 from the model's own; in C++ (`--replay`, the tenth, an update from the other 26 reviewed recordings, 10,580
answers) 451 / 13 / 39 against 438 / 13 / 52. Each guess on a reviewed image learns the kind of the yes answer it
overlaps at least half or the user's tighter box around it (surest guess first, each answer once), else "none of
the kinds", as the kind update learns
(`addAnsweredGuesses`); drawn boxes and images whose kinds are not all checked teach nothing. By overlap since
2026-11-08: before, an answer belonged to a guess only with the same box, so once a new model made the guesses again
a partly reviewed recording's answers barely counted (40 answered images of 2026-11-07_13-09-21 with the eleventh:
learned from 2 answers, now 215). Each guess's tag (kind, %, "or" when close) then comes from the re-fit. Carried answers still come
first. Then a guess starts as yes of a kind when the model and the re-fit both pick it at 78% (`kPresumeYes`), or the
re-fit alone is at least 90% sure (`kPresumeLearned`; 85% until the user picked 90% on 2026-11-08, fewer wrong pre-fills for more questions, numbers in core/Marks.h); else it waits for the user (`presumedKind`). Picked in a
Python simulation of whole reviews in order, answers carried forward, the user's answers as the user (2026-11-08):
the tenth on the two 2026-11-07 recordings it never trained on, guesses pre-filled right / wrong / asked 880 / 38 /
206 against 807 / 38 / 279 for the model alone; the ninth on 4 places it never saw 1440 / 107 / 1821 against 1049 /
950 / 1369. The first setting (pull 0.01, alone 95%) was safer but learned little: 825 / 30 / 269 and 872 / 62 /
2434. Replayed in C++ (`--replay`) on 2026-11-07_12-57-55, answers carried forward: 438 / 28 / 153 against 386 / 23 /
210 (Python on the same images 442 / 26 / 152 against 385 / 23 / 212); fits 3.5 ms on average there. The status
bar says "learned from 512 answers" (hidden after an undo or a changed answer, until the next new image re-fits). A
fit takes about 9 ms at 1,000 answers and 17 ms at 2,000 (Release). It needs the guesses' features
(`<category>-features.bin`, below) and a model.json with the kind classifier; without them the review works as before.
There is no Skip: the user chose recording 1 image every 3 s over skipping near copies in the review. No setting
for a review step (the user removed one before).
**Box nudge** (the user asked, 2026-11-08, after their first review boxing far-off characters tight,
2026-11-06_20-41-55: "fixing every single box is absolute madness so i need better autoguesses"; `core/BoxLearner`).
Until then the user kept the finder's boxes (in every recording the guesses' boxes matched the user's yes boxes at
x1.00 width and height); in that review they said no to 305 guesses and drew each again tight around the body, small
ones (120 px tall or less) at about 0.73 of the guess's width and height, big ones unchanged, and kept 331. Entering an
image not reviewed yet now nudges each guess's box the way the user boxed the guesses so far: a straight line from the
guess's features (the 256 numbers the kind classifier reads) to how its box moves (its middle, in its width and
height) and resizes (width and height, as logs), fitted by ridge regression (pull 10 on the weights, a hundredth of
it on the bias) to every guess on the reviewed images that matches a yes answer: kept ones teach "as boxed", redrawn
ones the user's box. Images near the one shown count most (a sample's weight halves every 10 images away). A nudge
moves the middle at most half the box and sizes 0.37 to 1.65 times, inside the image. The nudged boxes are what the
review shows, carries answers onto and saves when the user moves on; the guesses file keeps the finder's. Simulated
in Python, then replayed in C++ (`--replay`) with the same numbers: of the 364 guesses the user drew again, within
0.7 overlap of the user's box 1% -> 63% (within 0.8: 0% -> 35%; overlap on average 0.42 -> 0.73); of the 326 kept,
93% stay within 0.7 (88% within 0.8); on a review that kept the boxes (2026-11-06_20-51-48) nothing moves out of 0.7.
Without the half life 52%, from the box's size alone 26%, nearest neighbours in the features no better. A fit takes
about 3 ms (690 guesses, Release); with no answered guesses yet it does not nudge. The log's `image` line adds "box
nudge learned from N answered guesses in 3.0 ms; the user's boxes near this image on average: 0.79 of the guess's
width, ...", each nudged mark "nudged from [the finder's box]"; the status bar "boxes nudged your way". Two rules
learned the tighter box the same day: matching a guess to an answer (`matchAnswers`, for the kind re-fit, the kind
update and the box nudge) also takes a yes answer 90%+ inside the guess overlapping it at least 0.15, unless 2+ yes
answers lie 60%+ inside it (a group the user split); before, 297 of the 385 redrawn guesses counted as "not a
character". And a "no" the user drew again tighter (`boxedAgain`) is not carried to the next image, where it landed
on the same character: replayed on that review with answers carried, pre-filled right / wrong / asked 543 / 18 / 251
against 450 / 60 / 302. Training: the tight boxes are a new box style. The seventh model, trained with about half its
boxes tightened, was worse everywhere; mixing the user's tight boxes with the ~15k older loose ones may do the same as
the tight share grows. The user decided (2026-11-08): everything tight from now on ("this should help with
differentiating single characters from crowds"). **Re-tightened 2026-11-09** (the user asked: "go through ALL previous
reviews ... and re-tight it"): 13,274 yes boxes in 32 recordings (all but 2026-11-06_20-41-55, the user's own tight
review, left as they answered it), each file first copied to `character-answers.before-retight.txt` (to undo: copy
it back). How: the guess matching the answer (overlap 0.5+) moves and resizes by a ridge regression (pull 20) from
its 256 features to the user's tight box, fitted to the user's 354 redraws plus the big guesses they kept; checked by
5-fold by image against those redraws, overlap with the user's box 0.79 on average, 81% within 0.7 (SAM 2.1 large
on a zoomed crop 0.66 / 53%: it kept loose boxes on tiny characters; D-FINE large on crops 0.70: it boxed parts). The
nudge fades with the box's height: full up to 120 px tall, none from 180 (the user redrew 75% of guesses under 90 px,
36% at 120-150, 7% at 150-180, none above). An answer no guess matches (~700) shrinks by its size alone, but not when
wide (animals, corpses) or the own character (by size alone those were cut). Small boxes became about 0.64 of their
width and 0.68 of their height. Looked over by eye on sheets of 36 per recording in 6 recordings. Held-out scores of
the twelfth model and earlier were on the padded boxes: against the tightened ones they look worse than they are
(a box 0.73 of the truth's width and height overlaps it 0.53, near the 0.5 that counts as found).
**Night of 2026-11-09 experiments** (scratch only, nothing live changed; the user asked to try anything that helps
crowds within 16 ms a frame). All scored on the same held-out images (497, 3,488 tight boxes) at equal false alarms,
counting a guess that fits the character in either box style. Training on the re-tightened labels (same settings as
the twelfth) is the one big win: at 200 false alarms 3,103 found against the twelfth's 3,017, and boxes around 2+
characters 27 against 64. Everything else was within about 1%: the same weights run at 1280 or 1536, a model trained
at 1280, RF-DETR's blurry square crops turned off (`scale_jitter=False`), sharp 2x crowd crops added to training, a
zoomed second look (one 1280x720 crop around the most small guesses, its boxes taking the full look's kind, duplicates
dropped; +1% only at 300+ false alarms) and a mirrored second look (+1% at 150-200). The same training rerun with
another seed differs by about 10 found at equal false alarms, so those 1% gains are borderline. Misses are mostly monsters under
40 px tall (123 of 514 missed), crowded ones (32% missed against 13% alone), and nearly invisible ones (fog, behind
UI windows, see-through ghosts). Missing labels are real: on its own training images the tight model is 50%+ sure of
123 things touching no yes box, about two thirds of a sample clearly characters; 73 never had a box, 30 sat inside a
2x+ bigger box answered no (a padded or group box rejected, the right box never drawn). Scripts and numbers: the
session scratchpad's `tonight/` (findings.md).
**Ctrl+Z** (the user asked, 2026-11-07; `core/ReviewHistory`) undoes the last step, one per press (held down it
repeats): an answer (keys 1-6, Y, N or a button), a drawn or removed box, or a move to another image (Left / Right
or the bar's arrows; a Right that made the image reviewed and checked its kinds is one step). Each step keeps what
the image held before (its answers, or its draft if not reviewed yet), so undoing puts that back exactly, saves the
answers file again if it changed, shows that image and selects the mark it changed. Undoing past a move goes back to
the image before, the next undo its last edit. What entering an image does (answers carried forward, sure guesses
pre-answered, the kinds learned) is worked out again from what it holds, so it needs no step; an undo that changes
the answers makes the next new image re-fit. With Ctrl down the other keys do nothing
(Ctrl+Y, redo elsewhere, would answer yes). One history per opening, gone when the review closes; no redo.
**Zoom** (the user asked, 2026-11-08: a crowd of far-off characters was too small to answer; `core/ImageView`): the
mouse wheel over the image zooms in from 100% (the whole image, the default and the least) to 800% in image-editor
steps (125, 150, 200, 250, 300, 400, 500, 600, 800%), keeping the pixel under the mouse where it is; a right or
middle drag moves around; 0 shows the whole image again. A label at the image's top right says "Zoom 250%" (with
"Right drag: move  0: whole image") while zoomed in, and "Zoom 100%" for a second after coming back. The zoom stays
from image to image until the review closes (a crowd stays a few images). Zoomed in, the review picks the unanswered
marks in view first (after an answer, Tab, on a new image), so answering a crowd up close does not jump away; when it
picks one out of view (none left in view, Next with marks unanswered, Ctrl+Z) the view moves to centre it, and a key
answer with nothing selected only shows the mark first: nothing is answered unseen. A click never moves the view.
Marks all out of view are not drawn (no stray tags at the edge), and the answer buttons hide while the selected mark
is out of view. The review log's answer and draw lines add ", zoomed in 400%" when zoomed in. Checked in snapshot
mode (`--scroll`, `--right-drag`).
**Last reviewed** (the user asked, 2026-11-07): when a review closes (Close review, picking a screen, or closing
observex while reviewing) and the answers file differs from when it opened (any answer, kind or box on a reviewed
image, or an image newly reviewed; undone back to how it was is no change, and answers on an image never moved on from
are not saved, so they are none either), it writes `reviewed-<category>=2026-11-07 10:48:12` into
`recording.observex` (`writeReviewed`: only that line changes, every other stays as it is; stopping a recording keeps
it). The list then shows a third line in the row, "Last reviewed today, 10:48" (the latest of its categories), "...
yesterday, 22:05" or "... 6 Nov, 22:05"; rows never changed in a review show none. The list's dates say "Yesterday"
since then too.
**Review log** (the user asked, 2026-11-08: so they can later ask Claude how they reviewed a set, what they mostly
fixed and why, and get proposals for the model, the heuristics or their own labelling): every review appends to
`<category>-review-log.txt` in the recording's folder (`app/ReviewLog` writes, `core/ReviewLines` words it; the file's
header explains every line). One event per line with date and time, details indented below: `open` (model, which
classifier, learning on or off), `image` (how it started mark by mark: box, finder's sureness, the model's and the
re-fit's sureness per kind, what it was pre-filled as and by which rule; the re-fit's answers per kind, its time and
how far it moved the classifier), `select` (click, Tab, Esc), `answer` (from what to what, key or button, seconds
since the image appeared), `draw`, `remove`, each with the marks it overlaps (overlap and how much of each box lies
inside the other: one box over two characters, a box drawn over a guess), `next blocked`, `done` (what the image holds
and every change from how it was shown: changed, confirmed, drew, removed, kept), `back`, `undo` (what it did),
`guesses ready`, `close`. Boxes are in image pixels and the image file name is on every line, so the image can be
opened at that spot. There is no box resizing in the review: a wrong box is fixed by drawing one and answering the
guess no, which the log shows as a draw overlapping the guess and an answer no on it. Checked in snapshot mode
(2026-11-08): a 13 s session over two images wrote 51 lines. It only records; nothing reads it back.
Files, in the recording's folder, in the text format of `core/Marks.h`: `<category>-guesses.txt` (from the finder,
read only; from a model with kinds each guess, after how sure it is of a character, names its surest kind and the
next, each with how sure of it: `0.820 monster 0.700 player 0.400 1012 540 80 120`) and `<category>-answers.txt` (written by review; only reviewed images, rewritten through a temporary file
on every change). Also `<category>-features.bin` (2026-11-08, from the finder, binary, `core/Features`): for each
guess, in the guesses' order, the 256 numbers the model's kind classifier reads, for learning while reviewing and the
kind update. The review and the kind update ignore it when it is older than the guesses or the model, or does not
match the guesses (`freshFeatures`). Next to the model, `models/<category>/kinds-update.json` (2026-11-08, the kind
update above: answers, recordings and the re-fitted classifier at full precision, written through a temporary file;
`core/ModelInfo` reads it with model.json's parser and uses it only when it is at least as new as model.onnx and
shaped like model.json's classifier). While a review is open and its own finder is done, the refresher
(`engine/Refresher`) runs a finder on each other reviewed recording whose guesses or features are not from the
current model, one at a time, so the kind update can use them (after a new model that is every one: 27 recordings,
1,826 images in 46 s); closing the review stops it, and the next review goes on. The finder
(`engine/Finder`): when a review opens a recording with images not reviewed yet, and
`models/<category>/model.onnx` exists, and the guesses are missing or older than it, it runs the model over the
recording inside observex, on its own thread (below-normal priority) and GPU device, with the same `Detector` as
live recognition: read each JPEG, upload it, detect, then write the guesses and their features, each through a
temporary file (guesses first). A re-exported model makes it look again at each recording when its review opens. No Python
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
weights; run it again by hand to re-export a model). Since 2026-11-08 (for learning while reviewing) the ONNX has a
third output, `features`: each candidate's 256 numbers that its kind classifier reads. Only the finder copies them
back; live recognition does not (3.2 ms per image before and after). export.py also writes that classifier into
model.json (`kind_head`: its biases and weights, rounded to half precision as in the ONNX). `guess.py RECORDING` writes `<category>-guesses.txt` with the
Python model (half precision); observex no longer runs it (its own finder does the same, see above), but it stays as
the Python reference to check observex's results against.
Its own Python in `tools/train/.venv` (gitignored; setup in `tools/train/README.md`). Training runs on the GPU for
about 35 minutes on 845 images (2026-11-03): run it in the background, never in the foreground, and never two trainings at once
(two filled the 24 GB, Windows spilled GPU memory into normal RAM, both slowed to half and one hung). Keep the batch
size fixed (see the README).
Answers follow the box rule in docs/training-loop.md, step 2 (body only, every visible character, none for hidden
ones; monsters, other players, NPCs and the user's own character are all characters). It trains at 1024 pixels and observex runs it at 1024 too (`LIVE_RESOLUTION` in train.py;
export.py, guess.py and the score use it). From 2026-11-01 it ran at 768, a third faster for nearly the same answers
then; with the ninth, 1024 scores clearly better (AP50 0.902 against 0.873) for 3.1 ms per image live instead of 2.0,
and the user chose it (2026-11-03). Training at 768 was worse (2026-11-01).
`--kinds` (2026-11-02; the live model since 2026-11-03) learns monster, player, npc and own as four classes, and
since 2026-11-08 unknown (can't tell) and dead as a fifth and sixth (until the user answers some, classes with
few or no examples)
(`KINDS` in train.py, the review's order) from the images whose kinds are all checked (the others are left out and
counted), trains on images with a rare kind more often (`REPEAT_RARE`: players about 2.6 times, NPCs 1.6), and
since 2026-11-08 on crowds twice (`CROWD_REPEAT`: two characters at most 120 px tall that nearly touch; 637 of 1813
training images; the model drew one box around several far-off characters, "very dangerous for realtime" the user
said), writes the kinds into model.json for the live Kinds flag, and adds to the score how often a found character
got the right kind, and since 2026-11-08 how many false alarms are group boxes (one box around 2+ marked characters;
the twelfth on the 453 held-out images of that evening: 45 at 30%, 38 at 40%, 32 at 50% sure).
Unsplit groups (2026-11-08, the user asked): a zoomed second look (the live model on a window 5x each answered box's
height, of the screen's shape) at all 11,124 answered characters up to 150 px tall found 95 answered boxes holding
2+ characters; by eye 31 are real (two gremlins, two orcs, players one behind another), the other 64 the zoomed model
cutting one character (a spider's left and right legs, a monster and its weapon, a glow). 30 were split (24 boxes
answered no as the user marks a group box, 55 added; one unclear left; backups
`character-answers.before-splits.txt` in 10 recordings): the answers were already clean, about 3 group boxes per
1000. The same pass found 443 characters at zoom that no answer (yes or no) covers, on reviewed images: things no
guess offered at review time and nobody drew. By eye (the user asked) 224 were added (115 monster: fog zombies in
2026-11-01_08-48-08, orcs, far werewolves; 48 player, mostly the village crowds of 2026-11-06_21-59-42; 46 dead,
corpses lying; 4 npc under their name tags; 11 unknown, a figure whose kind can't be told; backups
`character-answers.before-missed.txt` in 24 recordings); the other 219 were junk (inside the minimap, behind UI
panels, faint shapes in fog or under the darkened top of 2026-11-07_12-37-30, specks under ~18 px). **Sweep in the
review** (2026-11-09, the user chose to answer them in the review): the fourteenth on all 2,486 answered images found
192 things at least 50% sure that no yes answer covers (touching none, or beside one but not 30%+ overlapping it or
60% inside either way; not about the same box as a no; one per thing): 82 monster, 68 player, 28 dead, 7 npc, 4
unknown, 3 own, in 171 images of 27 recordings. Each was written as a guess (with both kinds) after its image's
answers, the file first copied to `character-answers.before-sweep.txt`; the user answers them in the review (opens at
the first, Shift+Right to the next). A sample by eye: most of those touching no answer looked like characters, the 23
beside an answer mostly a second box on an answered character, a spell effect or a shadow. Answered the same
morning (10:41-11:47, about 20 s a spot): 125 characters (38 monster, 29 player, 27 dead, 26 can't tell, 3 npc, 2
own), 67 no; on those 161 images the user also redrew 63 of the 1,385 re-tightened boxes (1 in 9 under 30 px tall,
none above 90; their box about 1.25x the re-tightened one's width and height: the re-tightening cut small
characters too much) and changed ~15 kinds. The same morning they reviewed three new recordings
(2026-11-06_19-52-12, 19-57-43, 20-08-02; 330 images). **Checks in the review** (2026-11-09 midday, the user asked
to improve the training data; scripts in the session scratchpad's `clean/`): a zoomed look (the live model on a
crop of the screen's shape 6x the box's height, at least 270 px tall) boxed the user's 63 redraws far better than
the re-tightening (overlap with the user's box 0.70 against 0.41, closer on 51 of 60); where it overlaps a
re-tightened box under 90 px tall less than 0.4, about half to two thirds were wrong on the images the user looked
over closely. Written into the answers files (each first copied to `character-answers.before-checks.txt`) as guess
lines the review asks again: 512 box checks as two guesses of the answer's kind on the same character, the user's box
and the zoomed one (Y on the better, N on the other; the order mixed), 58 where the zoomed look found nothing there
or something else (the user's box alone, Y or N), skipping 31 whose zoomed box matched one the user had answered no;
30 kind checks (the model 90%+ sure of another kind; 19 of them the user's own character "rebornlover" in
2026-11-06_19-57-43 answered monster) and 9 no checks (the model 80%+ sure of a character answered no, nothing else
covering it; 17 more were the mouse pointer, which the model takes for a monster at 82-88%, left out), each one
guess with the model's kinds. 445 images, 1,121 boxes to answer. A check line names a second kind at 0 so the tag
reads "Player? 100%". Until answered those images are left out of training like any to check; to train before, put
the unanswered checks' old lines back from the backups. The user answered 339 box checks, 5 kind and 6 no checks
(two-box checks: the zoomed box 215 times, their own 26, no to both 48, of which they drew a box of their own 36 times,
about halfway between the two: overlap 0.77 with the two boxes' average), then asked Claude to answer the rest
(2026-11-09 afternoon). Claude's eye, tested blind on 24 of the user's two-box answers, agreed 12 times; on 16 one-box
answers 8 (a coin flip on small blurry ones). "Always the zoomed box" matched the user's final box within 0.7
overlap 216 of 277 times. So the 222 open two-box checks took the zoomed box, except 17 where the zoomed box lay on a
character already boxed (crowds, mostly 2026-11-06_21-59-42): those kept the user's box. The 9 open one-box checks
were left for the user. Kinds by context: the 19 "rebornlover" ones own (it is the user's character that day: its
name in the status window, middle of the screen), also 2026-11-07_12-57-55 000089 (own sits there in 101 of 110
images); players with a party name (hatemondays, rebornenjoyer) player; a werewolf box answered player monster; two
lying corpses answered no dead; a running figure answered no player; two kneeling ones the user called dead kept.
Six old duplicates (one character, two yes boxes of the same kind) turned no. Each file Claude changed was first
copied to `character-answers.before-claude.txt`; the list is in the session scratchpad's `clean/claude_answers.json`.
**Current model: fifteenth (after the checks, trained 2026-11-09 19:41-21:30, 107 min, in the session scratchpad
`clean/out_15`; live since 2026-11-10 00:18, the user's choice; engine built by observex_bench on a copy, loads in
0.4 s; 4.6 ms per image like the fourteenth; no kind update yet):** the
fourteenth's recipe on the cleaned labels, 38 recordings, 2421 images to train on (9 left out: the open one-box
checks), 606 held out (3685 marked). Both models on those 606 (fit rule, found at equal false alarms 150 / 200 / 250 /
300 / 400): fourteenth 3287 / 3346 / 3366 / 3387 / 3406, fifteenth 3337 / 3377 / 3403 / 3428 / 3440 (+31 to +50; seed
noise ~10); group boxes at 50% 27 -> 20; the right kind 95.8% -> 95.3%. Split: on the 33 recordings the fourteenth
trained on, +13 / +14 at 0.25-0.35 false alarms per image, +32 at 0.5; on the 5 it never saw (2026-11-06_19-46-39 to
20-18-23) false alarms at 50% 37 -> 7. So cleaner labels gave a little, new scenes most. **Error check** (Claude's eye,
`clean/errsheet.py`, sheets and `verdicts.json` in `clean/err`): of the fifteenth's 646 held-out mistakes at 50% (115
lone false alarms, 364 misses, 167 wrong kinds), samples of 32 / 32 / 16 sorted: label wrong 6 / 2 / 0, model wrong
15 / 21 / 11, can't tell 11 / 9 / 5. Weighted: label errors 7% for sure, up to 37% if every can't-tell were one (those are
tiny, blurry, see-through or crowded). Label errors seen: corpses unmarked in recordings from before the dead kind,
small characters unmarked, a box half or twice the character, two boxes on one. Model errors: name tags, UI windows,
spell light, the Discord overlay, half a big monster; misses of small or crowded ones and some plain Gremlins; Gremlins
called npc (monster -> npc is the top kind mix-up, 30).
The thirteenth
was already training without them. The thirteenth (crowds twice, the 30 splits; 97 min) scored the same as the
twelfth on the same 453 held-out images: at 50% 2725 found, 180 false alarms, 33 group boxes against 2724 / 167 /
32 (at 30%: 2832 / 417 / 42 against 2833 / 434 / 45); the right kind 96% both. Crowd repeats did not cut group
boxes; the twelfth stayed live, the thirteenth was kept in that session's scratchpad only. The score and guess.py read a kinds model as observex does (`keep_kind_scores`, `kinds_guesses`: each thing
once, how sure by `eitherKind`; RF-DETR's own predict lists a thing once per kind). The score still counts characters of any kind, so a kinds model
compares with the one-class ones on found and false alarms.
The fourteenth (trained 2026-11-09 00:10-01:44 into the session scratchpad, live from 09:49 that morning to 00:18 on
2026-11-10; kept in `models/character/previous-2026-11-09-0144/` with its kind update and engine): the twelfth's recipe and kinds on the re-tightened labels (32 recordings, 1989 images to train on,
3001 a pass with rare kinds and crowds repeated, 93 min). On its 497 held-out images (3,488 tight characters) against
the twelfth, counting a guess that fits the character in either box style, at 200 false alarms 3,103 found against
3,017 and boxes around 2+ characters 27 against 64; at 50% sure 3,078 found / 174 false alarms against 3,017 / 188;
the right kind 96% (more in "Night of 2026-11-09 experiments" above). As fast (`--images` 4.6 ms per image both). Its
boxes are tight, so the review's pre-filled boxes are tight too. No kind update yet (the twelfth's was learned on its
features); its model.engine was built by observex_bench on a copy and copied in (the engine depends only on
model.onnx, and observex builds a new one if a saved one does not load), so the first start does not wait. Earlier
models' engines were also built by bench speed checks hours before they went live. The twelfth is kept in
`models/character/previous-2026-11-08-1730/` with its kind update and engine.
The twelfth (trained 2026-11-08 16:20-17:31, live from 20:25 that evening to 09:49 on 2026-11-09; the
eleventh's recipe with all six kinds, Can't tell and Dead included, on 30 recordings with the reviews of 2026-11-08
and the relabelled corpses; 1725 images to train on, 2144 a pass with rare kinds repeated, 70 min): on its 431
held-out images (2959 characters; neither model trained on any; corpses and can't-tells left out, and guesses on
them ignored) against the eleventh AP50 0.928 -> 0.937, at 50% sure 2586 found / 151 false alarms -> 2604 / 164 (13
of them called dead, mostly corpses nobody marked), at 65% 2430 / 90 -> 2458 / 89, the right kind 95.1% -> 96.4%;
but more eager under 50%: at 30% 307 -> 422 false alarms (mostly a second box on a character already found, and a
few junk spots that repeat). Of the 20 held-out corpses at 50% sure it calls 12 dead, 5 monster (collapsed
skeletons, a kneeling werewolf, one behind the UI), misses 3; it called 3 living monsters dead (2 walking keltirs,
long and low like a corpse). Can't tell has 4 answers in all, too few to learn yet. As fast (`--images` 4.4 against
4.6 ms per image). The eleventh is kept in `models/character/previous-2026-11-08-0202/` with its kind update
(kinds-update.json, learned on its features, so not valid for the twelfth).
The eleventh (trained the night of 2026-11-08 at 02:02, live from 09:50 to 20:25 that day;
the tenth's recipe on 26 recordings, with the two of 2026-11-07; 1373 images to train on, 54 min): on its 343
held-out images (2183 characters; no model trained on any) against the tenth AP50 0.906 -> 0.908, at 50% sure 1887
found / 136 false alarms -> 1893 / 138, the right kind 95.0% -> 94.8%: within noise overall. Where the new images
are, the kind is right on the 2026-11-07 recordings' held-out images 82% -> 95% (players taken for monsters 29 -> 5:
the user's party); a little worse on the 11-03 and 11-06 places (94 -> 92%). The tenth (2026-11-07 12:04, 24
recordings, 1197 images; against the ninth on 299 held-out images AP50 0.884 -> 0.899, the right kind 75% -> 96%)
is kept in `models/character/previous-2026-11-07-1204/` (its export from before the features output in
`before-features/`); the ninth in `previous-2026-11-03-0630/`.
Tried the same night: the eleventh's data at `--model large`: AP50 0.907, 1883 / 118, 94.6%; as fast live (4.5
against 4.7 ms per image, `--images`) but 10 fewer found for 20 fewer false alarms, so not kept. The tenth's held-out
curve is flat after pass ~19 of 40, so more passes do not help; more reviewed places do.
The ninth (2026-11-03 06:30, the first with kinds; 20 recordings, 845 images to train on, rare kinds
repeated), run at 1024 (since the same morning): on 211 held-out images (1103 characters) at 50% sure it finds 939
with 61 false alarms, the right kind on 98% of those found; AP50 0.902 (at 768: 894 and 67, AP50 0.873). The eighth
on the same images (it trained on none), at 768: 886 and 49, AP50 0.874; at 1024 AP50 0.881. Some of the ninth's
false alarms are far-off characters missing from the answers, some are not. On a recording kept out of training
whole (mostly NPCs), kinds find a little less than one category (AP50 0.912 against 0.923 at 768, 0.933 against
0.936 at 1024) and the kind is right only about half the time there (a new place's NPCs taken for monsters): Only
unsure's kind gap records those for the review. Details, and everything tried that night, in docs/training-loop.md,
step 4.
The eighth (2026-11-02 23:13, the first with the learning rate falling to nothing along a cosine, `LR_SCHEDULE`) is
kept in `models/character/previous-2026-11-02-2313/`. The sixth (2026-11-02 00:16) was the first with the user's
own character as a character (0 → 122 of 141 found). A seventh model trained on SAM-tightened boxes was worse
everywhere (AP50 0.712), so the answers keep the boxes as drawn (details in docs/training-loop.md, step 4). Strict
overlap scores (AP75, AP50:95) favour whichever model pre-answered the review, since a pre-answered yes keeps that
model's box: on the 18 orc images the sixth matched its own boxes at 98% overlap; compare models by AP50 and found /
false alarms at 50%.

Tracker (`core/Tracker`, built 2026-11-01; the user saw sureness swing and one-image blips, see the live log):
follows what the model finds from look to look, so each character keeps one steady box and an identity (id, speed).
A new thing starts being followed only from a box at least 60% sure (`kStartSure`; 50% until 2026-11-08) and counts once found on 3
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
merging parts into the whole (worse; and on 2026-11-02, when a Talon Spider showed as two halves, a side-by-side
merge rule would have joined 98 pairs of real characters on 8% of the answered images: split characters are for
training to fix, with the box rule's whole box yes and halves no), and dropping boxes lying inside a bigger one
(small gain, hides characters standing in front of others). What time can still fix (measured 2026-11-09 on 25
live logs, 13,535 answered characters, ninth and tenth models): of the characters the look at a saved image missed
(50% sure), a look within 150 ms found most (tenth: 86.0% found, +5.4% within 150 ms, +2.2% within 500 ms, 1.2% only
30-50% sure, 5.2% never seen within half a second; ninth: 73.5 / 17.3 / 2.4 / 0 / 6.9%), which the tracker's hold and
30% keep already cover; of 795 sure false alarms only 79 were blips, the rest steady (330) or around 2+ characters
(386), the same in every look, so no rule over time removes them. Boxes barely wobble (sure boxes: 9 in 10 move
under 2.5% of their height between looks). The 60% start rule (2026-11-08, the user asked for steadier live boxes; replay of the tenth model's three live logs of 2026-11-07 with the Python tracker port): boxes appearing or vanishing 2.14 -> 1.36 a second, at the 220 answered images 993 -> 983 of 1051 characters shown and 107 -> 89 boxes on nothing marked; six older logs 6.47 -> 5.31 a second, 3736 -> 3691 of 4459, 872 -> 774. Confirming after 2 looks instead of 3 showed more but was jumpier on the older logs; holding longer (300-500 ms) was steadier but kept more boxes on nothing.
Lower cutoffs, checked and not kept (2026-11-02, the user asked: the nine live logs replayed through a copy of the
tracker, scored against the user's answers at each of the 455 saved images; the user's own character left out,
which those models did not count). On the fifth model's two logs (641 characters): now 518 found, 95 false boxes,
5.5 changes a second; starting from 40% 526, 114, 6.9; keeping from 20% (`Recognizer::kMinSure`) 530, 122, 5.3.
The added boxes are mostly things that never move, which following cannot filter: the minimap and map window,
stones, pillars, lamp posts, name tags, chat text. Misses are for training to fix, not the cutoffs. Worth checking
again once there are live logs of a newer model.

Not built yet: react (mouse and keyboard), and the knowledge file (what to look for and how to react,
saved and loaded).

**Training stopped (2026-11-10, the user's decision after many evenings of small gains):** no more training rounds,
label cleaning or model tricks; the fifteenth stays. Next is heuristics on top of the model, tuned on clips: a
ring buffer saved on request, a bench replay of a clip with per-look numbers, and our own player (frame by frame
and smooth, later like Unreal's Rewind Debugger: per look what the model saw, what the tracker and rules decided).
**Clips: built 2026-11-11** (the user asked, 2026-11-10: the last 10 s, exactly what they see, nothing drawn on it,
every guess and anything interesting as text, so the same seconds can be compared after a heuristics change; Review
asks "recordings or clips?", clips listed with miniatures like recordings; reviewing a clip only plays it for now,
the user will say what comes next). The clip buffer (`engine/ClipRecorder`, see Architecture) always runs while a
screen is observed. A clip is a folder in `<recordings folder>/clips/`, named by when it was saved: `clip.mp4`
(H.264, a frame each time the screen changed, at the time the screen showed it), `clip.txt` (`core/ClipLines` and
`core/LiveLines`, header explains: `frame FRAME IMAGE TIME` ties each video frame to the screen image number the
looks use; `look`/`kinds` as in the live log, down to 10% sure; `shown IMAGE DONE ...` what the app showed after
each look: the tracker's things with id, sureness, kinds, box, speed, held; `note TIME TEXT`), `picture.jpg` (the
screen at the save, the list's miniature) and `clip.observex` (`ClipInfo`: saved, seconds, frames, looks, screen,
size, model). `observex_bench --track <clip>/clip.txt` replays its looks through the tracker as for a live log.
In the app: **Save last 10 s** (top bar, left of Record, or the system-wide Ctrl+Alt+C, shown in Settings with the
other hotkeys) saves what the buffer holds, then says "Saved 10.3 s" for a moment; it is greyed out (with why, on
hover) until the buffer holds frames or while a save is written. **Review** first asks "What do we review?":
recordings (as before) or clips; the clips list ("Play a clip") shows each clip's `picture.jpg` with when, seconds,
frames, looks and screen, newest first. A clip opens on the clip screen (`app/ClipView`, the frame as large as fits,
framed like the observed screen) instead of the observed screen; the top bar then has play/pause (or Space), the
time ("0:05.9 / 0:10.1"), the frame number (the `frame` lines' numbers) and Close clip. It plays the frames at their
own times (`engine/ClipPlayer`: GPU decode through Media Foundation's source reader, with its frame-rate conversion
turned off, `MF_XVP_DISABLE_FRC`, or it repeats and skips frames of a video whose frame rate varies; the CPU decoder
if the GPU's fails; paced by a waitable timer). Opening one takes about 0.1 s on the UI thread. The app's own notes go
into `clip.txt` as `note` lines: clip saved, save last 10 s (button or hotkey), recording started/stopped, mark,
review and clip opened/closed, kind update made, show Characters/Kinds on/off. The clip buffer pauses while a review
is open (the finder gets the GPU) and repeats the last image meanwhile. Checked 2026-11-11: every decoded frame's time
matches its `frame` line within 0.06 ms; colours within about 0.7 of 255 per channel of `picture.jpg`.
Targets, from the 2026-11-09 measurement above: steady false alarms (UI panels, minimap, pillars, name tags) and
boxes around 2+ characters, which no rule over time removes, and kinds that change from look to look.

**Next: learn while observing** (agreed with the user, 2026-11-01, one step at a time; step 6 of the training
loop): questions saved while observing when the model is unsure, then a background trainer that refits the running
TensorRT engine so answers change the boxes live.

**The training loop.** The first target game is Lineage 2, and the first thing to recognise is characters (any kind:
monsters, other players, NPCs and the user's own; telling them apart comes second; the category was "monsters"
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
  shader squeezes the image to the model's size (1024 x 1024) into a buffer shared with CUDA, TensorRT runs the network,
  and only the candidate boxes come back. The network and the copies back run as one CUDA graph, recorded on the
  first look (one launch instead of hundreds: 5.56 -> 4.68 ms per image on recording files, same guesses; the
  Finder gets it too). TensorRT's highest builder optimisation level (5) was tried: slower (4.86 ms) and 141 s to
  prepare, so the default stays. Those at least 30% sure go through the `Tracker` (timed by when the
  screen showed the image). Hands `Found` (what it follows, surest first) and `RecognizerStats` to the UI.
  TensorRT is loaded at run time from `deps/` (no import libraries); the engine for this GPU is built once per new
  model.onnx and cached as `model.engine` (one Detector loads or builds at a time; a second waits, then loads it).
  Paused while reviewing (the pipeline stops handing it images). While recording it also writes each look into the
  live log (`setLog`), down to 10% sure. When the UI sets its reload flag (`reloadKinds`), it reads the kind update
  at the top of its next look and hands it to its Detector.
- **Finder thread** (`engine/Finder`, below-normal priority, only while a review needs guesses): its own GPU device
  and `Detector`; reads the recording's JPEGs and writes `<category>-guesses.txt`.
- **Refresher thread** (`engine/Refresher`, while a review is open and its finder is done): lists the other reviewed
  recordings whose guesses or features are stale and runs a `Finder` on each in turn.
- **Kind update thread** (`engine/KindUpdater`, below-normal priority, after a review closes): reads every reviewed
  recording's answers, guesses and features, fits and writes `kinds-update.json`; the UI then tells the recogniser
  to reload. Stopped, it writes nothing.
- **Recorder thread** (`engine/Recorder`, below-normal priority, only while recording): its own GPU device reads the
  recording channel into a CPU-readable texture and writes numbered JPEGs (`engine/ImageFile`, WIC). Slow disks or
  encoding can never hold up capture; images that arrive while one is saving replace each other. Each saved image
  gets a `saved` line in the live log.
- **Clip thread** (`engine/ClipRecorder`, while a screen is observed; the user asked, 2026-11-10): the clip buffer.
  Its own GPU device reads the clip channel (every new image while `Pipeline::setClipEnabled`) and has the GPU's
  video encoder (NVENC, loaded at run time from the driver's `nvEncodeAPI64.dll`, API 13.0) compress it into H.264,
  synchronously: a frame each time the screen changed, timed by when the screen showed it, at least a frame gap apart
  (1/144 s, or what H.264 level 5.2 allows at the screen's size). Within a gap it copies nothing; the image the screen
  shows when the gap ends is the next frame, timed a moment later (under a gap) if the screen showed it sooner, so the
  video keeps to its level (a 240 Hz screen gives about 120 frames a second: every other image). No B-frames, a key frame at least every 0.5 s (a still screen's
  last image is repeated as one), constant quality 22 within 120 Mbit/s, BT.709 limited range. It keeps the
  compressed frames, cut at key frames, back to the last key frame at or before 10 s ago, plus every look the
  recogniser hands it (`Recognizer::setClip`: what it found down to 10% sure and what the tracker followed, through a
  fixed 64-look queue that never waits; drops are counted) and the UI's notes. `save` hands a copy of all that (the
  frames shared, not copied) to a writer thread: `clip.mp4` (Media Foundation's sink writer, the frames as they are,
  no second encode; MP4 timescale 144000, so the frame times stay exact), `clip.txt`, `picture.jpg` (the newest
  image) and `clip.observex` last. A clip is 10.0 to 10.5 s. Measured 2026-11-11 (Release bench, 2560 x 1440 at 240
  Hz, pulse, `--model models/character`, 4 runs each with and without `--clip`): 120 frames a second at about 4.3 ms
  each, capture unchanged (239.8 images/s), the recogniser 3.28 -> 3.57 ms a look (239 -> 238 a second), a save takes
  under 0.1 s; the plain desktop makes about 7 MB a clip. (Before the gap rule it copied every image, 240 a second,
  and encoded 150-160 frames a second, over the level: 0.8 ms more a look.)
- **UI thread** (`app/App`, the main thread): `MainWindow` (Win32 window, swap chain, system-wide hotkeys: Ctrl+Alt+R, Ctrl+Alt+M, Ctrl+Alt+C), ImGui, `Theme`
  (fonts, colours, logo, the boxes and tags drawn over screen images, shared by the review and the live view), `Widgets` (`BarButton`, the top-bar button look), `ScreenPicker` (the miniatures),
  `ScreenImage` (a screen image the UI can draw at any size: it keeps a mip chain so shrunken views look smooth, and
  draws opaque), `Thumbnails` (the Review list's pictures), `Settings` (observex.cfg, folder picker) and `Review` (the review screen; it reads each image with
  `engine/ImageFile` on the UI thread, 11 to 17 ms for a 2560 x 1440 JPEG, and keeps marks in `core/Marks`).

The threads share data only through lock-free hand-overs, so the UI and the recorder can never slow the pipeline:

- `TripleBuffer<T>` (core): newest-value hand-over for stats.
- `ImageChannel` / `ImageReader` (engine): three shared GPU textures rotated by a TripleBuffer; keyed mutexes order
  the two devices' GPU work. The reader copies each new image out right away and hands the texture straight back.
  One channel for the UI preview, one for the recorder, one for the recogniser and one for the clip buffer (their
  readers also wait on an event, so they sleep until an image arrives).
- Atomics for state, the preview and recognition on/off flags, the recording rate and the model preparing progress.

Lifecycle: the UI thread owns the pipeline. Picking a screen starts it on that screen; a display mode change restarts
it on the same screen. Capture interruptions (fullscreen switch, UAC prompt, lock screen) are recovered inside the
pipeline thread. The picker captures the screens itself only while it is shown (on the UI device, never waiting), and
not the observed one, whose miniature reuses the observed image. A recording belongs to `App` (`Recording`: folder,
start time, images saved, its `RecordingInfo`); a pipeline restart only pauses it (the `Recorder` is destroyed before the pipeline stops
and made again after, numbering on in the same folder). Picking a screen, a disconnected screen or a save error
ends it. The `Recognizer` lives with the observed screen: made in `App::observe`, destroyed in `stopObserving`
before the pipeline frees its channel (its destructor also cancels preparing a model). So does the `ClipRecorder`
(made in `observe` and handed to the recogniser; destroyed after the recogniser, finishing a save still being
written), so the clip buffer starts empty on each pipeline start.

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
  runtime parts into `deps/` (gitignored, kept across clean builds), each checked by SHA256 (`cmake/TensorRT.cmake`),
  and NVENC's header (`nvEncodeAPI.h`, FFmpeg's nv-codec-headers n13.0.19.1, `cmake/Nvenc.cmake`).
- Build: `cmake --build --preset debug` (or `release`)
- Test: `ctest --preset debug`
- Bench: `build/vs2022/bin/Release/observex_bench.exe [--screen N] [--seconds S] [--no-pulse] [--no-preview]
  [--record FOLDER [--every S] [--unsure]] [--model FOLDER] [--clip FOLDER]`. Lists the screens and measures one (the
  primary by default). `--clip` runs the clip buffer as the app does, prints its numbers (encoded/s, encode ms, seconds
  and MB held, dropped looks), notes halfway, and at the end saves a clip into a new folder in FOLDER (a scratch
  folder: it shows the screen) and says what it wrote; `observex_bench --track FOLDER/clip.txt` replays its looks.
  `observex_bench --clip-frames CLIP --out FOLDER [--every N]` decodes a clip as the player does and writes
  `frames.txt` ("FRAME TIME_MS" per frame) and a JPEG of frame 1, every Nth and the last, to look at a clip's frames
  (1.7 ms a frame on the GPU at 1440p). `--unsure` records like Record's "Only unsure" (needs `--model`; its answers are the log's `mirror`
  lines).
  Its "pulse" is an invisible 2 x 2 pixel, click-through, never-focused window in the screen's corner that presents
  every refresh like a game, so the screen has something to capture. `--record` saves images like the Record button
  does (into a scratch folder, never the repo), so its cost shows in the numbers; with `--model` too it writes the
  live log like the app and marks once halfway (the way to test the log headless). `--model models/character` runs the
  recogniser too and prints its numbers (recognised/s, ms per image, found). `observex_bench --model FOLDER --images
  FOLDER --out FILE` runs the C++ detector over a folder of JPEGs and writes its guesses in the marks format, kinds
  included (to compare with Python's `guess.py`), and each image's mirror test to `FILE.mirror.txt` (prints the median and how
  many Only unsure would save; for a kinds model also how many are unsure which kind something is, and the total). `observex_bench --track LOG` replays a live log (a recording's
  `character-live.txt`) through the C++ tracker and prints boxes appearing or vanishing per second with and without
  it, things followed, and its cost per look; for a log with `kinds` lines also how often a thing's kind changes per
  second (the model alone: a box overlapping half of one on the look before, as another kind; followed: an identity
  shown as another kind). No GPU. Use it after any tracker change.
  `observex_bench --model FOLDER --replay RECORDING` (2026-11-08) walks a reviewed recording as the review would, its
  answers as the user, and prints how many guesses the model's own rule and the learning rule pre-answer right and
  wrong and how many they ask about (each also with answers carried forward), and how long the fits take; then the
  box nudge: of the guesses the user kept as boxed and of those they boxed otherwise, how many land within 0.7 / 0.8
  overlap of the user's box as boxed and nudged, and its fit times; no GPU.
  Like the review it starts from the kind update when FOLDER holds a valid one (then the last line, how far the
  guesses file's kind sureness is from the classifier's, is large if the guesses were made without it).
  It needs the finder's guesses and features in the recording: on a copy, take the answers file out, open its review
  once (snapshot mode), then put the answers file back.
  `observex_bench --model FOLDER --update-kinds RECORDINGS` (2026-11-08) makes the kind update headless: it re-makes
  every reviewed recording's stale guesses and features with the finder, one at a time (`Refresher`), then runs the
  update job and prints what it did (written: answers, recordings, fit ms; nothing new; or which recordings still
  need guesses). It writes into FOLDER and RECORDINGS, so only ever run it on scratch copies (jpgs hard-linked).
  Measured with the tenth on copies of 27 reviewed recordings: fresh in 46.1 s (25.2 ms per image, finder start
  included), 11,083 answers fitted in 485 ms; run again, "nothing new" in 0.02 s.
- The app: `observex.exe` in the project folder. The Release build writes it there (gitignored). The Debug build
  stays in `build/vs2022/bin/Debug/`.
- UI snapshot: `observex.exe --snapshot <file.png> [--click X Y | --drag X1 Y1 X2 Y2 | --right-drag X1 Y1 X2 Y2 |
  --scroll X Y TURNS | --key [Ctrl+|Shift+]NAME]... [--mouse X Y] [--recordings FOLDER] [--wait SECONDS]`. Draws the UI in a
  hidden window (it never appears or takes focus), plays each input step in order about 0.6 s apart (clicks, drags,
  wheel turns at a point, positive away from the user; key presses named as ImGui names them: `Y`,
  `RightArrow`, `Delete`, ...; `Ctrl+Z` holds Ctrl with it), rests the mouse at `--mouse` for hover effects, waits
  `--wait` more seconds (at least; e.g. for the finder; frames take longer than 16 ms, so the real wait is longer),
  saves the frame as a PNG and quits (closing a review on the way, as closing observex does). Points are
  window pixels, as in the PNG (take one snapshot first to find them). It really observes and records what you
  click. Snapshot mode never shows a Windows dialog. `--recordings FOLDER` is the folder the Review list reads
  (default `recordings/` next to the PNG); point it at a copy, since answers in a review are saved. Use a build with no console window: the Release exe, or, while the user has `observex.exe` open (then the
  Release link fails), `cmake --build build/vs2022 --config RelWithDebInfo` and
  `build/vs2022/bin/RelWithDebInfo/observex.exe`. Save snapshots and test recordings outside the repo, since they
  show the user's screens. Snapshot mode does not read or write observex.cfg, never registers the hotkey,
  records into `recordings/` next to the PNG, and never makes the kind update (it would write into `models/`); the
  refresher does run there, so `--recordings` must be a copy.

## Layout

- `src/core/`: platform-free building blocks (`TripleBuffer`, `StatWindow`, `Marks`: boxes, answers, the marks file
  format, carrying answers forward, which guesses start as yes, matching guesses to answers for the kind update;
  `ReviewHistory`: the review's Ctrl+Z; `ImageView`: the review's zoom; `BoxLearner`: nudging the review's guesses' boxes the way the user boxes them;
  `KindLearner`: re-fitting the kind classifier to the review's answers and for
  the kind update, and picking kinds with it; `Features`: the `<category>-features.bin` format and when it is fresh;
  `ModelInfo`: model.json's kinds and kind classifier, and `kinds-update.json`; `LiveLines`: the live log's `look`
  and `kinds` lines; `ClipLines`: a clip's `frame`, `shown` and `note` lines; `ReviewLines`: the review log's words;
  `Recordings`: the `recording.observex` file, listing recordings and their images, and `clip.observex` and listing
  clips; `Detection`,
  `Tracker`: following found things from look to look). No Windows or GPU headers. Unit tested.
- `src/engine/`: the real-time pipeline, the recorder, the recogniser and the clip buffer (`Gpu`, `ScreenCapture`,
  `ImageChannel`, `Pipeline`, `Recorder`, `ImageFile`, `Detector`, `Recognizer`, `Finder`, `Refresher`, `KindUpdater`,
  `ClipRecorder`, `ClipPlayer`). Windows, Direct3D 11, CUDA, TensorRT, NVENC and Media Foundation, no ImGui.
- `src/app/`: the window and UI (`App`, `MainWindow`, `Theme`, `Widgets`, `ScreenPicker`, `ScreenImage`, `Thumbnails`,
  `Settings`, `Review`, `ReviewLog`, `Snapshot`, `main`), plus
  `observex.rc.in` and `Resources.h` (icon, fonts and version info built into the exe).
- `bench/`: `observex_bench`, the headless pipeline benchmark.
- `assets/`: the logo (`logo.svg`, `logo.png`) and the app icon (`observex.ico`), all made by `tools/make_logo.py`.
  Change the logo there and rerun it.
- `tools/train/`: the training tool (Python: `marks.py` reads and writes the marks format of `core/Marks.h`,
  `train.py`, `guess.py`, `export.py`). The trained models go to `models/` (gitignored): `model.pth` (Python),
  `model.onnx` (live recognition; also each candidate's features), `model.json` (its kinds, score and kind
  classifier), `model.engine` (made by observex for this GPU) and `kinds-update.json` (made by observex, the kind
  update).
- `tests/`: doctest tests for `src/core`. Add tests with any change to core.
- `docs/`: plans that span many sessions. `training-loop.md` is how observex learns to recognise things.
- `cmake/Dependencies.cmake`: Dear ImGui and doctest via FetchContent, and the Inter font files. Pin every dependency
  to a release and its SHA256. `cmake/TensorRT.cmake`: TensorRT and the CUDA runtime into `deps/`.
  `cmake/Nvenc.cmake`: NVENC's header into `deps/` (the encoder itself comes with the NVIDIA driver).

## Conventions

- Namespace `observex`. Types `PascalCase`, functions and variables `camelCase`, members end in `_`, constants `kName`.
- Format with `.clang-format` (clang-format ships with Visual Studio under `VC/Tools/Llvm/bin`).
- New source files must be added to the target lists in `CMakeLists.txt`.
- Our targets build with `/W4 /permissive-`; keep them warning-free.
- Setup code reports errors by throwing (`check()` in `engine/Gpu.h`); the hot path returns status codes instead.
- ImGui and the UI's GPU device are used on the UI thread only. The pipeline never touches UI objects.
- UI colours and sizes come from `app/Theme`; scale pixel sizes by the DPI scale.
