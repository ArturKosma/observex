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
on the right, then the tabs Live, State, Stats, Skills, Log and Debug, whose tiles share the view under them (see Tabs and tiles). Right
of Show (while a screen is observed live), muted, what observex spends on a screen image in all ("12.3 ms"; the user
asked, 2026-11-12, for the last frame's total: `App::imageWorkMs` adds each running part's own average time per image
over the last second; a tooltip on it lists each part). Since 2026-11-15 it is the Debug tab's total (see Debug):
`App::costs` (every part's `core/Costs` meter, published once a second) and `costTotalMs`; before, it counted the UI
finder's stats reading twice and left out drops, the clip buffer and the app's own frame.
The screen button looks like a dropdown (monitor icon, text, arrow) and reads "Choose a screen", or "Observing" and
the observed monitor's name. It opens live miniatures of every
screen, laid out the way the screens are arranged on the desktop, each labelled with its monitor's model name (from
the monitor itself, e.g. "Odyssey G61SH"; size and refresh rate show on hover). Clicking one starts observing it, and
the window shows only that screen, live. With nothing observed yet, the miniatures fill the window under the logo.
F3 shows the pipeline's and the recogniser's numbers over the view (hidden by default).

Live recognition (step 5 of the training loop): while a screen is observed, the recogniser runs the trained character
model (`models/character/model.onnx`) on every new captured image, on its own thread, whatever is shown: also while
minimized or with the flag off. While reviewing it pauses (the user asked, 2026-11-01): the pipeline hands it no
images, so the review's finder gets the GPU; the model stays loaded and it picks up again when the review closes. It
pauses while a clip is open too (the user asked, 2026-11-11: a clip is offline data, and running on meant the GPU
66-93% busy and the video encoder 21%, from the model and the clip buffer).
**Show** (eye icon, right of the screen button, only while the screen
is shown) is a dropdown of flags like Unreal's view flags: **Characters** draws a violet box tagged "Character 81%"
around each character the tracker follows (saved in observex.cfg as `show-characters`). Under the flag, only
while it cannot draw (since 2026-11-12 the user wants no explanations or numbers under the flags; F3 has the
numbers): the progress of preparing a new model for the
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
the kind is the update's surest, the second its next, each `sure` x its share of the update's kinds). (Under Kinds a
dim line said "Kinds also learned from 11,083 of your answers" until 2026-11-12.) It is made only when some answers are newer
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
**UI** (the user asked, 2026-11-12: step 1 towards a per-pixel depth model of the screen that leaves the game's
interface out, and later reading HP or a target's name from it; `show-ui`, off by default) draws every piece of the
game's interface the UI finder found: filled see-through cyan (`theme::kUi`), outlined, tagged with its name ("Chat",
"Window"). The finding goes on whatever the flag says (the user asked, 2026-11-12: the flag only draws), while a
screen is observed; it pauses with the recogniser while a review or a clip is open. An open clip has a finder of its
own on the frames shown (`ClipView`, handed the frame shown when it changes, at most 10 a second; the Guesses popup
has the same UI flag, the user asked, 2026-11-12). `engine/UiFinder` (below-normal priority, own GPU device): the
pipeline hands it a new image every 100 ms (`UiFinder::kImagesPerSecond`, the ui channel), `GreyMaker` (keeping one) makes it grey on the GPU, only the full-size level comes back (3.7 MB), and
`core/UiLayout::find` finds on it, needing no model:
- Framed windows ("Window": Map, Skills, Inventory, Character Status, the minimap, ...): Lineage 2 draws each with a
  1-pixel light frame line (grey 120 or more and 40 brighter than the pixels 2 away on both sides). The lines are
  found in rows and columns, joined across gaps up to 30 (tabs, buttons) while they stay solid, and kept from 40
  pixels long if solid: line pixels 65% of the length or more. Text makes lines too (its letters' tops along a row,
  60% at most, 39% on average; tall letters stacked down a column, mostly under 65%); frame lines are 95% and more,
  but the Actions window's side beside its close button is 69% (75% lost it). Before (the user's clip
  2026-11-12_14-21-53, the Inventory slid over the chat), chat text made a window that cut the chat short. Two
  upright ones at least 40 apart standing on a level one (within 6), with a level one covering half the width where
  they end (or sides 150 or longer), make a window. A top line is looked for near where both sides start (up to 60
  above, the title bar): near only one side's start it was another window's (the Inventory's bottom made the
  hotbar's top, over the ground between them, its right side starting 280 lower). Requiring the top line to reach
  both sides was tried and lost the title bars of windows whose top line breaks at a corner (the minimap's buttons,
  close buttons). The screen's left and right edges stand in for a side there (the
  Inventory at the right edge) when both level lines reach the edge (since 2026-11-12: the chat's tab row, from x 104,
  and the hotbar's end made a window over the ground between the widened chat and the hotbar), and only where no
  side line of its own is within 12 pixels of the edge (the Equipment panel was two windows). A window partly under
  another is found whole: a first look finds rectangles (the visible part, where it is one), then lines ending at a
  found window's edge from outside it, on its rows or columns, are taken to go on under it to its far edge, and a
  second look keeps a window only if at least one corner is under a window of the first look, each other corner is
  where its side's own line ends (not along a longer line: two windows' lines that only line up made a window over
  the ground with five windows open), and it covers something those do not (so two windows side by side stay two). Before (the user's clip
  of 2026-11-12), the minimap with the Inventory over its lower left kept only its strip right of the Inventory.
- ui.txt's pieces (Chat, Messages, Hotbar, Status, Target, Menu, Bar: no frame, partly see-through; and Minimap, by
  its compass, for when bright sky behind it hides its frame: a frame line must be 40 brighter than the pixels 2 away
  on both sides, and against white sky it is not, so 60% of clip 2026-11-12_11-34-03 had no minimap; its compass is
  just inside the frame, so where the window is found it wins; and Buttons, the icon panel at the bottom right, which
  has no frame of its own, by its first button's corner or, when a tooltip covers that, its last button):
  `models/ui/ui.txt` (gitignored with the models; its header explains its lines) names anchors, small pictures cut
  from clips by `tools/ui/make_anchors.py` (`NAME.pgm`, their mean over the frames named, and `NAME-mask.pgm`, the
  pixels that never changed there: the interface's own, so the world behind never counts). They are found anywhere
  on the screen by zero-mean correlation over the mask (an SSE2 test of up to 8 bright/dark pixel pairs first, at most
  64 hits each), one per place across anchors (the larger score x sqrt(masked pixels) wins), and each piece sits
  relative to its anchor. A side can stretch to another anchor (`right resize-corner ...`: a resized chat; the search
  stops at the next window's anchor), or to the nearest frame line (`right line`: the target window's right edge).
  A side may name several ways, tried in turn (the Chat's right: its upper corner `resize-corner`, then its lower one
  `chat-corner`, for when a window covers the upper; cut from three clips so the chat's scroll thumb, which moves
  with the text, is left out of it: cut from two, it held the thumb where they had it, and on clip
  2026-11-12_14-21-53 the corner was never found, so the Inventory over the upper corner cut the chat short;
  make_anchors.py keeps a pixel that is the same grey at every place within 12, as well as the same after evening
  out each place's brightness, which had dropped the corner's frame line); the row or column an anchor is looked for at keeps its measured
  distance from the piece's nearer edge (a taller chat still finds its lower corner). A side found no way ends where
  a framed window begins across its way, if no farther than the size measured: the piece's visible part, the window
  covering the rest (the user's screenshot of 2026-11-12: Chat and Messages were lost where the Character Status
  window covered their right corners). A main anchor's hit well inside a framed window makes no piece (the
  Inventory's scroll arrows made a false Messages).
The Status window has two anchors (2026-11-15 night): `status-left`, its bracket and the bars' left ends, lost the
window whenever a bar's left end was empty (clip 2026-11-15_23-08-49: the XP at 0.04% after a level up, 0.858 against
0.90; 0 MP or HP would do the same), so `status-labels` finds it by the bars' "HP" and "MP" letters, their white strokes
and dark outline only (`keep 80 200`: the bars' colours behind left out); the window and its level box are see-through
and its bracket is the target window's too (a bracket-and-level-box anchor scored 0.94 on the target window). Over the
39 clips' pictures it scored 0.998-0.999 at the window and at most 0.60 anywhere else; the clip that had lost the window
read 227/227 HP, 186/186 MP and 0.04% on all 50 frames checked.
Pieces inside others are left out. ui.txt and its pictures are read again within a second of changing; while it
cannot be read, the one before goes on (with none, only framed windows are found) and the reason shows under the
flag. F3 adds "UI found 10/s, N ms each (finding N)". Measured 2026-11-12 with `observex_bench --ui CLIP` on 8 clips
(2026-11-11 and -12, the camera turning, the user moving, resizing, opening and closing windows): Chat, Messages,
Hotbar, Status and Bar on every frame, the target window and the menu on the frames checked by eye, framed windows
too, nothing on the world; a find takes 3.2-5.2 ms on average (9 at most) on the CPU. Live (`--find-ui`): 10 images a
second, 5.8 ms each, finding 4.7 of them, on a desktop (22 ms on a desktop full of icons); the recogniser's looks
unchanged within noise (3.38-3.85 ms with it, 3.34-3.60 without). After the overlap rules (same day, the 8 clips
again): the same pieces on every frame, except Messages ending where the Character Status window covers it. Two small
false windows on the hotbar's bottom row (the frames of its end slots; inside the hotbar, so harmless). After the
user's second screenshot (same day, 10 clips, 11,535 frames): the false window at the bottom left (on 1,806 frames,
all with the widened chat) and the minimap not whole (1,385: the Inventory over it, bright sky behind it) on none;
Buttons on every frame; contact sheets of 40 frames checked by eye. A find takes 4.9-9.6 ms on average (23.6 at
most, on the clip with five windows open; with the game running alongside). After the user's third report (same
day, clip 2026-11-12_14-21-53 around frame 521: the Inventory slid over the chat, "chat boxes go crazy"; 11 clips,
12,740 frames): the chat cut short on 674 frames (two clips) and false windows over its text on 363, now none; the
hotbar's box over the ground above it on 577, now none; nothing else changed but boxes covering the same together
and the hotbar's end-slot boxes gone. A find takes 4.2-8.0 ms on average (13.6 at most). Not found
yet: a piece whose own anchor is covered (only the covering window is marked), tooltips, party windows, buffs, a
vertical hotbar; towns are untested; a frame line on very bright ground may be missed. The layout is the user's UI scale at 2560 x 1440: another scale or screen size
needs its anchors cut again.
**Stats** (the user asked, 2026-11-12: numbers to feed a state machine, "a button to show all the stats on the right
side of the screen. its toggleable", recorded so a saved clip shows them, and "make sure the info is a separate dll so
we can hot reload with new heuristics and compare to recorded"; the design is to be iterated by the user): on every
image the UI finder looks at (10 a second, its thread), `heuristics/StatsReader` reads the bars `models/ui/stats.txt`
names on the pieces found (Status: HP, MP, XP; Target: HP; the file's header explains its lines): the number first, by
masked correlation of each glyph of a fixed-width font (`digits.pgm` and `digits-mask.pgm`, 13 characters `0-9/.%`,
pitch 10, cut by `tools/ui/make_digits.py` from the `from` lines: run it after adding one), only where a column has a
white pixel, then the longest run of glyphs a pitch apart (a label letter taken for a glyph is left out); then how full
the bar is, where the first run of its colour from the left ends, the label's and the number's columns left out (the
number's letters cover the bar top to bottom). Past that end the bar must be dark (its empty colour) along its middle
row, else a window is over it and how full it is is unknown (`full` below 0). A number that disagrees with the bar by
more than 2% is not kept (`doubted`); one that agrees gives the exact share. Only the pieces' rows come back from the
GPU, in colour (a staging copy, the lent image then goes back). It is `observex_stats.dll` (a DLL of its own,
`heuristics/StatsAbi` and `StatsPlugin`; `engine/StatsLibrary` loads it from a copy in `%TEMP%\observex-stats` and
again when rebuilt, like the heuristics DLL; the UI finder makes a new reader from its next image; without the DLL
the rules built into the app read): **to change the reading while the app runs, edit `src/heuristics/StatsReader` and
build only `--target observex_stats`**. The **Stats** tile (its tab under the top bar; live and for a clip;
`app/StatsPanel`, `show-stats`, off by default; until 2026-11-12 night a button and panel over the image) shows
groups You (HP, MP, XP, Casting), Cooldowns and Target (HP, Name): a small bar, the number, the percent; a doubted
number muted with "?", an unknown one a dash (no DLL or build-time text since 2026-11-13: the user asked). App
keeps the last 15 s of what was read (`statsHistory_`) and a clip save writes them beside the clip as
`clip-stats.txt` (`core/GameStats`' stats lines, by screen image, ms before the save, spaces in names as `_`); on a
clip the tile shows ONE column (the user asked, 2026-11-13), by the Guesses popup's Characters choice: Recorded (that
file, at the frame's image) or Today's heuristics (the clip's own UI finder on the frames shown, by the newest DLL; a
rebuilt DLL reads the frame shown again). Clips saved before 2026-11-12 evening say "Not recorded in this clip".
**Casting, skills, target name** (the user asked, 2026-11-13; stats ABI 3): the cast bar (ui.txt piece `Casting`,
anchor `cast-left` cut from clip 2026-11-12_23-25-45, `unlike` frames so the empty bar's place is not taken for it):
how far the light blue fill's head is. **Add Skill** (in the **Skills** tab since 2026-11-14, kept in the profile: see below; not hardcoded, so it works
on other chronicles): the UI finder finds every miniature on the observed image (`heuristics/Miniatures`: top and left 1-px
bevel lines; up to 40, the Hotbar piece's first, copied to a 2560x64 atlas on the GPU) and the popup shows them; the
user picks one (while the skill is ready) and names it, saved as `models/ui/skills/NAME.pgm` (grey; the X on its row
deletes it; the UI finder reloads the reader when that folder changes, so skills come back on the next start). Each
skill is found among the hotbar's miniatures by its pixels, near the picture or its darkened picture (`kSkillMatch`
22 grey levels, a pixel either way), then looked for only where it was (a few hundred pixels; the whole Hotbar only
when it is missing, at most once a second); its cooldown left from the bright sector growing back clockwise (72
sectors), its seconds from the yellow font (`glyphs cooldown 123456 ... yellow`, bit rows + popcount). Casting's name
is the skill whose cooldown began last. The target's name: Windows' OCR (`heuristics/TextOcr`, Windows.Media.Ocr,
windowsapp.lib) on stats.txt's `words Name Target` line, only on a new picture of it (same white pixels as the image
before, or a hash seen before: the old answer), the most-read spelling while its window stays (a letter or two off
is the same target). **Target Changed** (the user asked, for the Log session): `GameStats::target` (None or the
name) changes once a new one holds 2 readings, `targetChanged` on that reading. Measured 2026-11-13 on clip
2026-11-12_23-25-45 (100 images at 10 a second) with the user's own Wind Strike picked through Add Skill: found on
100/100, seconds 6..1 on the right frames, Casting 17%..99% named Wind Strike, target 'Gremlin' 100/100 with one
Target Changed; 0.57 ms median a read (0.50 bars only), OCR ~2 ms on a new name (7.7 ms the first, with its start).
Measured
2026-11-12 with `observex_bench --stats` on 1,980 recorded images: HP's number kept on 1,886 of 1,890 images where
its bar was seen, MP 1,890 of 1,894, XP 1,892 of 1,914 (the rest doubted, mostly under a Quest window); bars hidden
by a window: HP 26, MP 22, XP 2, the target 65 of 1,169 (not checked by eye); 0.6 ms a read (1.3-1.4 ms live).
Checked in hidden snapshots on the user's game the same evening (level 3: 123/123, 90/108, 11.67%, the target's
bar 100% then 0% as it died), on a clip (Today read 256/256, 105/105, 30.34% off the video), a test DLL change shown
on an open paused clip without restarting, and a saved clip's `clip-stats.txt` image numbers matching its frames.
Not read yet: CP, buffs, party (the target's bar has no number).
**Skills tab and the precision pass** (the user asked, 2026-11-14: "stats should be VERY precise ... as whole state
tree will depend on them", "dont try to remember screen position, as i will often be moving those bars around", be
very verbal in the log, and stress tests "in difficult scenarios ... very precise in reading, and doesnt cost me much
in terms of frame time"; stats ABI 4). **Skills** is a tab of its own (closed by default; open, it covers the Stats
and State area): Add Skill (the miniatures found, as before), each skill a row with its icon, name, **Has Cooldown**
(without, it is found but not in Cooldowns: Sit) and an X. Skills live in the profile (`.brain` line `skill
cooldown|no-cooldown SIDE PIXELS NAME`, the miniature as hex BGR; `models/ui/skills` is no longer written). The Stats
tile shows each skill's icon by its cooldown (no X there), Casting's skill name and icon, Target as a right column
when the tile is wide enough, and **Nearby** (Characters, Monsters: the character model's tracked boxes; also numbers
in Compare Number, `characters` / `monsters`). Tasks are hardcoded kinds (`TaskKind`: **Use Skill** + a list of skills,
**Select Target** + what (Nearest Monster), **Click Mouse** + a button; file `task ID use-skill SKILL|SKILL...`, `task ID
select-target nearest-monster` (`lock-mouse`, Lock Mouse Onto's word before 2026-11-15, reads as it), `task ID
click-mouse left|right`; a typed task of an older file is dropped). No Rename in the state menu (the name field
does it); a quiet "Details" over the details; Load has a folder icon. Target Changed lines are gone from the Log.
**Why lines**: every reading says why something is not read (`GameStats::why`, 512 chars, " | " between: "Target: no
window", "Wind Strike: not at 1043,1376 (far 15.2, alike 0.37, no miniature edge)", "Name: read 'Woif' ... none
agree, took ...", "Status MP: number not read, bar ends at 54% past where it begins"); a clip's `clip-stats.txt` has
them as `why IMAGE TEXT` lines after the image's stats line. Nothing remembers a screen position: the UI finder finds
the pieces on every image (10 a second), the reader reads only their rows. **What is read and when it is refused**
(`heuristics/StatsReader`): a bar's fill needs a third of its rows (2 at least) of its colour in a column (lilac by
hue, so a darker or lighter screen keeps XP); past the fill the bar must be its own empty colour (stats.txt `empty
red|blue|grey`, default dark) on 9 of 10 columns looked at, else a window is over it: unknown. With no number agreeing
(none read, or one off by more than 2%: doubted), a fill ending past where the number began when last read (less 16
px) is not taken: a window across the bar ends the fill exactly where it hides the number. A **close** line
(`close Target 13 23 -20 -9`: the window's X, rows from its top, columns from its right) is looked for (diagonal light
pixels, score 0.3, 3 px play): without it something covers the window's right end and nothing on it is read; of
several pieces by one name the one with its X is read (the hotbar's row arrow next to a dark window looked like the
Target's corner); a Target window without its X counts only if it was whole on the image before at the same place.
Skills: found where they were by pixel distance (22) plus shape likeness (0.3) plus a light miniature edge (`framed`:
half the edge pixels light, a pixel either way); the look after a key press (the picture dimmed in a light frame) by
likeness 0.5, or 0.2 if seen within 12 images, plus the edge; yellow seconds there mean cooling, not pressed. The
target's name: Windows OCR on the picture drawn 3 ways (blocks x4, smooth x4 and x3 on the picture's own ground and
letter levels); two agreeing are taken, else 2 more drawings (smooth x4 on softer levels, blocks x3) and the 5 vote,
a name agreed before winning ties; a name picture more than 4 px wider or narrower than the name's, not read, clears
it (the target changed). **Measured** with `observex_bench --stats DIR --list --skills BRAIN` (also `--pieces`: the
pieces found; `--ocr PGM...`: Windows OCR on pictures) on 664 images of 22 clips (all agree with the reads before the
pass, which were checked by eye) and on 81 of them in 8 made-hard variants (12% lighter, 15% darker, gamma 0.8 and
1.25, the Status/Target/hotbar windows moved, a window pasted over the Status bars, the Target's right end and the
hotbar): no wrong HP/MP/XP/target/name read anywhere (before the pass: 61); left unread instead: bars under a pasted
window, and a cooldown's seconds on the 12% lighter screen (its pale yellow clips to white; still "cooling"). 0.74 ms
average a reading (1.1 ms slowest without OCR), OCR 2-10 ms only on a new name picture, all on the UI finder's
thread. The bench's scratch tools (frame bank, stress variants and comparers) were not kept.
**Profile and shortcuts in the Skills tab** (the user asked, 2026-11-15: the profile chooseable there "same like with
State"; save each skill's hotbar key "next to Has Cooldown", for the Pico to press later; "keep observing if i change
the skill's position on the skillbar ... update the miniature and the shortcut"; stats ABI 5). The tab's top is the
State tile's Load + profile name field (`StateTreeView::drawProfileBar`, its own text buffer) and + Add Skill after it
(under it in a narrow tile). Each skill row has **MP Cost** (2026-11-15, the user asked: "mana should be introduced
in the Skills tab"; a 52 px number field left of Shortcut, "–" for none: Use Skill is done once that much MP was taken)
and **Shortcut** (a field left of Has Cooldown; "–" while not read, amber
with a tooltip while it has '?'; typed in, uppercased, empty = "-" none). In the profile: `skill cooldown key=F2 mp=10
42 ...` (`key=` and `mp=` in any order, each left out while not known; older files read). A skill's name has no `_`
or `|` in it (the file's spaces in `cooldown:SKILL` and its lists); its fields go on a line of their own under the
name when the tile is too narrow for every name whole beside them. **Refresh** (right of + Add Skill; the user asked,
2026-11-15: "takes control over mouse, hovers over every registered skills by first finding their icon on the skill
bar, and copies the mana cost"; `core/SkillRefresh`'s `SkillRefresher`, unit tested; `App::toggleRefresh`,
`driveRefresh`): only while a live screen is observed, the brain inactive (starting the brain stops it) and the
controller answers; it brings the game's window (under the first skill seen) to the front, then for each profile
skill in order moves the cursor onto its miniature's middle on the hotbar (where the stats reader last saw it, as the
desktop's pixels; `core/CursorGlide`, the easing Select Target uses; not seen within 0.6 s: skipped), holds it there
0.5 s for the game's tooltip, then asks the UI finder to read the area round it (`tooltipArea`: 12 miniature sides
either way, 14 above, 5 below, on the image) by Windows OCR on the next image (`UiFinder::readText` / `text()`: the
grey image's area turned over, levels 100..190 stretched, doubled smoothly; on the finder's thread), one read at a
time. `readMpCost`: the skill's name in the words (as written, or a letter or two wrong: `sameName`) and the number
after "MP" past cost words and marks ("MP Consumption 10", "MP: 10"), else "Consumes 10 MP", else "10 MP"; never one
with '/' or '%' (the Status bar's "90/108"); O read as 0, l or I as 1. Two reads in a row with the name and the same
cost: written into the profile (saved, Ctrl+Z undoes it); the name twice with no MP: its cost kept; nothing in 3 s:
given up on. Then the cursor goes back where it was. The button is highlighted while it runs, how far along its
bottom; a click or Win+Esc stops it. Log: "Refresh: the mouse goes over each of 3 skills to read its MP cost off its
tooltip (Win+Esc stops it)", "Refresh Wind Strike: MP cost 10 (was not set)", "Refresh Sit: no MP cost on its
tooltip, its cost kept", "Refresh Heal: not found on the hotbar, skipped", "Refresh Ice Bolt: its name not on its
tooltip in 3 s (read: "...")", "Refresh done: 2 of 3 skills read (not read: Heal)", "Refresh not started: observe a
game screen first". Not yet tried on the game (2026-11-15): the tooltip's look is not known here, so the first runs'
log lines (what OCR read) are what to tune it by. **Reading the key** (`heuristics/KeyLabels`, `KeyFont`):
the slot's label at a miniature's top left (cream letters, dark rim, proportional anti-aliased font, glyphs may touch,
custom binds like "A+K1") is read by glyphs in `models/ui/keys.txt` (`glyph SIDE W H GREYHEX KINDS [TEXT]`, KINDS
L letter / D rim / . not compared; per slot side = UI scale), not Windows OCR (it read "FIO"). Letter pixel grey >= 160
and not bluish (B <= R+24, G+24), rim grey <= 100, rows 1 to 0.22 of the side, start in the first fifth; glyph by glyph
from the left, each tried -1..+3 columns and a row either way, taken at correlation >= 0.8 with letters 60 lighter than
their rim and at most 15% of its letter count of rimmed letter pixels it lacks; within 0.02 the wider glyph wins; letters
no glyph knows give one '?' if 5+ rows tall from row <= 3 (a picture's corner under the label gives none). The stats
reader reads it only while the skill is ready (`GameGauge::key`, "-" no letters, empty not read; `GameGauge::at` the
miniature's box); a '?' adds a why line. Typing a key teaches (`App::keyTyped`): from the skill's newest live picture
(else the profile's), the unknown parts get the typed letters between the known ones (one each when as many, else one
glyph over them), a disagreement gives one glyph over the whole label; keys.txt is written (.tmp, rename) and the readers
reload it within ~1 s (the UI folder's change watch); the typed key stands while the skill stays on that slot.
**Following moves** (`App::followSkills`, live only, not clips/reviews): the UI finder publishes each ready skill's
picture, box and key (`UiSkillPictures`); the same slot (3 px) and key on 3 readings in a row, on another slot than
taken before or with another key than the profile's, replaces the profile's key and miniature (BGR), logs e.g. "Wind
Strike: shortcut F4 (was F2), moved to another slot, miniature updated", saves (Ctrl+Z in State undoes); a key with '?'
is saved too so the Pico never presses the old one. The skill is found on its new slot by the hotbar look-over (its
picture is compared without the key's corner). Seeded 2026-11-15 from Sit's picture (F, 1, 2) and clip
2026-11-14_14-50-29 frame 10 (F1's "1" over the white sword, "4"); on that clip's 1207 frames: Wind Strike ready on 456,
"F2" on all, Sit "F12" on all, no key read while cooling or pressed (the press flash misreads "?2"); Wind Strike pasted
into the F4 slot (the slot's label kept) was lost one image, then read "F4" at the new slot. Bench: `observex_bench
--stats --keys [--layout DIR] read BRAIN | read IMAGE X Y SIDE | teach BRAIN SKILL TEXT | teach IMAGE X Y SIDE TEXT`
(teach writes keys.txt); `--stats --list` prints each skill's `key`.
**Training screenshots and Drops** (the user asked, 2026-11-15: "a new way of teaching ... handmade", packages "by
date + hour", "15/11/2026 - 15:00", a shortcut Lineage 2 won't steal, "show up in Log", and a Drops category to draw
boxes on adena/loot around corpses, no kinds). **Shift+Win** (Win released with Shift down since Win went down and no
other key between, so Win+Shift+S and the like take nothing; `core/ShotKey`, unit tested) through a low-level keyboard
hook (`app/ShotHook`, only while observing, every key passed on; the Start menu does not open on Shift+Win) takes one
full-resolution PNG of the observed screen (the pipeline's `shots` channel, `Pipeline::takeShot`, a Recorder on its own
thread with `ImageFormat::Png`, 2560 x 1440 ~4.6 MB) into the hour's package: folder `handmade_2026-11-15_15-00` in the
recordings folder with a `recording.observex` of `kind=handmade` (`core/Recordings`: `handmadeFolderName`,
`handmadeStarted`, `handmadeTitle`; numbers after the highest there, `nextImageNumber`, so none is replaced). The Log
says "Training screenshot 3 saved in handmade 15/11/2026 - 15:00" (or why none was). Snapshot mode: `--shot` takes one
once observing runs. Review's chooser has a **Handmade** tile ("N packages · your Shift+Win screenshots, to draw boxes
on"), its list titled "Review your screenshots", each package by its title; then "What are we looking for?":
**Characters** or **Drops** (`kCategories`' `drop`: "Drop" / "Not a drop", no kinds; answers `drop-answers.txt`,
`reviewed-drop` in the package's file; with a drop model in `models/drop/` the review guesses drops too, the Finder
cutting each image into squares, below). No kind update for a category without kinds.
Checked in hidden snapshots: two PNGs saved with their Log lines, the tile, list and title, Drops chosen, a drawn box
tagged "Drop" saved to `drop-answers.txt`.
**As administrator** (found 2026-11-15: Shift+Win was not heard in the game): Lineage 2 runs as administrator (the
Reborn launcher is elevated and starts it; no compatibility flag or shortcut setting does it), and Windows keeps keys
typed into an administrator's window from a keyboard hook of a program that is not (UIPI), so Shift+Win (and the
Escape pause shortcut of the time) was deaf while the game was in front; run as administrator they work (the user checked). So observex,
started live and not as administrator, starts itself again as one (`app/ShotHook`'s `restartAsAdministrator`:
ShellExecuteEx "runas", the same arguments plus `--as-administrator` so it never loops, the same folder) and ends:
Windows asks the user each time. Not in snapshot mode (it stays as it was started, so the hidden snapshots still run
from a normal shell) nor under a debugger. **But the root `observex.exe` itself carries Windows' Run-as-administrator
compatibility flag** (HKCU `...\AppCompatFlags\Layers`, RUNASADMIN; found 2026-11-15 when a test of mine popped a
UAC prompt on the user's screen): starting it always asks, even with `--snapshot`. Never start the root exe for
tests: run hidden snapshots from `build/vs2022/bin/RelWithDebInfo/observex.exe`. If the user says No it goes on, and when it starts observing the log says
"Not running as administrator: Shift+Win is not heard while Lineage 2 is in front ..." (the Brain's Win+Esc, a registered hot key since 2026-11-15, is heard either way).
**Dead mark** (the user asked, 2026-11-15: "bias the guesser that something is dead once the target's red hp bar
reaches 0 without switching targets", find the target's box by "2 blue dots on the left and right of its name", "the
box nearest to it", "shift the weights of the box towards dead", "see the effect on my screen"). `core/TargetWatch`
(unit tested), fed each new live reading by `App::watchTarget` (from `keepStats`), with every box drawn (snapper's or
recogniser's; the own one too): while there is a target and its box is not known it asks the stats reader for the
plate over each box in turn (one a reading); a plate with coloured orbs read over any box (Select Target's look counts
too) makes the target the box nearest under it (findPlate's rule: middle within the box's sides +-40 px, row above a
third down it, no higher than plateArea, score |dx| + 0.5 |row - (top - 30)|); known, that box's plate is checked on
every reading (5 answers without it selected, or the box gone: looked for again). Its death: the Target HP bar, once
read above 0.5%, read at most 0.5% on 2 readings in a row (unknown readings skip), the target (GameStats::target, the
name) unchanged; once per target; the same name above 5% again after it died is another target. Then its box (or the
one found within 30 readings after, or a box the tracker lost and found again as a new id) goes to
`Recognizer::markDead(id)`: from its next look on, after the heuristics and before publishing, that id shows as the
"dead" kind at least 90% sure with its kind before as the second (so in the overlay, the snapper's boxes, the clip
buffer's Recorded things, the Nearby counts, and Select Target, which then never picks a corpse); the last 8 ids kept.
Looks: Select Target's first (`App::lookAt` keeps it as `brainLook_`), else the watch's (`App::sendLook`). Log lines:
"Target Imp Elder is box 12", "Target died: Imp Elder; box 12: marked dead", "Target died: Imp Elder (its box not known
yet: looking for its plate)", "Dead target Imp Elder is box 14: marked dead", "Dead target Imp Elder: its box not
found". Not checked live yet (the game was not on screen): the user is to watch a kill.
**Drops** (the user asked, 2026-11-15: "teach the model how to recognize drops", first "only around the target that
we recognized as dead", then the same evening "at all times": the camera turns away from a kill, the character runs off
and comes back, and drops seen while seeking monsters are worth stopping for). The drop model (`models/drop`,
`train.py --category drop`, tools/train/loot.py; one category, RF-DETR Medium at 576) never sees the whole screen at
once: a drop is 20 x 9 px of 2560 x 1440 with the camera far off, 8 x 6 once squeezed to 1024. It sees one
full-resolution **square** per look, scaled to 576; the Detector cuts it on the GPU (`Detector::detect`'s `DetectArea`:
the squeeze reads only the square; boxes come back in screen pixels). Round a corpse (`core/LootWatch`'s `lootSquare`,
unit tested; loot.py has the same rule): side 4 x the corpse box's longer side (L2J scatters a kill's drops up to 70
game units each way; the character is 47 tall), at least 192 px, at most the screen's height; its middle the box's
middle across and half the box's shorter side above its bottom; moved, not shrunk, inside the screen. Round drops found
with no corpse (`lootSquareRound`): the largest drop a tenth of the side (`kDropsSpot`), their spread within its middle
80%. **Training**: squares cut round the drops of the user's Drops answers (the corpses were mostly gone by the
screenshot): 8 per image of many sizes (a drop 5-25% of the side) with the drops anywhere in the middle 80%, 4 with none
in them, and 5 round each **look-alike** (2 on held-out images): a "no" answer, or a guess in `drop-guesses.txt` at
least 30% sure touching no drop of a reviewed image (every drop on it is marked, so it is not one), kept in the package's `drop-look-alikes.txt` so a later model that learned them no longer has to guess them; the last fifth of
the images held out whole. **Live** (`engine/DropFinder`: its own thread and GPU device, below normal priority, 20
images a second through the pipeline's `drops` channel whenever the screen is observed and no review or clip is open,
the Show flag on or off): with nothing to look at closely it **sweeps** the screen one square per image (`lootTiles`,
below: 31 squares, the whole screen every 1.6 s); with a spot from `core/LootWatch` (stepped every UI frame by
`App::watchLoot`) it looks at the spot and the sweep's next square in turn. Finds under 50% (`DropFinder::kMinSure`) are
dropped, and so are finds on the game's interface (the UI finder's pieces: the cast bar and the minimap's arrow looked
like drops). Spots: a box the dead mark (above) marks starts a **kill's** spot round it, following the corpse's box while
the tracker has it and then the middle of the drops last found (the same size); it ends kLostMs (1.5 s) after neither
the corpse nor a drop was seen (picked up), or kMaxMs (60 s) after the death; the same name marked dead again within
10 s is the same kill (a new box id), another death starts anew. With no spot, drops a sweep look found start a **found**
spot round them, following them while the close looks find them, ending kLostMs after they were last seen; a kill takes
over. Log lines: "Looking for drops round Imp (box 12)", "Drops round Imp: 2" (each time the most seen at once rises),
"Drops round Imp: none left", "No drops round Imp", "Stopped looking for drops round Imp after 60 s"; a found spot says
"Drops seen: 2" only once a close look found drops there too (one sweep look alone may be wrong), and "Drops gone".
**Show > Drops** (`show-drops`, on by default; drawn after the characters, `Overlay::drawLoot`): each drop of the
spot's newest close look marked by four gold corners 2 px outside it (`theme::kDrop`, a dark halo under them; a box's
lines hid a drop of a few pixels) and tagged "Drop 87%"; the close-look square itself is not drawn (the user asked,
2026-11-15: its corners, 192 px and more, looked like a drop's outline; F3 still gives its side); under the
flag, only while it cannot draw: no drop model yet, preparing it for the GPU (once, saved as model.engine), or why it
failed. F3: "Drops N/s, ms each, slowest, close square N px". **On a clip** (the user asked, 2026-11-15 night: the
Drops flag was missing from the clip screen's Guesses popup): the same Drops flag there draws the guesser's drops.
Recorded: what Show > Drops drew live, kept in clip.txt as `drops` lines (`App::noteDropsInClip` after each
`watchLoot`, written whatever the flag says, as the drop finder looks either way: a line each time the close look, the
spot or its kind changes, or nothing is drawn any more; `core/ClipGuesses` `recordedDrops`); clips saved before say "Not
recorded in this clip." under the flag. Today: what today's drop model finds on the frame shown, the whole frame in
squares like the review's finder (`engine/ClipDrops`: its own thread and GPU device, started the first time the flag
wants Today's; the frame shown handed to it at most 10 a second, a paused one always; about a tenth of a second a frame;
each frame's drops kept, so going back shows them at once; until a frame's own come, those of a frame up to 250 ms
before it), drops on the interface the clip's UI finder found left out as live. So Today shows every drop the model
sees, not only round a spot: a retrained drop model can be judged on old clips. Checked 2026-11-15 in hidden snapshots
on clip 2026-11-15_19-58-40 (frame 1100, after the Imp's death: "Drop 84%" on the yellow ring under the dead target,
the look-alike still to teach) and on a new clip saved from a hidden snapshot (its `drops` line written and read back);
a clip with drops recorded live is still to be looked at. **Review**: a model whose model.json has `"loot_side"`
(core/ModelInfo `parseModelLooksAround`) makes the Finder look at each image in squares (`lootTiles`: 512-px tiles
7 x 4 and 1440-px 3 x 1 on 2560 x 1440, each overlapping the next by a quarter at least), the same drop found in two kept
once, the surest (`keepApart`: IoU 0.5 or 60% inside the smaller). `observex_bench --model models/drop --images FOLDER
--out FILE` does the same, to compare with Python and to find look-alikes. Measured 2026-11-15 (the live model: 64 Shift+Win screenshots, 91 drops, the last 13 held out; trained with the look-alikes the first model found): held-out squares 54/54 drops, 2 false alarms at 50%; held-out whole screens in squares (C++) 13/13 drops, 1 false alarm at 50% (the first model 7), 0 at 70%; on the user's clips 2026-11-15_19-58-40 and 19-59-03 (adena from Imp kills) the adena next to the corpse is found at 76-87% (a far pile of 12 x 7 px too), and rings and runes went down by 22% and 70%. Left to teach: the blue click-to-move marker and the yellow ring under a dead target (both 80-90% when found). Cost (hidden snapshot, game running): 19 looks/s at about 6 ms each; the character recogniser 4.82 ms a look with it, 4.72 without.
**Name plate** (the user asked, 2026-11-15, with screenshots of no hover / hover / selected, for Select Target's
done; stats ABI 6: `PlateLook`, `StatsReading::setLook`): the game draws a character's name between two round orbs
over it while the mouse is on it (grey inside) and while it is the target (blue inside, or red on a monster fighting
the character). The app says which box to look at (`PlateLook`: an id and the box on the screen image;
`UiFinder::setLook`, given to the reader before each `areas()`); the reader adds one colour part over it
(`plateArea`: 320 px either side of the box's middle, from max(180, 0.6 x its height) above its top to a third into
it) and `heuristics/NamePlate`'s `findPlate` finds the orbs there: a bronze ring (red >= 90, red - blue 25..120, red
>= green, green >= 0.6 red, green + 8 >= blue) on 13 of 16 angles at radius 7.5 or 8.5, its inside (9 x 9) coloured
(blue - red >= 40, or red 60 over green and 40 over blue) or grey (channels within 30, not mostly ring-coloured), orbs
inside UI pieces skipped (the bottom right UI has a plate-like pair); two orbs of one kind on a row (2 px), 24 to 520
apart, with white text (all channels >= 200, 12 px at least) between them 12..19 px from the left orb and 10..17 from
the right; of several, the one nearest the box's middle above its top (within its sides +-40, above a third down it).
`GameStats::plate` (`NamePlate`: look id, seen, selected, orb to orb, the name read by Windows OCR while selected,
orbs round it for the why); a why line when none: "Plate: none over x,y-x,y (N orbs round it)". Checked with
`observex_bench --stats IMAGE --plate X0 Y0 X1 Y1` (a `plate` line per image) on the user's three screenshots (none /
hovered / selected, "Imp Elder") and on 41 frames of clips 2026-11-13_00-50-14 and 2026-11-12_23-25-45 (a box under
each plate the Python prototype found, else one over nothing): the same answer on all 41, 23 selected (read "Goblin" /
"Gremlin" on each) and 18 none.
The screen picker (Observing ...) has **None** while a screen is observed (the user asked, 2026-11-12): it stops
everything (capture, recognition, recording, the clip buffer) and shows the choose-a-screen page. Since 2026-11-15 (the
user: a line under the screens with a note beside it was hard to see and clutter) it is a tile left of the miniatures,
as tall as they are, set a little apart: a dark empty screen with a struck-through circle and a "None" name chip like
the screens', violet outline and sign under the mouse; no explanation text (`ScreenPicker::drawNone`; `draw` returns a
`Choice`: a screen or None).
**Ground** (the user asked, 2026-11-12: step 2 towards depth, "a green plane with opacity that rotates with the camera
and reacts to zooms"; `show-ground`, off by default; also in a clip's Guesses popup) draws the ground as the camera sees
it: filled see-through lime (`theme::kGround`) cells 1.5 own heights wide on the ground's own map, fainter farther out,
a ring round the own character's feet and the horizon line; half as strong until the pitch is measured. The camera
(`core/Ground`: it circles a point 0.856 of the own character's height above its feet at the screen's middle, focal 1700
pixels at 2560 wide; pitch, distance, yaw and the feet on the map) is followed by `heuristics/GroundFollower` (from
observex_heuristics.dll: it reloads while the app runs, see the heuristics below), live in
`engine/GroundFinder` (below-normal priority, own GPU device, every image from the pipeline's ground channel: 15 a
second followed, only the quarter and eighth sizes read back, 0.3 MB, and 60 x 60 full-size pixels round the minimap's
cone; on the images between only the cone, see below; runs only while the flag is on) and on a clip in
`engine/ClipGround`. On a clip the flag draws the guesser's grid (the user asked, 2026-11-12: "Recorded option should
ALWAYS show what the player saw in live play"; fixes are tried on Today, made again by the DLL's hot reload):
Recorded's is the grid the app drew live, saved with the clip as clip.txt's `ground` lines (`App::noteGroundInClip`:
a line each time what it drew changed, a new preview image, a new camera from the finder, or nothing drawn any more;
the camera as drawn, written exactly), each frame showing the last line at or before it, so it steps as live did;
clips saved before have none and the Guesses popup says so. Today's is the ground followed again over the video with
today's heuristics (`ClipGround`, once when first asked for and again when the DLL reloads: on the frames of the
images live followed, or 15 a second on older clips). `observex_bench --ground CLIP` prints Today against Recorded
(pitch, yaw and distance off on the frames both drew). Not the same even with unchanged heuristics: Today starts
afresh at the clip's first frame (live had followed for a while) and reads the compressed video. Checked 2026-11-12
in a hidden snapshot on the desktop screen (lines written, saved and read back; no character there, so nothing was
drawn); a clip of the game with Ground on is still to be looked at. Each image: the own character's
box gives the distance (only while the box is as tall as the character would be there, and while the camera looks down
less than 69 degrees: steeper, the box's bottom hardly moves with the distance and is not the feet);
`heuristics/GroundMotion` finds how the ground moved: 60 patches on a grid below the horizon, leaving out the interface
(the UI finder's pieces) and the characters, learned on one image and looked for on the next moved as the ground would
move: first a search on the eighth size, all patches at once, over how far the camera turned, tilted, turned again (a
tilt taken for a turn is never undone otherwise), slid and zoomed (and, while the pitch is unknown, its pitch: a camera
looking steeply down moves the ground otherwise), then each patch a few pixels round there on the quarter size; then
`GroundFit` fits the pitch, distance, turn and slide to the patches found (Levenberg-Marquardt, robust). A camera turn
turns the ground round the feet by up to 0.4 radians and 270 pixels between two images, blurred: a patch merely shifted
(as the snapper finds things) is then no match, which is why the patches are moved through the camera. Where too few
patches match (the fastest turns), the search's own motion moves the camera; a motion neither fitted nor searched stops
the camera (its speeds, which foretell the next search, zeroed); nothing followed for 0.6 s (looking straight down), the
camera is forgotten and found again. Where the camera looks comes from the minimap (`heuristics/MinimapCone`, late
2026-11-12, the user asked: the yaw drifted, and tilts were taken for turns): its light view cone, 92 degrees wide from
the own arrow on the north-up map, turns with the camera whatever its pitch. Its tip is found once in the minimap's
window (a UI finder piece: the window's middle below its 18-pixel top strip, give or take 5 pixels, where the cone's
edges are sharpest), then only round it is read; the heading is half way between the steepest rise and fall of the
grey 10-28 pixels from the tip, 92 degrees apart, every half degree. It matched the ground's yaw to 4 degrees over 260
degrees of turns with no lag at 15 images a second. The search then looks only 0.04 radians either way of the turn the
cone says (so a tilt is never taken for a turn), the fit has a soft prior on that turn, and the yaw is set to the
cone's (all of the way while the camera holds still, a 0.3 share of the difference while it moves). Measured with
`observex_bench --ground CLIP --frames` (`--out` writes frames with the grid and a camera line each; `--heuristics
DLL` follows through that observex_heuristics.dll, the same frame for frame), late 2026-11-12 with the minimap: clip
2026-11-11_22-10-08 (fast turns, 250 degrees each way) 133 fitted and 15 searched of 153, pitch 18-23 degrees
throughout (before the minimap each step's turn matched SIFT on the far ground to 0.01-0.03 radians); clip
2026-11-12_11-34-03 (tilted from level to straight down and back, zoomed) 99 fitted and 49 searched of 152; clip
2026-11-12_15-50-36 (frames 352-426 tilted to nearly level, 641-889 from level to straight down with no turn) 118
fitted and 29 searched of 153: pitch 24 down to 5.5 degrees over 345-433 with the yaw steady to 0.2 degrees, then 8.5
up to 83 degrees by 833 and about 81 after, the yaw following the cone's real 5-degree turn; the cone seen on every
frame of all three. 13-14.6 ms an image on the CPU. F3 adds "Ground N/s, ... fitted N% (searched N%), ... pitch,
distance, minimap N%" (the images the cone was seen on). Late 2026-11-12, on the user's clip 2026-11-12_17-43-14
(fighting wolves, the camera nearly level; "starting at frame 424 is completely fucked until 972"), the user asked to
loop on the grid until the whole recording looks right: above the horizon (0.06 of the screen up), patches on far things
(trees, buildings, the far ground: `GroundMotion`'s far patches) are looked for where the minimap's turn takes them as
if infinitely far, and their middle rise is the tilt (divided by 1 - distance / 50, as the camera circles the feet),
which the search starts from, the fit is held to (0.02 radians), and a blurred search (matching under 0.6) takes;
without them a blurred search tilts none. Looking nearly level a tilt moves the ground's patches as no flat ground does,
so before them tilts were missed or invented. The search also tries standing still, and the patches are looked for 6
pixels round where 3 found too few. The own character's box's top counts a third of its bottom (looking down 20 to 35
degrees it is not the head's top as modelled: it held a 24-degree tilt to 15), and a box twice or half as tall as the
last one taken is another thing's (the tracker gave the own id to a wolf, and the camera went 33 away). Measured against
the far band's shift between every two video frames (a scratch probe, its depth corrected the same way, anchored where
the sky shows the level): pitch off 0.7 degrees on average and 1.9 at most from 0.6 to 7 s (before: 2.5 and 6.2; 21.6
and 56 before the far patches), and 7 to 9 s 25 to 37 degrees down as the band says once the steps it lost in the
fastest tilts are added back. Fitted and searched of about 150 then: clip 2026-11-11_22-10-08 137 and 10,
2026-11-12_11-34-03 112 and 37, 2026-11-12_15-50-36 130 and 22, 2026-11-12_17-43-14 114 and 24 (any small change moves a
count by 2). Still off: that clip's first 0.6 s (the camera tilting up 25 degrees from about 34, the fit starting at
26). Not done yet: hills and stairs (the ground is one plane), a minimap set to turn with the camera (its cone then
always points up and the heading is wrong; the user's is north-up), and a truth for the other clips (fitted is not
right).

Between follows the grid turns as the screen does (late 2026-11-12, the user on clip 2026-11-12_18-58-01: "from 561 i
start rotating, and until 570 the grid completely doesnt react ... when i stop rotating at frame 595 the grid keeps
moving until 604 where it snaps"; then "smooth it"). Followed only 15 times a second, the grid drawn (`groundAt` of the
newest camera) stood still until the next follow after a turn began, then went on along the follow's turning speed
after it stopped until the next follow snapped it back: 7 to 14 degrees off. Now the pipeline hands the ground finder
every image; one due (1/15 s after the last, a 2 ms slack) is followed as before, and on the others only the eighth
size (58 kB) and the minimap's cone are read (`Grounding::headAreas`, `headFirstLevel` and `head`, ABI 6):
`GroundMotion::findTurn` looks for the patches learned on the image followed with the camera turned only (tilted on
along its speed, not slid: the foretold slide moved a still camera), from stopped where the image before was to going
on as it went, 0.05 radians beyond either. The cone (redrawn by the game only about 30 times a second: steps of 4 to
14 degrees in a fast turn) keeps it honest: while it holds still (no change of 1 degree in 60 ms) the ground may turn
the camera at most 0.03 radians from where it was when the cone stopped (looking level or up, the patches turned it 4
to 14 degrees the cone did not: clip 2026-11-12_11-34-03); while it turns, at most that plus 50 ms of the cone's speed
from where it looks. The cone's turn since the image followed counts from its heading there only if it held still
then, else from the yaw followed (in a turn the cone there was a redraw behind: 11 degrees on 2026-11-11_22-10-08, and
held to it a stopped turn went on 14). Where the patches do not match (blur), where the cone looks; where neither
says, as before. A follow searches from going on as the images between turned (else the cone), one that neither fits
nor searches keeps that turn (`GroundFit::turned`; it went back to the last follow's yaw, up to 17 degrees), and its
pull to the cone goes only beyond how far the cone may lag (50 ms of its speed). `GroundSeen::followed` (the newest
image followed whole) is what the clip's `ground` lines name, so Today follows the same images and turns on the frames
between (`ClipGround`); the bench's `--frames` adds a `camera` line per frame. Each image between costs about 0.9 ms
(bench, read-back included); F3 adds "turned by it N/s (ms)". `MinimapCone`'s lock now ends after 450 ms unseen
(`kMissMs`), not 8 reads. Against the far trees' shift between every two frames (a scratch probe, scaled to the grid's
total): 18-58-01 frames 553-612 off 0.38 degrees on average and 1.4 at most (the cone alone: 1.0 and 5.1; live before:
3.2 and 13.4); 22-10-08 frames 44-75 0.45 and 2.2 (the first try with the screen: 2.6 and 13.8). The grid's jerk (the
change of its step between frames) averages 0.17 to 0.49 degrees on four clips (the cone alone 0.66 to 1.46); on
11-34-03 (level and up) 0.32 against the cone's 0.16; on 2026-11-12_17-43-14's 370-degrees-a-second turns, blurred
so the patches do not match, it still steps 9 to 14 degrees at the cone's redraws (0.42, the cone alone 0.43). The
follows' fitted and searched counts are within a few of before on the six clips.

The pitch by the depth network (late 2026-11-12, the user: "the depth will have to run all the time anyway for other
systems, so make it run all the time and make the grid position according to it. precision of the grid has top
priority"). The ground's motion says how the camera turns and tilts, but where it looks (the pitch) only in turns, and
it was often far off: 35 degrees with the horizon in the screen's middle (clip 2026-11-12_20-16-13, 0.8 s), 16
looking down past the screen's top (9 s; that needs more than 23), +3 with the horizon 2.7 below the middle (2 s), 31
where far trees stood a third down the screen (2026-11-12_20-09-30, 8.5 s). The depth finder now runs whenever the
flag Ground is on, the flag Depth on or off (see Depth; that flag only draws it), and the ground finder hands the follower each new map before
its next image (`Grounding::depth`, ABI 7: `DepthInput` and `GroundDepthNote` cross it). `heuristics/DepthTilt`
takes each map row's median in a band (rows 640 to 1080 of 1440, columns 700 to 1900; the interface and the
characters left out, 2 map pixels wider) and fits a line to them: flat ground's inverse depth falls on a line up the
screen that reaches 0 at the horizon, so where it reaches 0 says a pitch ("seen"). The network's depth is not quite
inverse depth: seen is the pitch up to 15 degrees, then means ever more of it (20 means 22, 24 33, 30 56, 38 and up
81.5: a table measured against the grid on five clips), and a map counts less where the table is steep (its weight
1/slope squared, at least 0.25). A map whose rows are off their line by more than 2% of their range says nothing (the
horizon in the band looking level, a hill, a wall). The follower keeps the camera's pitch on its last 48 images,
compares the map's pitch with the one on the map's image, and tilts the camera, the patches learned and the images
kept towards it smoothly on every image after (`pullDepth`: a time constant of 100 ms over the weight, 95% of the way
in 0.3 s where it counts whole, 1.2 s looking steeply down; a step a map moved the grid in steps); the first map after
the camera is found sets the pitch at once. Within a second of a map that weighed at least 0.4 (25 degrees or less) a
follow's fit holds the pitch at the first image to the one it has (0.005 radians, `GroundFit::Start::pitchHeld`): left
free, the own box and the matches put it back at every follow (up to 3 degrees off the map's) and the grid went back
and forth; held, a turn's motion still takes it about a quarter of the way. Steeper the fit is free: held, the grid
lagged fast tilts to straight down by 12 to 20 degrees. A clip's save keeps every map (not only those drawn), and its
Today gives them to the follower on the frame after their image, as live (`ClipGround`); the bench prints "depth maps
given N, of them N said the pitch" and with `--frames` a `depth` line per map (seen, pitch, the camera's then, how far
off). Against the horizon by eye (with depth, without): 20-16-13 at 2 s -2.7 (-3.4, +3.2), at 9 s more than 23
(26.3, 16.3); 2026-11-12_11-34-03 at 7.1 s +1.4 (0.2, -5.0), at 9.3 s about 16.8 by a building's rise (16.3, 20.7);
20-09-30 at 8.5 s the map's 11.7 (13.9, 31.1). Over 9 clips the frames whose pitch step changed more than a degree
went 489 to 433 (fewer on 7), and the grid sits 0.2 to 1.1 degrees from the maps' pitch (median a clip); fitted up to 8
fewer (held, more matches miss where the motion disagrees). F3's Ground adds "depth N/s said the pitch N% (N off, N ms)".
Not done yet: looking level the horizon is in the band and the maps say nothing (the motion carries the pitch then),
and the table's steep part (seen 24 and up) is measured against the motion only.

The pitch by the camera's turns (2026-11-15, the user: "the most PRECISE way to figure out the pitch given just the
screen"; `heuristics/TurnPitch`, unit tested, ABI 8; 9 for its tilt in the note). Lineage 2's camera never rolls and turns round the world's up,
which the camera sees leaning by its pitch: looking level a turn slides the picture sideways, looking down it also
spins it round its middle, and how much of each says the pitch, whatever the ground's shape (no plane, no depth). Each
follow `TurnPitch` learns up to 32 x 18 points (the strongest Shi-Tomasi corner a cell of the quarter size, which the
ground finder reads whole anyway; the interface and the characters left out), follows them onto the next follow's
image (pyramidal Lucas-Kanade on the quarter size and 3 halvings, 9 x 9 windows, started where the follower's camera
motion takes them: on the flat ground round the feet below the horizon, as if infinitely far above; kept only if
followed back to within 0.3 pixels), and fits the two cameras to them: the first's pitch, the turn, the tilt and
which way the camera went (a free direction, so it works while walking too), by each point's Sampson distance off its
epipolar line, Cauchy-robust Levenberg-Marquardt from 9 starts (the pitch given and 11 degrees either way; going
sideways, up or ahead), and how sure the pitch is (its spread, from the fit's covariance). A still camera says
nothing of the pitch (spread huge) and a tilt only how far it tilted. `TurnHold` (a one-number Kalman filter) takes a
pair that turned at least 0.6 degrees and is sure to 0.5 as a measurement, every tilt (the pair's, or the follower's
when the points were lost) as the process; while it is sure to 1 degree it holds the pitch: the follower tilts the
camera, the patches and the images kept to it (`tiltAll`), the fit's pitch is held (`pitchHeld`), the depth maps do
not pull it (their note still says how far off they are), the camera says `GroundCamera::pitchByTurns` (geoCamera
then adds no slope correction: the turns' pitch has no slope in it), and a follow whose points fitted is not lost
(the patches matched nothing for 0.6 s of a fast turn on 2026-11-15_14-24-16 at 5.1-5.7 s while the points did).
Gates: holding, a pair more than 5 spreads off is not taken, two in a row that agree with each other are (the hold
was lost in a blur); not holding, one more than 20 degrees off the follower's is a wrong fit (the fit finds wrong
cameras near +-85 degrees in fast tilts with little turn: 85.7 for 17 on 18-58-01's first follow). A follow whose points
were lost or blurred (fewer than 100 agreeing, `kLeastAgreeing`) adds 1.15 degrees of doubt and a third of the tilt
the follower made meanwhile (`kLostSpread`), so one such follow ends the hold and the depth maps pull again (the user,
2026-11-15 evening, clip 2026-11-15_21-19-16: "the ground flag looks very solid but only when im rotating the camera
on pitch very gently. the moment i do hard fast turn it completely breaks": the camera tilted 40 degrees in 70 ms
with the points lost, and the hold, then adding only 0.46 degrees a lost follow, kept 45.8 while the camera looked at
about 4; now it lets go and ends at 7.4, held 41% of follows; the user tilts inside fast blurred turns too: 2 degrees
unseen on 20-16-13 at 3.6 s). The screen alone cannot follow such flicks (the next step: the mouse, below). Measured (2026-11-15, against the Python prototype's half-size 4-frame pairs at 120 fps,
session 3307e15c's scratchpad pitch/: tracks.py, fit.py, cmpbench.py): 2026-11-12_18-58-01 held 99% of follows, off
the prototype 0.12 degrees on average (sd 0.19, median 0.003, between follows included), where the live grid as
recorded was 1.14 off (sd 1.26, most 4.7); 2026-11-15_14-24-16 held 80%, 0.33 off (median 0.02; only before its first
turn more); 2026-11-12_20-16-13 held 74%, median 0.02, but there the user tilts 3 degrees within 100 ms of a fast turn
and the frames between follows (head() turns only) lag up to 2.3 degrees; its first 0.3 s tilt 40 degrees with no
turn (the follower's pitch then, as before). 4.3 ms a follow on the ground finder's thread (15 a second). On rendered
ground (the tests) a 3-degree turn says the pitch to 0.06 degrees at 17 looking down, 0.13 at 46 (the spin a turn
adds shrinks as its cosine), 1.5 degrees on a screen half as wide only to 0.2. F3's Ground adds "turns held the pitch
N% (said it N%, N ms)"; the bench's `--ground` prints "turns: fitted on N frames, said the pitch on N, held it on N"
and with `--frames` per follow `turns POINTS AGREEING f/t/H turned DEG says DEG +- SPREAD miss PX MS`. The lens is
still kGroundFocal 1700 (the 264-degree turn of 14-24-16 closed against the minimap's cone says 1688 +- 9; a 1% lens
error is 0.1 degrees of pitch at 10, 0.25 at 30) and the screen's middle the principal point (10 pixels off is 0.34
degrees). Not done: the camera turns in steps of about 0.2 degrees (the same for yaw and pitch, user.ini's speeds both
3600), so tilts could be snapped to whole steps between turns; the lens could be fitted from a full turn.

The mouse (the user, 2026-11-15 evening: "this pitch task is failed until it is very precise and stable in very fast
camera rotations ... maybe the key is to listen for global mouse movement when RMB is being held? camera rotates when i
hold RMB ... figure out the proper math as to how much the camera is moving ingame based on my mouse stride delta * the
mouse sensitivity"). `engine/MouseWatch` reads every report of the mouse wherever it is used (Raw Input,
RIDEV_INPUTSINK, on a message-only window and thread of its own; QPC-stamped on arrival; never moves it), and while a
screen is observed (`App::mouseWatch_`, made with the clip buffer) hands each to the clip buffer
(`ClipRecorder::moused`), so a clip's clip.txt has `mouse TIME DX DY HELD BUTTONS WHEEL CURSORX CURSORY` lines (raw
counts, before Windows' pointer speed and acceleration; HELD 1 left, 2 right, 4 middle; from as long before the first
frame as the looks; `core/ClipLines` appendMouse/readMouse, `ClipText::mice`) and its head Windows' pointer settings
(`MouseWatch::pointerSettings`; this machine: speed 10 of 20, enhance pointer precision off; the game's Option.ini has
AutoTrackingPawn=False, so only the mouse turns the camera; its User.ini and L2.ini are encrypted, Lineage2Ver413).
`observex_bench --mouse [SECONDS]` watches it from the console and prints a line a second (reports, counts, counts with
the right button held, the cursor). The bench's `--ground --frames` follow lines end in `tilted DEG`: the tilt the
points saw between two follows (`GroundNote::turnTilt`, ABI 9), with their time to 0.1 ms. The research (2026-11-15,
UT2004's UnrealScript as the nearest source; nothing public on Lineage 2's camera): UE2 turns the view by 32 x DeltaTime
x counts x MouseSensitivity x FOV/90, the rotator in whole units of 65536 a turn. Measured on the user's 7 clips of that
night (2026-11-15_21-42-40 to 21-46-09: a 360, tilts to the limits, 180s at the top, flicks, the camera into the ground;
session 3307e15c's scratchpad pitch/): **a count turns or tilts the camera 36 units (0.19775 degrees), both ways, at any
speed** (the 360 of 21-42-40 lined up with its first image after 361.69 degrees of counts, by the images alone; the cone
said 35.98 units a count between rests; the points step in whole counts). The 37-unit idea was the lens: **1741 pixels,
not 1700** (the points of 1291 turning frames 1739.8, of 949 tilting ones 1743.9; `kGroundFocal` is 1741 since).
**Counts turn the camera only in the game's camera mode**: the right button held and the cursor put in the window's
middle by the game, seen on the first report after (7 ms to 0.45 s after the press; 5 presses of 16 it never did, and
their counts turned nothing). That report may already move it on: on clip 2026-11-15_23-34-30 the first was at (1279,
723) and the cursor never sat on the middle again, so the exact-middle gate lost a whole 9 s press (the cone turned 197
-> 21 -> 288 degrees, the card stayed at 222, cone off up to 159); since that night near the middle counts too after a
jump there (within `kWarpSlack` 2 plus `kWarpPixelsPerCount` 4 a count, the jump farther than that: a cursor dragged
across the middle does not jump). Over the 34 presses of that night's clips it took 5 more, all after the 7 measuring
clips (whose gating is the same), and on those 4 clips the cone stayed within 1 degree of the counts. So gated, the cone
stayed within 0.8 degrees (its own error) of the counts over up to 3060 degrees turned. **The pitch stops at 82.0
degrees down** (TurnPitch while yawing there, clip 21-44-29: 82.015, 0.2 either way over 120 frames); no lower limit
down to the lowest the clips went, 28.5 up. A frame shows the reports up to about 13 ms before its capture, each spread
over the 12 ms before it (82% of frames within a count of the points). The cone lags about 25 ms and redraws at ~30 Hz.

**`core/MouseCamera`** (built that night): the camera by the counts: the camera-mode gate, whole units (no float sums:
100000 random reports and back come to 0 exactly), the pitch clamped at the limit as the game does, `at(ms)` for a frame
(the lag and spread). Anchors (`anchorMouse`, from the follower's note on each follow): the cone's heading while the
camera rested 120 ms (the yaw is the middle of the last 9 rests' cones; a rest 2 degrees off forgets the ones before:
turned by something else); the turns' pitch where it turned at least 0.6 degrees with 100 points agreeing within 0.5
(the middle of the first 3, then of 5; past a limit by half a degree it is wrong, the first pair of 21-46-09 said 88);
the follower's own measured pitch only as a guess before them. Pushed past the top limit by more than the pitch may be
off (30 counts after a guess, 5 after the turns), the pitch is pinned there: exact from then on, the turns only checking
it (re-anchored if 9 say 2 degrees off; TurnPitch is up to a degree off the counts between the limits, high below 0 and
low at 30-70). `SharedMouseCamera`: the same under a mutex. Live: `App::mouseCamera_` (made with the observing, the
screen's middle as the game window's) gets every report from the mouse thread; the ground finder gives the follower the
counts' camera on each image (`GroundHints`' mouse fields, ABI 10: `GroundFollower` takes their turn as the search's,
holds the fit's tilt and pitch to them, and turns and tilts the camera so on follows and between them; no cone,
turn-hold or depth pulls while they are known) and anchors it from the note; the preview's grid takes the counts' yaw
and pitch on the very frame shown; Stats has a card Camera first (Yaw: minimap / turned only, Pitch: top limit / turns /
guess, Off by: the cone and the turns lately). Its anchors all come through the ground finder, so it follows while the
Stats tab shows (not only with Ground or Geodata on; the grid is drawn only with Ground): with both flags off the first
live try (2026-11-15 night, clip 23-08-49) said Pitch "Not read yet" and Yaw "turned only" throughout, the counts alone
knowing only how far it turned. A follow costs about 16 ms on its thread, 15 a second, the cone 1.6 ms between; the
depth finder still runs only with its flags (no depth map: the follower's guess, then the turns, as the bench found on
that clip: 20.1 degrees guessed, 21.4 from the turns once the camera turned). On a clip the re-follow
(`engine/ClipGround`, Today) does the same from its mouse lines (`bench --ground --no-mouse` leaves them out).
**The camera in clips** (the user asked, 2026-11-15 night: "last 10s is not saving camera"): clip.txt has a `camera`
line (`core/ClipLines` appendCamera/readCamera, `CameraShown`: the card in degrees, `core/MouseCamera`'s `cameraOf`)
whenever the card would show something else, whatever the tab shows, so every frame it turns (`App::noteCameraInClip`
beside the brain's, timed by the image the window showed). On a clip the Stats tile's Camera card shows Recorded's
(`ClipText::cameras`, `recordedCamera`: the last line at or before the frame) or Today's (`ClipGround::follow`'s
`mouseCameras`: the card on each frame after its own follow anchored the clip's mouse camera, as live; the first ask
starts the ground's re-follow, shared with the Ground flag), or a card saying why not (saved before camera lines; before
mouse lines; still working; not known by the frame). `bench --ground` prints Today's card against Recorded's every half
second.
`observex_bench --turns CLIP [--focal PX] [--lag MS] [--spread MS] [--top UNITS] [--bottom UNITS] [--anchor]`: each
frame pair's points against the counts. Checked on the 7 clips, each frame's camera turn and tilt against the points':
yaw 0.73 -> 0.09 degrees mean off (95% 3.69 -> 0.42), pitch 0.42 -> 0.07 (1.90 -> 0.31); in fast frames 3.25 -> 0.33 and
1.31 -> 0.18. The absolute pitch against the turns': within 0.12 median in 5 clips, 1.0 and 1.4 in the two that never or
late reached the top (the turns' own bias there). Open: the lower limit (a clip pushing the camera all the way up into
the ground), the per-frame spread (18% of frames a count off).

**Depth** (the user asked, 2026-11-12: step 2 of the depth plan, "move it to C++, into observex, add flag in the live
view as well as in guesses", the last 10 s recording exactly the depth seen, hot reload, the interface left out, colours
vivid enough that a distant monster shows against a darker background; `show-depth`, off by default) draws, instead of
the screen, how near each pixel is (nearer brighter), with the interface, the ground and the characters over it as
over the screen. The network is Depth Anything V2 Small (`models/depth/model.onnx` from `tools/depth`, fp16 weights,
the screen squeezed to 924 x 518, relative inverse depth out), run by TensorRT (`engine/TensorRt`, shared with the
detector; its engine cached as `models/depth/model.engine`, built about 80 s the first time) in `engine/DepthNet`, 20
images a second (`DepthFinder::kImagesPerSecond`) from the pipeline's depth channel in `engine/DepthFinder`
(below-normal priority, own GPU device, the CUDA stream of the lowest priority; made the first time the flag Depth or
Ground is on while observing, fed only while one of them is on and no review or clip is open, its maps shaded only
while Depth is on: the grid's pitch comes from its maps, see Ground. For a few hours on 2026-11-12 it ran always, flag
on or off; later the same night the user asked that the grid and the depth work only with their flags on). Its
map against the Python model's on the same frame: correlation 0.99956, 0.45% of the range off on average. The map is
shaded by today's heuristics (`core/Depth`'s `Depthing`, made by observex_heuristics.dll's own `observexDepth` export,
ABI 1, apart from the main ABI; the finder makes a new one when the DLL reloads, so a shading change shows without
closing the app) as `heuristics/DepthRules`: (1) the interface's pieces the UI finder found, 2 map pixels wider, are
holes filled from the world round them the way a texture's pixels past its edges are (the user's idea, 2026-11-12;
push-pull: averaged into halvings until no hole is left, then pushed back bilinearly, each halving but the full size
relaxed 12 over-relaxed sweeps, 1.8 times each step, towards the membrane its edge spans, so a slope goes on evenly
under a window: pushed alone a slope came out too far at a hole's top and too near at its bottom; with the trend down
the screen, each row's mean over the world's pixels, taken out first and put back after, so the ground's slope goes on
under a piece reaching the screen's edge too: held flat there it left a ridge up to 24 levels along the hotbar's edge,
and a slope rule at the screen's edge instead ran off under the game's bar across the whole bottom, where no row has
any of the world) and left out of the measures; (2) the rank of each pixel's
depth among the world's (contrast-limited histogram equalisation, bins capped at 3 times the mean, 10% straight): by
value everything past a few metres fell into the darkest levels; (3) 30% of the level from how much nearer a pixel is
than those round it (its log depth against their mean over 12 map pixels each way, softly limited at 3%): on clip
2026-11-12_17-43-14 frame 79 rank alone left a wolf 30 metres off 1 to 3 levels over the ground at its feet, with it
the three far wolves show orange in a darker halo, name plates too; (4) inferno's colours. Shading keeps nothing between
maps, so the same map always shades the same. The last 10 s keep it exactly as seen: each map goes into the clip buffer
packed without loss (`engine/DepthStore`: the network's fp16 map and the 8-bit levels shown, each pixel as its
difference from its left neighbour in byte planes, then Windows' XPRESS-Huffman; with the palette and the interface
left out), and the UI says which map it drew over which preview image whenever that changes (`App::noteDepthInClip`,
clip.txt's `depth IMAGE TIME MAP` lines; MAP 0: none any more); a save writes every map into `depth.bin` (the
clip's Today tilts the grid by them; the shows draw some). On a
clip the Guesses popup's Depth draws the guesser's: Recorded the levels and colours drawn then, Today the same map
shaded again by today's heuristics (again when the DLL reloads; where the app drew none live there is none);
`depth.bin` is read the first time the flag asks. Measured on clip 2026-11-12_17-43-14 (`observex_bench --depth`,
every frame, 11 interface pieces): network 3.7-4.9 ms (squeeze, network, map back, waited for; 7.5 ms live beside the
app's own models), shading 9.5 ms on the CPU (12.5 at most; 4.8 with no interface), packing 7.7 ms, unpacking 3.4;
about 490 kB a map with interface on the screen (620 on the desktop), so about 100-125 MB per 10 s in memory and in
depth.bin; packing loses nothing, and all 1212 maps written, read back and shaded again matched the levels shown.
Frame to frame: where the picture stood still, 1.4% of the pixels moved more than 8 levels (0.95 on average; rank
alone 0.4% and 0.45: the local term costs some steadiness for the far things), all pixels 3.1% and 1.73 (every 6th
frame: 3.9% and 1.82 still, 7.5% and 3.54 all; the slope rule at the screen's edge that ran off: 8.5% and 10.8 still).
`observex_bench --find-depth` runs it live (with `--clip` the saved clip has its depth: 202 lines and depth.bin from
a 12 s run on the desktop screen, 2026-11-12), and `--depth CLIP --out FOLDER` makes FOLDER a copy of a clip with the
depth drawn on it (each map from its own frame on), to see it on clips saved before. F3 adds "Depth N/s, network ms,
shading, packing, kB a map, changed". Not done yet: the live run measured on the game (it was covered by another
window), Today's shading runs on the UI thread when the drawn map changes (about 10 ms, 20 times a second while
playing), and the model is not tuned to Lineage 2 (far trees and the sky are good, flat UI-like things such as name
plates come out near).

**Geodata** (the user asked, 2026-11-15: "add Geodata flag in the live view", "if the system is not sure where i am
it writes in the middle of the screen type /loc and it keeps watching the chat window ingame to read it and sync",
"the flag draws the whole geodata as grid that matches the world"; `show-geodata`, off by default). A Show flag after
Ground: the server's geodata (L2J `.l2j` region files in `models/geodata/`, named `RX_RY.l2j`, rx = floor(x/32768)+20,
ry = floor(y/32768)+18; the whole u3j-lineage-geodata set from GitHub (ChaosPaladin, 2015), 203 regions, rx 11-26,
ry 10-26, 1 GB, every main town's region there, downloaded 2026-11-15 with the user's OK, each checked to walk its
blocks to its exact end; it matches the C1 server: 47 units = one own height; only the 9 regions nearest the one read
last stay loaded, `Geodata::kKept`) drawn as a grid on the terrain round the character,
fixed to the world (it never slides as they walk): fine lines every 32 units out to 1216, coarse every 96 out to
4032 (fading), coloured by height against the feet (level lime, lower cyan, higher orange at 120 units), the fine
cells a character cannot walk into shaded red (tree trunks, rocks), a ring round the feet; the game's interface is
left clear (lines cut where they go under a found window, Liang-Barsky; cells touching one left out). Pieces:
- **Where the character is** (`core/WorldPosition`, unit tested): the last /loc answer plus how far the minimap's
  map slid since (north-up, so world = -scale x slide), the scale (game units a pixel) learned from two /loc answers
  with a walk of at least 150 units and a slide of at least 40 px in about the same direction, kept in observex.cfg
  as `minimap-scale` once learned. Its height is set each image from the geodata's floor nearest the one it had (a
  bridge, a cave). `Doubt` says why it is not sure: NoLocation, Moved (walked before a scale is known), MinimapLost,
  MinimapHidden.
- **The minimap's slide** (`heuristics/MinimapScroll`, in the stats DLL, ABI 7, run by `StatsReader` at 10 a second on
  the Minimap window the UI finder finds): edge key points on the half-size grey of the map round the own arrow (the
  arrow, cone and ring left out), matched by correlation (each gradient capped at twice the key's, so the map's
  see-through buttons and bright overlays don't spoil it) within 3 half pixels of the foretold slide (14 after a
  miss), a new key after 20 half pixels; 6 misses in a row or the window moving more than 16 px start a new run (the
  position is then doubted). No new key on a weaker match (that drifted 6 px).
- **The /loc answer** (`heuristics/LocationReader`, same DLL): the Messages window's lowest lines; a line is new when
  its letter pixels share less than 70% with every line of the image before (the chat is see-through, so hashes
  changed every frame; this check costs 1.5 ms where reading every line cost 18), and a new line among the newest 3
  is read (the stats' digit OCR at 3x) for "Current location : X, Y, Z (...)". An answer whose Z is more than 100
  units from the geodata's floor there is logged and not taken (read wrong).
- **The grid** (`core/Geodata`, unit tested): `Geodata` reads regions on demand (flat, complex and multilayer blocks
  of 8x8 cells of 16 units); `GeoField` builds 320 cells each way round the character on a thread of its own (10-40
  ms, again when they come within 4200 units of its edge), taking in a multi-floor cell the floor nearest the ground
  round it and cutting down tall narrow bumps (the geodata keeps the tops of trunks: that held the ground 2,000 units
  up) as blocked; `geoCamera` sets the ground finder's pitch right on the geodata: the follower's pitch is off by the
  ground's slope along the view (it fits a flat plane), so a least-squares plane is fitted to the field under the
  rays and pitch = follower's - slope along the view - 1.95 deg (calibrated on clip 2026-11-15_14-24-16: -4..-6 deg
  facing the hill, +3 facing the plain); not while the camera's turns hold the pitch (`GroundCamera::pitchByTurns`,
  see Ground: that pitch has no slope in it); `geoGrid` projects it.
- **What to do** (`app/GeodataView`): while not sure for 0.7 s, in the middle a third down, "Type /loc" (with why:
  not known yet / once more for the minimap's scale / the minimap lost track) or "Open the minimap"; "No geodata
  here" where the region file is missing. Drawn whenever the flag is on, also before the ground finder has a camera
  (then the message alone). The Log tile gets each /loc taken or refused and each loss of sureness. Where it was
  last sure, the grid is drawn fainter until it is sure again.
- **The UI finder names the minimap window** (`core/UiLayout`): a framed window within 8 px of a placed piece takes
  its name (the Minimap's anchor hit inside its frame, and the window, not the piece, was found otherwise).
`observex_bench --geodata CLIP --out FOLDER` follows a clip the same way (see Build and test). Measured 2026-11-15 on
clip 14-24-16: the /loc read right (-9543, 25573, -3616), the minimap followed on 99 of 99 images, sure on 87 (until
the /loc); the grid on the terrain, red at the tree bases, the hill's line after the pitch fix, the UI clear; four
other clips one run each, at most 5 single misses. Checked in a hidden snapshot: "Type /loc" in the middle with the
flag on. Not done yet: the minimap's scale unchecked on a walk (no clip with two /locs and a walk between), zooming
or moving the minimap ends a run (a new /loc), clips don't replay it (Guesses has no Geodata), the pitch correction
comes from one clip, and only the village's regions have been seen against the game.

**Tabs and tiles** (the user asked, 2026-11-12): under the top bar a strip of tabs **Live**, **State** and **Stats**
(`app/Widgets` `drawTab`: medium weight, muted while off, an accent underline while on). Each tab shows or hides its
tile, a docked pane (`show-live`, on by default, `show-state` and `show-stats` in observex.cfg); the last tile shown
stays. No tooltips on the tabs (the user asked, 2026-11-12: "too much clutter"). `core/Tiles` places them: one shown takes the whole view; Live with others: Live on the
left, 3/5 of the width, the others in a column on the right, 2/5 (the user asked, 2026-11-13; it was 1/4), Stats above State, half each; without Live, Stats above
State across the width. The 6 px gap between tiles holds a 1 px line that drags (kept between 15% and 85%, not saved).
Each tile is a child window with the usual 16 px padding. Live shows what the whole view showed before (the screens to
choose from, the observed screen, a review or a clip; opening a review or a clip turns Live on), errors stacked at its
top right. The State tile is drawn first, and the view before the top bar: while it has the keyboard every key held is
locked for the frame (`ImGui::SetKeyOwner` with `LockThisFrame`), so the review's keys, the clip screen's and the bar's
clip steps do not act too. Unit tested (`core/Tiles`). Checked in hidden snapshots 2026-11-12 night: every layout, both
lines dragged, the user's game observed with its numbers in Stats.

**Log** (the user asked, 2026-11-13: "next to Live, Stats, State, add Log as last element ... a very minimalistic logs
window, with time of every message ... save it to file ... make sure last 10s saves it too. app restart should bring up
the logs, but make it a ring buffer"; then: no checks with the image and no ms added to the frame, only differences in
what Stats/State read or the app's own events). The last tab, **Log** (`show-log`, off by default), shows a narrower
column at the right, the whole height, the others left of it (the user asked, 2026-11-13: with all four open, "a
narrower column to the right of stats and state"; `core/Tiles` `placeColumn`: 1/5 of the width at first, its line drags
like the others; with Live only it sits right of Live; alone it takes the view). `app/LogView` draws a line each: the
time (HH:MM:SS, muted), then the text, wrapped under itself right of the times when too long for the column; a muted
date row ("Friday, 13 November 2026") where the day changes; it stays with the newest line while scrolled to the bottom.
Rows have their own heights (cached `tops_`: added lines measured alone, all again only when the column's width
changes; only the rows in sight drawn, so 10,000 lines cost nothing). A **Clear** button at the tile's top left (the
user asked, 2026-11-13: "a Clear button which clears current log"; the lines scroll in a child under it) empties the
log and observex.log (`LogBook::clear`: the file written again with only its header); dimmed with nothing to clear or
over a clip's lines; the clip buffer keeps the lines it already had (a clip saved soon after still shows them). `core/Log`'s `LogBook` (UI thread) keeps the lines in
`observex.log` next to observex.exe (in snapshot mode next to the PNG): each line `2026-11-13 00:06:01.234  TEXT`
(local time to the millisecond, two spaces), appended and flushed as it is made, read back at start (so a restart
shows the earlier runs' lines). A ring buffer: at 11,000 lines (`kTrimAt`) the oldest go down to 10,000 (`kKeep`) and
the file is written again (to a .tmp, then renamed over). `App::addLog` also hands each line to the clip buffer
(`ClipRecorder::logged`), so a saved clip's clip.txt has its `log TIME WHEN  TEXT` lines; on an open clip the Log tile
shows those, the ones made after the frame shown dimmed (`ClipView::logLinesBy`: made before the next frame; on the
last frame all, the clip's own save line too); a clip saved before says "Not recorded in this clip". What it logs
(nothing looks at an image; the XP comes from the numbers the UI finder already reads): observex started / closed;
"Observing Odyssey G61SH" (the monitor's name) and "Stopped observing ..." (also around a display mode change's restart), "Could not observe
...: why"; "Recording started: every 3 s, into FOLDER" (or only unsure), "Recording stopped / done / stopped by an
error: N images in FOLDER"; "Saved last 10 s as clip FOLDER" (logged just before the save, so the clip holds it), a
save that fails; XP (`core/XpWatch`, on each new stats reading in `App::keepStats`): the Status XP gauge's number
("26.64%"), taken once read on two readings in a row (a misread digit that still agrees with the bar shows on one),
timed by the first; "Got 0.12% XP: 26.64% to 26.76%", "Lost ..." and, when it falls by more than 50 points, "Level up,
got 0.32% XP: 99.80% to 0.12%"; the first number, and one after 30 s with none (`kForgetSeconds`: the character select
screen, another character) or after observing stops, only starts it. "Target Changed: Grey Wolf" / "Target Changed:
None" (asked through the Stats session, 2026-11-13): `GameStats::target` (changed by the stats reader once a new target
held on two readings) against the one `App::keepStats` logged last, since the UI may skip readings; None again when
observing stops. Checked 2026-11-13 in hidden snapshots on the
user's game (killing wolves and goblins): "Got 1.08% XP: 24.09% to 25.17%" matched the chat's 88 XP a kill (and the
next kill's 121 XP to 26.65%, the same rate); a saved clip's clip-stats.txt read 26.65% on every reading of its 10 s
(no misread); the clip's log line came back on the clip screen, dimmed mid-clip and lit on the last frame; restarts
brought the lines back. Unit tested (`tests/LogTests.cpp`: the line format, the file read back, the ring in memory and
in the file, the XP rules, a clip's log lines).

**Crash log** (the user asked, 2026-11-15 night, after observex crashed in TensorRT's nvinfer_11.dll about a second
after "Observing" and Windows' own report could not be read: the app runs as administrator, so its reports are
admin-only, and there is no debugger here): `engine/CrashLog`'s `installCrashLog` (from `App::init` right after the log
is made, and again at the end of `startObserving`, in case CUDA or TensorRT put their own handler in) appends a crash on
any thread to observex.log in its line format, so the Log tab shows it after a restart: `CRASH: access violation reading
0x... at nvinfer_11.dll+0x31acca8 on thread observex depth finder (1234)` (the threads are named), then the call chain,
one line a frame (`#0 module+offset function file:line`; names and lines from the PDBs next to the modules, "near" an
export where a module has none, such as Windows' and TensorRT's, a guess), then `dump: FILE`, a minidump in `crashes/`
next to the log (observex-YYYY-MM-DD_HH-MM-SS.dmp, the newest 5 kept; to open in Visual Studio with the PDBs). An
uncaught C++ exception (std::terminate, abort) is told the same way with its message. Windows then goes on with the
crash as before. **Release keeps PDBs** since (observex.pdb and the DLLs' in the project folder; /Zi, /DEBUG /OPT:REF
/OPT:ICF, so it optimises as before; gitignored with crashes/). `observex_bench --crash FOLDER [uncaught]` crashes on
purpose with it installed (checked 2026-11-15: the chain named crashHere and crashDeeper with their lines, in Release;
the uncaught one its message; 7 crashes left 5 dumps).

**Debug** (the user asked, 2026-11-15: "to the right of Log tab, add Debug tab, where you sum up every ms cost of the
observex ... i want to see the same sum that i currently have written ... it should also be written to the ring buffer
incase i do 10s clip. its needed to know what to optimize"). The tab after Log, **Debug** (`show-debug`, off by
default), is a column right of the Log column (`placeColumn` again, placed first; 1/3 of the width at first,
`debugSplit_`, so its table fits; its line drags). `app/DebugView` `drawCosts`: the total ("29.2 ms", the top bar's
number) "on each screen image, over the last second", then a table: each part (a thread) and under it, indented and
muted, each step of its run that ran that second, with **Each** (ms on average), **Slowest** (ms), **Per s** (runs a
second) and **Busy** (each x per s: how much of its thread's time). Names keep their whole width; a narrow tile cuts
numbers. `core/Costs`: `CostMeter` (`begin` when a run starts after its wait, `lap(step)` after each step, `end` when
it is done; `take(seconds)` once a second gives a `CostPart`: the run and up to 14 steps), `costTotalMs` (each part's
run.eachMs added up, the parts that ran). Parts (`App::costs`, only those that ran): Capture (`Pipeline`: the hand-overs
to each finder, preview, screenshot, recording), Characters (`Recognizer`: copy in, network, live log, heuristics, to
snapper, to clip buffer, mirror test), Snapping (`BoxSnapper`: grey image, areas, read back, learning, snapping, to clip
buffer), UI and stats (`UiFinder`: grey image + read back, finding, parts copied, miniatures, parts read back, stats
reading, skill pictures, OCR), Ground (`GroundFinder`, every image: depth pitch, grey image + turning, grey image +
following), Depth (`DepthFinder`: squeeze + network, hand-over, shading, packing), Drops (`DropFinder`: copy in, squeeze +
network, hand-over), Clip buffer (`ClipRecorder`: copy in, encoding, trim, taking lines; only runs that encoded a
frame), App frame (`App::run`'s `frameMeter_`: housekeeping, skills followed, stats kept, geodata, loot watch, brain
tick, finders steered, preview copy, clip shown, new frame, brain acting, drawing, rendering; Present is not timed).
Not timed: Record and screenshots (written on their own threads), the Review. The old F3 numbers (`StatWindow`s) stay
as they were. The clip buffer gets the parts once a second and right before a save (`App::noteCostsInClip` ->
`ClipRecorder::costed`; only while the buffer runs and no review or clip is open), written as `cost` lines (see
`core/ClipLines`); on an open clip the tile shows the look whose second holds the frame shown ("as recorded over the
second up to 0:04.2 of the clip", `ClipView::recordedCosts`; a clip saved before: "Not recorded in this clip").
Checked 2026-11-15 in hidden snapshots on the user's game: the total matched the top bar; Characters' network 4-8 ms at
120/s (its thread over half busy), UI finding 7-8 ms at 10/s, snapping's read back 1-1.5 ms. Unit tested
(`tests/CostsTests.cpp`: the meter, the total, the cost line round trip, a clip's cost lines by frame).

**State tree** (the bot's brain; the user asked, 2026-11-12: first a graph editor like an animation graph, then UE5's
"State Trees" mirrored in full in a floating window, then "make it much simpler, delete most of the functions, i will
keep adding them", in observex's own look, in the State tile; then, the same night, details on the right, Load/Save,
transitions with conditions, the active states and a backend that runs them): `core/StateTree` is the tree, Root
(always there) and states under it, each a name, its children in order and its **transitions** (a state to go to and
**conditions**, of five kinds as UE's condition types, `Kind`: **Compare Number**, UE's Compare Float: left, `<` `<=`
`=` `!=` `>=` `>`, right, each side a typed number or one of the game's numbers in percent, HP, MP, XP, Target HP,
Cast (how far the cast bar is; 0 with no cast bar) or a skill's cooldown left (0: ready); **Has Target** (yes / no: the
target's window found; the user asked, 2026-11-12); **Target Name** `=` / `!=` text: = holds when the name has the
text in it, ignoring case ("Ashen" is True for "Ashen Wolf", the user asked); **Skill** (a skill added in the Skills
tab is Ready, Cooling down or Being cast: the cast bar's skill); **Is Skill Ready** (the user asked, 2026-11-15:
`SkillsReady`, a list of the profile's skills and **Any Skill** / **All Skills**: true while at least one / every one is
ready; ready, `statetree::isReady`, is found on screen, cooldown 0 and not just pressed; no skills: never). `Variables`
come from `GameStats` once a reading (`variablesOf`: Status HP/MP/XP, MP in points from the MP bar's number "90/108"
while it is read, `kTargetPiece` HP, `kCastingPiece` (`castShown`, `casting`), every `kSkillsPiece` gauge, and the
target as the stats reading caches it, the user asked 2026-11-13 for it there at once: `GameStats::hasTarget` and
`targetName`, the name last read while the target's window stayed up). **Enter conditions** (UE's; the user asked,
2026-11-15, after asking how Combat could do something else while Wind Strike cools: "branches inside a state?" no,
children picked by priority): every state but Root has a list of conditions like a transition's (`State::enter`); it
is entered only while they pass, and entering a state goes on down to its first child, in order, whose enter
conditions pass (UE's Try Select Children In Order; none: the state itself), so a parent's children are its choices
by priority and drag and drop orders them (see Runner below).
Joined as UE's, the first IF, then AND / OR left to right; none: always; what is not read (a bar unseen, no name, a
skill not found on screen) fails every condition that reads it (Has Target excepted); ASCII symbols, the font has no
U+2264 and they showed as "?"): adding as
a last child, removing with everything under it (not Root; transitions to removed states go too), the rows depth
first, an unused name ("New state", "New state 2", ...). Each state also has **tasks** (UE's; the user asked 2026-11-13 for
Add Task, 2026-11-15 for kinds that drive the Pico, then "tasks go one after another" and "each task has a definition
of done before next task is activated"). They run **one at a time, in order** while the app acts (`App::actuateBrain`
on every UI frame): the first when the state is entered (again too, by a transition to itself), each next once the one
before is done, none after the last until the state is entered again (`Runner::task`, `finishTask`, `taskStarts`: a
new count is a task starting, afresh). **Use Skill** (`core/SkillUse`'s `SkillUser`, unit tested; the user asked,
2026-11-15: "accept an array, and it checks all the skills top to bottom which one is not on cooldown and uses that",
and "keep spectating if skill ended casting OR mana cost of the skill was taken"): a list of skills; of them, top to
bottom, the first ready one whose shortcut can be pressed is pressed (held 80 ms); none: it waits, said once in the log,
choosing again on every reading. Done once the game shows it used: the cast bar showed it (or a cast not named) and
then no cast, or MP in points fell by at least 3/4 of its **MP Cost** (Skills tab) from the most it was since the press
(regeneration before the cost). Fallbacks (never stuck): no sign the game took it (cooldown, cast bar, MP taken) on an
image shown 0.3 s after a press (`kRepress`), pressed again, and so on until it does (about every 0.4 s with readings
0.1 s apart), given up on 5 s after the first press (`kGiveUpSeconds`); its cooldown began with no cast bar, done 0.4 s
later (with a cost and MP read, once it was taken or 2 s later); a cast bar over 15 s, or 20 s since the first press:
done; a skill with no cooldown and no MP cost (Sit) shows nothing, so it is done once pressed (pressed again, Sit stands
up). Its **pressed look is no sign**: the game flashes the slot also when it refuses the skill. Until 2026-11-15 it was
one, with a press again only 1 s after a press and done after 2 s of only the flash; the user saw Wind Strike Ready in
the Stats tab and no try for 1-1.5 s, and the log showed why: Wind Strike read ready about 0.8 s after its cast ended,
the game ignored F2 then and again 1.1 s later (flash and all) and took it on the press 3.3 s after Ready, in both
loops of the day. Pressing a skill again while the game casts it does nothing. The task is done once its key is let go
too. Log: "Use Skill Ice Bolt: pressed F3 (not ready: Wind Strike)", "Use Skill Wind Strike: done, its cast ended (1.42
s after its cast bar showed)", "... done, its MP taken (102 -> 93, it costs 10)", "... pressed F2 again, no sign the
game took it; pressing it every 0.3 s until it does" (said once), then "... done, its MP taken (171 -> 162, it costs
10, after 6 presses over 2.00 s)", "... gave up, pressed 13 times in 5.08 s and the game never showed it taken", "Use
Skill: Wind Strike, Ice Bolt not ready, the task waits". **Click Mouse** once clicked (pressed, held, let go);
**Select Target** (Lock Mouse Onto until 2026-11-15, renamed; `core/TargetSelect`, unit tested) once the game shows
the monster as the new target: it chooses the biggest monster (no distances yet; the same one while seen or lost
under 0.3 s), moves the cursor onto its box's middle quickly and smoothly (12 x the distance a second, 500 to 7000
px/s, its speed changing at most 40000 px/s a second and slow enough to brake at that by the middle: the unit test's
1166 px in under 0.6 s, arriving under 1200 px/s), asks the stats reader for the **name plate** over it
(`App::lookAt` -> `UiFinder::setLook`), and once a reading of an image shown since the cursor came into the box (35%
of its size from the middle; the box as last seen, so a monster missing from a frame keeps the cursor on it) has the
plate up with grey orbs (hovered) it left-clicks (pressed, held 80 ms with the cursor still, let go); done once a
reading of an image shown after the click has the plate's orbs coloured and the target window up (when both names are
read and disagree, `sameName`, it waits 1 s, then takes it with a note), or, the plate not seen coloured (a tree over
it), the target window shows another target than on the reading before the click (none then, another name, or the one
then dead, at most 0.5%, and this one alive, at least 5%: the same name). Coloured orbs before any click: it was the
target already, done at once; coloured orbs without the target window: never clicked (a click on the target attacks),
done after 1 s with a note. **Never a dead target** (the target window's HP at most 0.5%; 2026-11-15, the log showed
it twice: chosen as it died, "Imp was the target already", and the brain stood in Seek Monster 7 and 15 s, as Seek
Monster -> Combat wants Target HP > 0, until the user stopped it): already the target, it is given up on at once and
another chosen ("... gave up on monster 525, it is the target already, but dead"); just clicked, the window may still
show the dead one before, so 1 s, then given up on ("... the target window shows it dead"); seen only by the target
window (its plate hidden), a dead one is no new target. **Fallbacks, so it never stays stuck** (the user asked, 2026-11-15: the cursor sat on a
monster whose plate a tree hid until the brain was stopped, twice; "getting stuck is death"): never shown hovered on
the images shown 1 s after the cursor came onto it and 1 s after it last showed hovered (`kHoverWait`: the plate
hidden, or the cursor not on it as the game sees it), given up on; no sign 1 s after a click, clicked again once
hovered, at most 3 clicks, then given up on; anything else, not the target 5 s after it was chosen
(`kMonsterSeconds`), given up on. Given up on, another is chosen; that one (also a new tracker id whose middle is in
its last box while its own id is gone: found again) only 2 s later (`kRetryAfter`), when it may have come out from
behind the tree; meanwhile with no other the log says so once. Not a fallback yet: no monster on screen at all, the
task waits (a transition on Nearby Monsters is the tree's way out). The log says each step: "Select Target: going for
monster 12 (96 x 130 px, the biggest of 3)", "... clicked monster 12 again (click 2), no sign it was selected", "...
done, Goblin targeted in 0.62 s (1 click)", "... done, Goblin targeted in 0.70 s (1 click; seen by the target window,
its name plate not seen)", "... gave up on monster 12, the cursor was on it 1.0 s but the game never showed it hovered
(no name plate over it, 0 orbs round it)", "... gave up on monster 12, the game stopped showing it hovered (...)", "...
gave up on monster 12, not the target 5 s after choosing it (2 clicks, shown hovered)", "... no monster to select on
screen but 1 given up on a moment ago, tried again within 2 s", "... lost sight of monster 12, choosing another", "...
gave up on monster 12, 3 clicks and never shown as the target", "Click Mouse: Left button clicked". The tree shows each state's tasks inside its box and
**highlights the task that runs** there (see the tree below). **The Brain** (the user asked, 2026-11-15, round 2):
started and stopped by the State tile's **Brain** button right of the profile's name (`BarButton`, caption "Brain",
**Active** green with a stop icon / **Inactive** with a play icon; it was the top bar's "State tree Working/Paused" and
a "Paused" dot in the tile; the profile's name field narrows to make room, and in a tile too narrow even then it goes
under them; no tooltip, the user asked, 2026-11-15: why it could not start is a Log line, "Brain not started: observe
a game screen first", "... controller: WHY", "... the controller did not answer; is the firmware running?") or
**Win+Esc** anywhere (`app/HotKey`: Windows' RegisterHotKey on a thread of its own, while a screen is
observed; the plain-Escape low-level keyboard hook it replaced never saw a key while the game was in front, as the game
runs as administrator and observex does not, and Windows keeps such hooks from an elevated window's keys; a registered
hot key is not kept). Starting it starts the whole tree afresh from **Root** (`StateTreeView::restart`); while it is
inactive the runner does not step and **nothing is green** in the tree (no current state, no running task, no condition
dots: `StateTreeView::isCurrent` is false then). The window's **border**: green while the brain is active, red while a
screen is observed and it is not, none while no screen is. The **Controller** light (dot, "Controller", Connected / Not
Connected) is right of the tabs, after Log. **Runner** (the user asked, 2026-11-13: "highlight the current state,
initially Root, and write the simple state machine that already checks conditions"; 2026-11-15 UE's child picking by
enter conditions): one current state, Root at first. On each new reading the transitions are tried from the current
state up through its parents (UE's leaf to root), each state's in order, Root's only while Root is current (Root stays
the hub: its transitions are not every state's); the first whose conditions pass and whose target can be entered
(`Runner::canEnter`: its enter conditions pass) goes, and lands where entering leads (`Runner::landing`: down to the
first child, in order, whose enter conditions pass; Root never goes down, its children are reached by its
transitions); a transition with no target never goes. Once the current state's tasks are done and no transition went,
it picks again as entering does, from its parent (from itself if it has children: it was landed on as none of them
could be entered), and moves only if that lands on another state (a child of Root has no parent to pick from and stays):
so Combat with children Strike (enter: Wind Strike ready, task Use Skill Wind Strike) and Hit goes on to Hit once Wind
Strike was used and cools, and back to Strike once it is ready and Hit's tasks are done. Stopped, loaded, or its state
no longer in the tree, it is at Root again. `Runner::active`: the current state and its parents.
**In clips** (the user asked, 2026-11-15: "when recording last 10s, and then reviewing, i dont see active task/state
highlights"; opening a clip stops the brain, so nothing was green): the clip buffer keeps what the tile showed green
as clip.txt `brain IMAGE TIME ACTING TASK [STATE]` lines (`App::noteBrainInClip` after each UI frame's tasks ran: a line
when the brain starts or stops acting or its current state or running task changes, and one when the buffer starts;
timed by the image the window showed then, as the ground lines; `core/ClipLines` appendBrain/readBrain, `ClipText::brains`,
`recordedBrain`). STATE is the current state by its names below Root, " > " between (`statetree::statePath`, e.g.
"Combat > Attack Spell"; empty: Root), as a profile read again gets other ids. With a clip open the State tile shows the
brain as recorded at the frame shown (`StateTreeView::Replay`, `stateAtPath` in the profile shown): the state green,
its parents' borders, the task that ran green and those before it fainter, whatever the guesser; the condition dots
and the pickers' values on the reading the Stats tile shows (Recorded's clip-stats.txt or Today's). A muted line under
the bar when it cannot: "Brain not recorded in this clip" (saved before 2026-11-15), or "The clip's brain was in X, not
a state of this profile" (renamed since, or another profile). The Brain button stays the live one (Inactive in a
clip). Checked 2026-11-15 in hidden snapshots: a copy of clip 2026-11-15_20-53-34 given brain lines (Combat > Attack
Spell at frame 1 with Use Skill green; Seek Monster at frame 680 with Select Target green, "Has a target" dot green and
"Target HP > 0" grey as the Goblin had died), and a clip saved in snapshot mode carrying its `brain` line.
**Parallel tasks** (the user asked, 2026-11-15, step 1 towards a camera-turning task that runs during casts: "a
parallel task", picked on the task, "visible visually that it runs all the time"; UE's tasks of an active state):
`Task::parallel`, a **Parallel** checkbox before the task's X in the details, only on kinds that share nothing with
the tasks one after another (`statetree::canRunParallel`: Debug Print for now; Use Skill, Select Target and Click
Mouse press the keyboard and mouse those use, so they get it with a mouse-sharing rule). A Parallel task is not one of
the tasks one after another (`Runner::task` skips it; a state with only Parallel tasks is done at once, for Tasks done
and picking again): it runs the whole time its state is active (current, or above the current one: Combat's while its
child Attack Spell runs; Root's only while Root is current, as Root's transitions), from when its state becomes active
until it is left, never done (`Runner::parallelTasks`, from the top state down; `App::runParallelTasks` each UI frame
while acting, `parallelRuns_` emptied when the brain starts or stops). In the tree its marker is two upright bars (no
skill miniature), green the whole time its state is active, also in a clip's replay. File: `task ID parallel KIND ...`
(read as not Parallel on a kind that cannot be). **Debug Print** (TaskKind::DebugPrint, nothing to choose): writes
where the brain is into the Log (`statetree::whereText`): "Debug Print: Combat > Attack Spell, running Use Skill" (",
its tasks done" once a state's tasks one after another ran; Root's name for Root); one after another it writes once
and is done; Parallel every 0.3 s (`kDebugPrintSeconds`). Checked 2026-11-15 in unit tests (runner, file) and hidden
snapshots (the bars, the checkbox, and a clip copy given a brain line in Combat > Attack Spell: Combat's Debug Print
green next to Attack Spell's Use Skill); the Log lines need the brain running on the controller (the user's check).
**When** (the user asked, 2026-11-15: UE's "transition at any time OR after all tasks are done"; `statetree::When`):
each transition is **Any time** (UE's On Tick: tried on every reading, also while a task runs; every transition
before, and a file's `go` line without a word after its target) or **Tasks done** (UE's On State Completed: tried only
once the current state's last task is done, or at once for a state with no tasks; `go FROM TO done`). A reading tries
all Any time ones first (current state up through its parents, as above), then, only with the current state's tasks
done, the Tasks done ones the same way, and only then picks again. So a parent's Any time transition (no target, low
HP) always wins over a child's Tasks done one, with no priorities needed, and a state's Tasks done transition to itself
repeats it: its tasks again from the first (Attack Spell -> Attack Spell casts Wind Strike again after each cast; an
Any time one to itself enters again on every reading, its task starting afresh each time, and picking again never
moves to the same state, so before When a state's tasks ran only once). UE's per-state Tasks Completion (All / Any) is
not needed: a state's tasks run one after another here, so done is the last one done. Not yet: UE's On State Failed
(no task fails yet), events, priorities, delays.
`App::tickBrain` gives it a step on every new stats reading (live, from the UI finder; a clip's Today on the frame
shown) and stops it while reviewing or when nothing is read. **Files**: `.brain` (observex's own extension), text,
`observex-brain 1`, then `skill ...` (the Skills tab), `state ID PARENT NAME`, `task ID KIND ...` (above), `enter ID`
(its `if` lines are that state's enter conditions), `go FROM TO [done]` (done: When Tasks done), `if and|or LEFT CMP RIGHT` (LEFT, RIGHT: hp, mp, xp,
target-hp, cast, `cooldown:SKILL` with "_" for spaces, or a number), `if and|or has-target yes|no`, `if and|or
target-name ==|!= TEXT`, `if and|or skill ready|cooling|cast SKILL`, `if and|or skills-ready any|all SKILL|SKILL...`
lines (`writeTree` /
`readTree`, which says the line and why it cannot read one), in Settings' **brains folder** (`brains` in observex.cfg;
default `brains` next to observex.exe; folder picker, Open, Use default under the recordings folder). **Profiles**
(the user asked, 2026-11-13: "we always work on a profile", named, say "Basic Combat - Mage"): each .brain file is
one; the tile always works on one: the last one opened (`brain` in observex.cfg) is loaded at start, else the newest
in the folder that reads, else a new one, "Brain", saved at once (`StateTreeView::openProfile`; also when the folder
changes). **Every change is saved** into it as it is made (no Save button; a .tmp first, renamed over; a field being
typed in is one change once it is left; `flush()` at shutdown for one still open), and **Ctrl+Z** undoes any change
to the tree, **Ctrl+Y** or Ctrl+Shift+Z redoes it (held, they repeat; a field being typed in keeps them for its text):
`statetree::History` keeps whole trees, ids and all, up to 200 steps, so any operation added later is undone alike;
a change is a tree whose `writeTree` text differs; loading starts a new history. `app/StateTreeView` draws it: at the
top left **Load** (a list of the folder's profiles, the folder under it) and the profile's **name in a field**, 240 px:
typing another and Enter (or leaving it) renames the profile and its file, Escape keeps it; a name another profile has
gets a number, the one at its end counted on, else " 0" ("Mage" -> "Mage 0" -> "Mage 1"; the user asked); no Add State
button, it is in the right-click menu: the user asked, 2026-11-13). Under them the tree on the left and,
only while a state is selected, its **details** on the right (45% / 55%, a 1 px line between that drags; nothing at
all, no line, no text, with none selected, and the tree takes the whole width: it had cut Root in a narrow tile).
The tree: each state a box like
the top bar's buttons (`kSurface`, `kBorder`, 8 rounded, the name in medium weight; lighter hovered; selected: the
accent border; **current: green** while the brain is active, a faint `kYes` fill and border, Root at first, and its
parents a green border (the active states); its enter conditions muted after its name, "if Wind Strike ready",
"if HP < 50 and a target", "if Wind Strike, Ice Bolt or Heal ready" (`statetree::describe`); its
**tasks inside it under its name** (the user asked, 2026-11-15: "tasks should show up in the left column under states,
but state should be visually different than a task"): a 24 px line each in the regular font at 86%, a small diamond (Use
Skill: its first skill's miniature), the kind, and muted what it works on ("Select Target  Nearest Monster", "Use Skill
Wind Strike, Ice Bolt"); the one that runs
a green line (faint `kYes` fill and border) inside the green box, those done before it fainter; the box as tall as its
name's row and its tasks), 24 px indent a depth, lines (`kBorderHover`) from
under each parent down and across to its children, and after each box its transitions, UE-like: a small arrow (a 1 px
line on pixel centres and a filled head, `kTextMuted` at 75%; the user found the open chevron "weird and ugly",
2026-11-13) and the target's name, muted; a straight arrow, or a returning one (along the bottom, round, back along
the top to a head pointing left) only when the target is the state itself (until 2026-11-15 also for one above it,
as UE's; the user: Rest -> Root looked like Rest going back to itself); a Tasks done transition has a small tick before
its arrow (`drawDoneMark`, the arrow's colour, 1.5 px). A click selects (on empty space: nothing); a double
click or F2 renames in place (trimmed; an empty name is refused; Escape keeps the old one); Delete removes the selected
with all under it (not Root); a right click offers Add State, Delete (on empty space: Add State, under Root).
**Drag and drop** (the user asked, 2026-11-15: change the states' order, with "visual feedback when dragging onto
something so that i know if it will land above or below"): a state held and moved past ImGui's drag threshold (6 px;
not Root) is dragged with all under it (`StateTreeView::drawDrag`, `Tree::move`, unit tested). Over a box's upper half
it lands just before that state, over its lower half just after it and all under it, each time as its sibling (so it
can change parent too); over Root, as Root's first child; a gap counts to the nearer box. An accent line with a small
ring at its left end shows where, in the gap it lands in, starting at the depth it lands at (after a state with
children: under the last of them, at that state's depth). The dragged state and all under it are dimmed in place, its
name in a small accent box right of the mouse (held under the mouse it hid the names of the states it was dropped
by); over itself, a state under it, or off the tree there is no line, the cursor says not allowed, and letting go
moves nothing; Escape cancels. Near the tree's top or bottom edge it scrolls. Let go, the move is one change: saved,
and Ctrl+Z undoes it. Checked in hidden snapshots 2026-11-15 (`--hold`): above Rest, after Fight and its children,
after Kill under Fight, a state over its own child (no line), drops saved in the file's new order, Ctrl+Z and Ctrl+Y.
**Transitions drag too** (the user asked, 2026-11-15: "drag Transitions similar to how i drag states"): each
transition after a box (its tick, arrow and target's name) can be grabbed: a faint pill behind it under the mouse; a
click on it (let go on it, not dragged) selects its state, not on the press, as the details opening would move the tree
under a drag. Held and moved past the drag threshold (`StateTreeView::drawTransitionDrag`, `Tree::moveTransition`, unit
tested), it lands in the row under the mouse (the gaps count to the nearer box, as for states): before the first
transition there whose middle is right of the mouse, else after the last (over a box or left of it: first). In its own
state's row that is a new order (transitions are tried in order: the first that passes goes); in another state's row it
becomes that state's, its target, When and conditions kept (so Combat -> Rest dropped on Rest is Rest -> Rest, a
repeat). An upright accent line with a small ring at its top shows where, in the gap it lands in; the dragged one is
dimmed in place, its tick, arrow and target's name in a small accent box right of the mouse; just before or after
itself, or off the tree, there is no line, the cursor says not allowed, and letting go moves nothing; Escape cancels;
near the tree's top or bottom edge it scrolls. Let go, the move is one change (saved, Ctrl+Z undoes it) and the state it
went to is selected, so its details show it. Checked in hidden snapshots 2026-11-15 (`--hold`, `--drag`): hover pill,
a reorder to first, a move to another state's row, over itself (no line), drops saved in the file's new order with the
conditions kept, Ctrl+Z back to the file before.
Tasks cannot be dragged yet: they stay in the order they were added.
The details: at the top the name in a field (no heading, no label; 240 px, not the details' width: the user asked,
2026-11-13), then **Enter Conditions** (not on Root: only "+ Condition" while it has none, else a card of condition
rows as a transition's, `StateTreeView::drawEnter` / `drawConditions`), then **Transitions**, each a card: "Go to" a state (a
list of all, indented by depth) and an X, under it "When" Any time / Tasks done (the two fields lined up), its
conditions (IF / an AND-OR toggle, then by kind: left, compare, right;
"Has a target" / "Has no target"; Target Name, = or !=, a text field; a skill list (the profile's skills, with how the
last reading saw them) and Ready / Cooling down / Being cast; Is Skill Ready: Any Skill / All Skills, then X, and
under them its skills as a **skill array** (below; on a narrow card from under IF); then X; on narrow
details the second half on a line of its own; while the state is active a dot by each, green while it holds), "+
Condition" (picks the kind: Compare Number, Has Target, Target Name, Skill, Is Skill Ready); then "+ Transition". A number side is a
field and an arrow to pick one of the game's numbers (each listed with its last value, the skills' cooldowns after
them); a game's number side picks "Number" to go back (starting from its value). Then **Tasks**: each its kind with a
target or button picker and an X (the one running is shown green in the tree, not here); Use Skill's skills are a
**skill array** with the task's X at its top right; and **+ Task** (picks the kind). **Skill arrays** (the user asked,
2026-11-15: "look how unreal does it", a vertical array of rows, a new element only by clicking "+", no checkmarks and
no "Wind Strike, Sit" preview; `StateTreeView::drawSkillArray`): a rounded well (darker than a card it is on, a card's
colour on the panel), at its top a muted "Skills" and how many, and a + at the right that opens the list of the
profile's skills under the well (each its miniature, name and, right, how the last reading saw it; the ones already in
the array dimmed, "added", not pickable: a skill is in it once); then a row per skill: its place (0 first, as Unreal's
Index; under the mouse six grip dots, and dragged by them the skill moves up or down, the others closing up, one change
once let go: Use Skill's order is the order it tries them), its miniature, its name (ending "..." when too narrow) and a
chevron (a click opens the same list under the row to pick another), and an X; the row lighter under the mouse.
The Skill condition's list has the same miniature rows. Unit tested (the tree, every
condition kind, reading the gauges, the runner, the file round trip and its errors, the history's undo, redo and
limit). Checked in hidden snapshots 2026-11-13 (a new folder: "Brain" made and saved; Add State, naming, Ctrl+Z twice
and Ctrl+Y, each step in the file; renaming to "Basic Combat - Mage", and into a taken name: "Basic Combat - Mage 0";
Add Task saved, and undone). Checked in hidden snapshots 2026-11-12 night: building a
tree with transitions and conditions, the returning arrows, the menu, Save into the folder, Load, and the user's game
observed: Root and its first child green, the details of the active state. Again 2026-11-13 after the user's round 4:
nothing right of the tree until a click, the name field on top, Add Task, the crisp arrows (a repeated path point had
blurred the returning one's bottom line), the 2/5 column, and the game observed with Root -> Rest always, Rest ->
Fight on Has Target: Root green at first, then Fight with an Ashen Wolf targeted. For later, UE 5.8's StateTree runtime (read in
D:\rds\Engine\Plugins\Runtime\StateTree, StateTreeExecutionContext.cpp): selection goes down from Root; a state fails if
turned off, its required event is not sent or its enter conditions fail; its selection behaviour picks its children, a
child that fails is passed for the next (backtracking), and if all fail the state fails; every active state, Root to
leaf, ticks its tasks in order; a state completes by its tasks (Any: the first success; All: all; a failure always
wins; no tasks: succeeded) and stays active until a transition goes; each tick the tick and event transitions are tried
leaf to root, the highest priority wins (ties: the deepest state, then the first in its list), only then the completion
ones from the completed state up; with none, selection starts again from Root, and if that fails the tree fails; a
delayed transition counts down and then goes without trying its conditions again; an event lasts one tick; at most 5
transitions a tick.

Recording (step 1 of the training loop): **Record** (no hotkey: the system-wide Ctrl+Alt+R, Ctrl+Alt+M and Ctrl+Alt+C
were removed on 2026-11-12, the user asked) saves the observed screen into a new folder named by date and time, e.g.
`recordings/2026-10-31_23-05-12/000001.jpg`, 1 image every 3 seconds (fixed, `kRecordingInterval` in App.cpp: the
user asked for no rate settings, then for 1 every 3 s instead of skipping near-copy images in the review,
2026-11-01). Only images the screen actually updated are saved. The Record button first asks "What do we record?"
(the user asked, 2026-11-02; `RecordKind` in core/Recordings.h):
- **Every 3 s**.
- **Only unsure**: since 2026-11-12 (the user asked) it records for one minute (`kUnsureSeconds`), an image due every
  second (`kUnsureEvery`) and saved only when the model is unsure of it: at most one a second, none while it is sure
  of everything (before, at most one every 3 s until 110 images). When an image is due the pipeline asks (`core/Question`, `Pipeline::question`,
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
  images). Yes saves the next new image; no skips that step. So about two thirds of the steps save with the
  ninth, about 13 images a minute (half with the eighth). On its held-out images the eighth's kept half held 71% of the
  model's mistakes (a random half 52%, the half with the most things 30 to 65% sure 68%) and 60% of its false
  alarms 65% sure or more (43%): the mirror test also catches mistakes it is sure of (consistency-based active
  learning, CALD). Mixing in the unsure-things count did no better. The C++ test matches Python's (`--images`
  below: correlation 0.97, the same answer on 98%). Each answer is a `mirror` line in the live log, to retune the
  cut-off from real recordings. (The first Only unsure, built earlier on 2026-11-02, saved after looks with 2 or
  more things 30 to 65% sure; replaced the same day.)

Every 3 s stops by itself after 110 images (the user asked, 2026-11-03; `kRecordImages`), only unsure after its minute
(`recordingDone`: the main loop checks the saved count or the time, and a label then says it is done), or when the
user stops them.
Auto (60 images every 3 s, then 50 only unsure, then it stopped; 2026-11-02) was removed then, as the user asked: on
the user's first new place (2026-11-02_23-46-18) the eighth's mirror test would have kept 38 of the 60 every-3 s
images, and those held all 23 of its false alarms, 31 of its 35 misses and 15 of the 18 with nothing seen; the 22
skipped were quiet (2.3 characters each against 4.7). So per image to review, only unsure finds more of every kind
of mistake; it only takes longer to walk (110 images in about 8 minutes instead of 5.5). Every 3 s stays for an
unbiased look now and then: what only unsure skips is never saved, so only those show whether it is still quiet.

While reviewing, recognition is off, so only unsure saves nothing then (its minute runs on). Starting and stopping
writes
`recording.observex` into the folder (`core/Recordings`: `key=value` lines `started`, `seconds`, `screen`, `size`,
`every`, `kind` (`every` or `unsure`; `auto`, the removed kind, reads as `unsure`; the first Only unsure ones say
`unsure=1`), `images`, and `reviewed-<category>` once a review changed answers (see Review), written through a
temporary file; the review's list says "only unsure" on those); the review
lists only
folders that have one (the user asked, 2026-11-01:
choosing a folder felt unnatural). The 11 recordings made before got theirs from a one-off script (`started` from
the folder name, the rest from the images and the live log).
While recording, the button reads "Stop", the time, "38 of 110 images" and about how long is left ("about 4 min
left", `recordingSecondsLeft`), or for only unsure the images so far and the seconds left ("12 images · 37 s
left"), with a progress bar along the button's bottom (`BarButton::progress`, `recordingProgress`: images or time), so the user knows how much more to walk around. Its tooltip gives the size on disk (the
Recorder sums each file's size; shown in 1024s, as File Explorer does). JPEG quality 85: about 0.7 MB per 2560 x 1440 Lineage 2
image, so about 0.85 GB per hour. **Settings** (gear) holds the recordings folder (default: `recordings` next to
observex.exe; folder picker, Open, Use default), under it the brains folder (the State tile's .brain files; default
`brains` next to observex.exe) and the live log flag (below). Settings live in `observex.cfg` next to the exe. Both it and
`recordings/` are gitignored.
Recording costs capture nothing measurable (bench: 238.7 and 239.3 images/s recording, 238.2 and 239.2 not).
Live log (the user asked, 2026-11-01, to show Claude what they see live: sureness that is high at times and low at
others, one-image blips): while recording, `engine/LiveLog` writes `character-live.txt` into the recording's folder:
a `look` line for every image the recogniser looked at (screen image number, time, everything found at least 10%
sure with its box; the app draws 30% and up), for a model with kinds a `kinds` line right after each (since
2026-11-08: each thing's two surest kinds and how sure of each, in the look's order; the format is `core/LiveLines`,
shared by the writer and `--track`), `saved` lines tying each saved JPEG to its screen image number,
`mirror` lines (each Only unsure answer: image number and how differently it saw it mirrored; with kinds, also how
close any thing's two surest kinds were), and `mark` lines (until 2026-11-12 **Ctrl+Alt+M** marked a moment: a numbered `mark` line, and the image on screen
saved right then, `Pipeline::recordNow`; the hotkey is gone, the bench still marks once halfway). Claude reads the log: follow each thing from look to look
(match boxes), plot its sureness over time, find one-look blips, and line them up with the saved images. About 85
MB per hour; lines are formatted outside a lock and buffered (1 MB), marks flushed. The file's header explains the
lines. A Settings checkbox, "Log what the model sees while recording" (`live-log` in observex.cfg, on by
default; the user asked for it), turns the log off from the next recording (no grey text under it since 2026-11-12). Image numbers come from `ImageStamp` (the pipeline's count and the time the screen showed it), which every
`ImageChannel` hand-over now carries.

Review (step 2 of the training loop, `app/Review`): **Review** lists the recordings (the recordings folder's
subfolders with a `recording.observex`, newest first: a picture of the first image (the user asked, 2026-11-02;
`app/Thumbnails`: read 320 pixels wide, one per frame and only for rows in view, about 6 ms each for a 2560 x 1440
JPEG, kept until observex closes; WIC's JPEG shrinking saves little, 9.6 -> 5.8 ms); when, e.g. "Today, 12:56";
length; images; how many are reviewed per category; read once per opening), then "What are we looking for?" (categories in `kCategories`: Characters, and Drops since 2026-11-15), then shows the recording's
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

Tracker (`heuristics/Tracker`, built 2026-11-01; the user saw sureness swing and one-image blips, see the live log):
follows what the model finds from look to look, so each character keeps one steady box and an identity (id, speed).
A new thing starts being followed only from a box at least 60% sure (`kStartSure`; 50% until 2026-11-08) and counts once found on 3
looks in a row; once followed, boxes from 30% keep it (pairs with a sure box are matched first). One that is not
found is held where its
speed takes it, as long as it was seen, up to 150 ms; matching is nearest-first to where each is heading (reach
grows 0.5 sides per 100 ms unseen), or a box lying 80% inside the other (cut by the screen edge); `sure` is smoothed
over about 60 ms; its kind, how sure of it and its second kind come from per-kind scores smoothed over about 300 ms
(each look counts the model's surest kind and its second, so on a steady answer they are the model's). Until
2026-11-11 the kind's sureness and the second stayed as the first look had them: a wolf first seen 58% sure of
monster showed "Character?" for the 9 s it was followed, while the model said monster 80% on average; the fix
re-tagged 614 of the 10898 boxes shown in that clip (`--clip-guesses`), moved none. **Boxes are handed out where
their thing is by the time they are on the screen** (2026-11-11; the user saw every box shifted in a camera turn
and said box precision is SUPER important, the bot will click box middles): until then a box was where the thing
was on the image looked at, about 12 ms before it showed (9.5 ms to look, then until the screen's next image), so
in a turn (3-4 px/ms) boxes trailed 30-50 px. Now its speed is a least-squares line through where it was found
over the last 20 ms (`kSpeedMs`, at least two looks; it was the speed so far followed by 30% of each step, which
lagged a turn), and each box is led along it by `kLeadMs` 12 ms. On the 12:10 clip (`--clip-guesses`, against the
model's boxes on each frame's own image): boxes of things moving fast off 26.5 -> 11.1 px on average, middles
outside the thing 63.5% -> 24.2%, in its middle half 14.8% -> 47.8%; all boxes outside 6.9% -> 2.9%; boxes
coming or going 5.9 -> 5.3 a second. What is left is the game's own unevenness (an orc in a steady turn moved
51, 5, 51, 15, 30 px on images 8, 4, 8, 4, 8 ms apart), which nothing in the looks foretells, so a bot should
click when the camera is still or slow. No ABI change: it reloads. Foretelling cannot do better: on frame 361
(a turn speeding up, 16, 40, 55, 70, 119 px a look within 50 ms) boxes still trail 35-50 px, and six more ways
(shorter line, speeding-up terms, the faster of line and last steps) all left more fast boxes outside their things
(22-33%). **Boxes snapped onto every new screen image** (`heuristics/SnapRules` and `heuristics/Snapper`, live since
2026-11-11; the user asked for boxes snapped to the characters, since in camera turns foretold boxes still miss):
each look teaches how each thing looks (its box on the look's own image), and on every new screen image
(`engine/BoxSnapper`) each box is looked for near where its speed takes it (zero-mean normalised cross-correlation,
coarse to fine over halvings, to a fraction of a pixel; 0.6 the least match, 0.7 or 0.8 were worse: blur lowers real
matches; reach 20 px plus all its speed took it since), its speed then a line through where it was snapped over 30
ms, and handed out 6 ms ahead (`SnapRules::kLeadMs`, about until the next image). Things not found stay where their
speed takes them (held). While it runs the UI draws its boxes, not the recogniser's; clips record them (`snapped`
lines) and Recorded shows them from the first one on. On the 12:10 clip (`--clip-guesses --snap`): on the image
snapped, fast boxes 2.4 px off, 3.2% of middles outside their things (Today: 11.1 px, 24%), all 0.7 px; 94% snapped.
As shown until the next image: fast boxes 9.1 px off, 15.5% outside. What is left is the game moving things
unevenly between images (moved on by exactly the time to the next image, which is not known live: 7.6 px, 11.7%),
so a bot clicking a box should lead it by when its click lands. A thing behind the minimap snaps onto the minimap.
**Game hitches** (fixed the evening of 2026-11-11; the user, on the 19:08 clip, frames 490-491: a box well off the
nearest orc, then a second box beside it, boxes sliding right): the game showed no new image for 50 ms (the one gap
over 20 ms in both clips), things then 16 px on instead of the 340 their speed said, and 4 ms later the camera caught
up 120-160 px and stopped. The tracker had foretold the whole 50 ms, lost everything and followed it again as new
things, while holding the old ones where their speed took them (the sliding second boxes); the snapper took those
held guesses over its own (right) places, and looked as far as the speed went, so that one image took it 30 ms
(boxes from before the hitch shown meanwhile: frame 490's offset). Now both foretell at most 20 ms
(`Tracker::kMaxAheadMs`), start a thing's speed line again where it jumps off it by over 60 px
(`Tracker::kJumpPixels`; normally within 20, 10 of 27,970 finds farther), the snapper keeps its own place for a thing
the look holds, drops a held box snapped onto another's (the same thing twice), and hands out nothing found by neither
the looks nor the images for 60 ms (`SnapRules::kGoneMs`; boxes on nothing 1141 -> 637 on that clip, 570 -> 427 on
12:10; 30 ms more, but boxes come and go more). Left: far small characters jump more than the tracker reaches (it
matches within about their own size), so for up to 60 ms a ghost of one stays where it was; the orc's box is off for
one image (the one before any look of the moved scene is done).
**Today snaps too, and on a new clip it is Recorded exactly** (the evening of 2026-11-11; the user: whenever we record
something new and open it the first time, Recorded and Today should show the exact same thing, so we can iterate on
heuristics). The clip screen shows the heuristics' own sets at once, then `engine/ClipSnapper` (its own thread at
below-normal priority and its own GPU device; about 5 s for a 10 s clip) snaps them onto the clip's video as the
snapper thread does live (`core/ClipGuesses` snapGuesses), on the recorded schedule: which images were snapped, when
each set was done and which look's things it had taken in (`snapped` and `finds` lines; a clip without them: every
new frame, done 2 ms after it). The video is not the screen bit for bit (compressed, and without the images the
encoder had no time for), so live the snapper notes each search it makes (`core/Heuristics` SnapFind: the finder's
version, the thing, the image and box it learned it from, where it looked and how far, what it found and how well),
clips keep them (`finds` lines), and the replay takes the recorded result of a search that is the same
(`SnapFind::sameSearch`: the same finder and learned image, the rest within 1 px) instead of searching the video. So
until the heuristics change Today is Recorded; a changed rule makes other searches, and those search the video.
**Bump `Snapper::kFinderRevision` whenever its learn or find change**, or results of the old finder are taken for the
new one. `observex_bench --clip-guesses` says at how many sets Today is drawn as Recorded, and when the first differs (the
Guesses popup said it, and the snapping's progress, until 2026-11-12); for a clip saved before the `finds` lines it
cannot be exact (the 19:46 clip: 22% of the sets). Checked by a unit test running the live rules and snapper on made-up images, writing the clip
lines and replaying them on other pixels (every set the same, and not without the `finds` lines), and on the first
three clips recorded with them (22:07, 22:10 and 22:37 of 2026-11-11: 1507, 1498 and 1195 snapped sets, all the
same). Needed ABI 3 (`kObservexHeuristicsAbi`).
**A lost thing in a camera turn** (the evening of 2026-11-11; the user, on the 19:46 clip, frames 917-923: a
"Character? 51%" box above a wolf next to a player, standing still on the screen until 922, then gone, and the wolf
without a box): a monster lost 130 ms before in a fast turn was held (standing still after its 20 ms) and took a 47%
box the model found on a pale shape above the wolf (once followed, any box counts); the snapper never found that on
the images after and held it, also standing still after 20 ms, for its 60 ms. The wolf itself the model found only
12-52% sure in the turn and 16-27% after it (below the 30% the recogniser hands over), so no rule boxes it; only a
model that sees it better would. Now a thing moving 0.8 px/ms or faster is held at most 100 ms
(`Tracker::kFastHoldMs`; on the 31 live logs at their 2,399 answered images, `--track`: boxes on nothing marked 2149
-> 2040, characters shown 10514 -> 10505 of 14526, boxes appearing or vanishing 3.706 -> 3.710 a second; on the four
clips, snapped: boxes on nothing the model found 5-9% fewer, its things without a box +1.5%), and the snapper holds a
box on along its speed from image to image (at most 20 ms a step, so a hitch still counts 20 ms): as near and as many
as before, but moving with the turn. Tried and left out: the tracker moving held things on look by look (fewer
characters, jumpier, even for fast ones alone); a thing without its own speed also matched where the camera moves
things (the median speed of those followed: 10-13% fewer of the model's things without a box in turns, but more
flicker there, and in this clip it boxed the pale shape); held things taking only sure boxes after a while (fewer
false boxes, but fewer characters and jumpier).
**A thing the snapping lost in a turn** (late evening of 2026-11-11; the user, frame 159 of the 22:10 clip: boxes beside
their monsters while the camera turned at 8 px/ms): the snapping had its own speed for a thing once it had snapped it
twice, and a look moved its box but never told it the speed. One lost as the turn sped up (looked for 47 px from
where it was, 34 px around) was then looked for at the speed it had then (1.1 px/ms against the tracker's 7-9) and
never found again; between looks its box slid on too slowly, up to 100 px behind (78 of that clip's 1077 frames). Now
a look that finds one the snapping could not find on its last image puts it there with the tracker's speed and
starts its line again (`SnapRules::merge`). On the 22:37 clip, each set against the model's boxes on the image it was
snapped onto (lead taken back): boxes on nothing 552 -> 519, the model's things without a box 550 -> 506, fast boxes
on their things 817 -> 881 at 2.4 -> 2.7 px off; in its turns held boxes 2 px/ms or more off the others 220 -> 148.
Not judged on the older clips: their videos lack images (below). The rest of that moment is the tracker's: small
monsters (18-22 px) jumped farther than their own size between looks as the turn sped up, so it lost them or swapped
their ids, and held the lost ones where they were for up to 150 ms (their speed was still slow, so not
`kFastHoldMs`); the scene's shift below takes some of that.
**A camera jump** (night of 2026-11-11; the user, frame 178 of the 22:37 clip: "huge offset on boxes"): the game stood
still 27 ms, then the scene was 76 and then 183 pixels on in two images (the player's own character, the camera's
pivot, stayed; a large monster close to the camera went the other way). The tracker matches each thing near where it
is heading (its larger side), so the small monsters were all lost and followed again as new ones, and the snapping,
looking 20 px around, lost them too: on frame 178 their boxes were 114-250 px off, held where they had been. Two
changes, both in the heuristics DLL:
- The tracker pairs by the scene's shift (`Tracker::sceneShift`): when things not found near where they were heading
  have boxes about as far off as each other (`kShiftNear` of their side; at least `kShiftSupport`, and at least half of
  those just lost), each is also paired where that shift takes it, and one paired so over `kJumpPixels` away starts its
  line again. The shift is the mean of theirs weighted by how small they are; things that stayed where they were
  heading (the player's own character) do not count. Its tolerance first grew with the shift (a quarter of it): in the
  22:10 clip's crowds three lost things then paired others with boxes across the screen (speeds of 150 px/ms). On the
  five clips of 2026-11-11, Today not snapped against the model's boxes on each frame's own image (boxes on nothing,
  the model's things without a box): 22:37 996 -> 915, 532 -> 362; 22:07 464 -> 446, 109 -> 84; 19:46 1908 -> 1884,
  793 -> 569; 12:10 456 -> 440, 137 -> 128; 22:10 1683 -> 1762, 1035 -> 746. On the 31 live logs (`--track`) characters
  shown 10505 -> 10518, boxes on nothing 2040 -> 2048, coming or going 3.710 -> 3.668 a second.
- The snapping looks wide after a jump (`SnapRules`, `kJumpReach` 250 px): when most of the things it expected to find
  (and at least 3) were lost on one image at once, it looks for them that far around on the next one, takes the finds
  only where at least `kJumpSupport` (2) say alike how far the scene went (within their side and `kJumpShare` of it:
  over 200 px the monsters' shifts spread by 25 px), looks for the others near there, and holds the ones not found
  there; a wide find starts its line again (one with no line of two yet got 34 px/ms through the jump). On the 22:37
  clip it looked wide on 14 of 1795 images. Looked for 250 px around, two of the five small monsters were found on
  lookalikes and one not at all (motion blur lowered real matches to 0.65-0.9), hence the vote.
On the 22:37 clip, each set on its own image (`--today-out`, lead taken back), against the snapping fix before: boxes on
nothing 519 -> 562, the model's things without a box 506 -> 379, fast boxes on their things 881 -> 989 at 2.7 -> 2.6
px off (nine in ten under 5.1 -> 4.3, middles outside 3.7 -> 3.1%), held boxes in its turns 2 px/ms or more off the
others 148 -> 100. Frame 178 now: four small monsters' boxes 1-5 px off, the fifth 24 px (held where the jump took it;
not found, blurred), the far one at the screen's edge as before. Frame 177 still shows the image before's boxes (the
jump came between the two images; nothing could foretell it). Still wrong there: the large monster at the bottom left,
behind the see-through chat window: its box is mostly the window, which does not move, so the snapping "finds" it in
place (0.78, where its real matches were 0.89-0.96) while it went 133 px left; the next look puts it right (frame 180).
That needs the snapping to leave out what does not move (the chat window), not done. On the 150-a-second clips the
snapped replay counts more boxes on nothing (22:10 1377 -> 1694; their videos lack images, above).
**The game limited to 120 images a second** (the user, 22:37 clip, NVIDIA Control Panel Max Frame Rate 120, global,
on a 240 Hz screen): a look 8.2 -> 4.6 ms (the GPU no longer draws 150 game images a second beside the model; 4.5 ms
on the bench without a game), no image waits for the look before (3.5 -> 0 ms), image to its boxes 11.8 -> 4.6 ms,
every image looked at (looks 118 -> 119 a second), and 95% of the images 8.3 ms apart (before: 49% 4.2, 40% 8.3, 11%
12.5). Its clips' videos hold 117 of the 119 images a second (at 150 a second they held 107: the clip recorder takes
about 4.3 ms a frame). That matters for judging a rule change on a clip: a search not recorded live searches the
video, and for an image the video lacks the frame before stands in (`SnapImages`), so a thing is "found" at the
same place on two images, its speed 0, and stuck (the snapping fix above, on the 22:10 clip: more boxes on nothing,
1310 -> 1377). So judge changes on clips recorded at 120. The clip screen shows each frame as its image came, when the
boxes of the image before are still up (its own come about 2 ms later; at 120, 99% of the frames), so `--clip-guesses`'
precision there counts those; Today dumped by `--today-out` can be judged on each set's own image instead.
The grey images (`engine/GreyMaker`) are made on the GPU, from the channel's image itself (copying it cost the model
0.15 ms a look), and the last 8 stay there; only the parts the snapping says it reads (`Snapping::learnAreas`,
`snapAreas`) come back: 7.3% of each image on that clip, 350 KB. Reading whole images back (4.9 MB) slowed the
model's looks from 3.2 to 5.1 ms, as the copies hold up the GPU it runs on (the shaders cost it nothing; a lower GPU
thread priority did not help; through CUDA's copy engine the copies waited behind the model's whole look, so the
boxes came 7 ms late); the parts: 3.2 -> 3.9 ms. Live (bench, parts as large as the clip's): 0.9 ms an image, boxes
there 1.1 ms after the screen showed the image, every image at 240 a second. The bench checks the parts: snapping on
them and on whole images hands out the same boxes (and a unit test turns all else over). Needed ABI 2 (the app
rebuilt). On the user's fight log (17.5 s, `observex_bench --track`): boxes appearing or vanishing 21.1 ->
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
asks "recordings or clips?", clips listed with miniatures like recordings; reviewing a clip first only played it, then came
the second step below). The clip buffer (`engine/ClipRecorder`, see Architecture) always runs while a
screen is observed. A clip is a folder in `<recordings folder>/clips/`, named by when it was saved: `clip.mp4`
(H.264, a frame each time the screen changed, at the time the screen showed it), `clip.txt` (`core/ClipLines` and
`core/LiveLines`, header explains: `frame FRAME IMAGE TIME` ties each video frame to the screen image number the
looks use; `look`/`kinds` as in the live log, down to 10% sure; `shown IMAGE DONE ...` what the app showed after
each look: the tracker's things with id, sureness, kinds, box, speed, held; `snapped IMAGE TIME DONE LEAD ...` the
snapper's boxes on each image, the same fields; after each, since the evening of 2026-11-11, `finds IMAGE LOOK FINDER
...`: which look's things it had taken in, and each search it made on that image (see Today below); since
2026-11-12 `ground IMAGE TIME FOLLOWED KNOWN MEASURED PITCH DISTANCE FOCAL MIDDLEX MIDDLEY YAW FEETX FEETY`: the grid
the Ground flag drew (see Ground); since 2026-11-12 `depth IMAGE TIME MAP`: the depth map the Depth flag drew, kept in
`depth.bin` beside it (see Depth); since the night of 2026-11-15 `drops IMAGE DONE KILL X Y SIDE [SURE X Y W H]...`:
what Show > Drops draws (see Drops); `note TIME TEXT`; since 2026-11-13 `log TIME WHEN  TEXT`: a line of observex's
log, see Log; since 2026-11-15 `cost TIME PART EACH SLOWEST PERSECOND [STEP EACH SLOWEST PERSECOND]...`: the Debug
tab's numbers once a second, see Debug), `picture.jpg` (the
screen at the save, the list's miniature) and `clip.observex` (`ClipInfo`: saved, seconds, frames, looks, screen,
size, model). `observex_bench --track <clip>/clip.txt` replays its looks through the tracker as for a live log.
In the app: **Save last 10 s** (top bar, left of Record; Ctrl+Alt+C did it too until 2026-11-12; its tooltip is one
sentence) saves what the buffer holds, then says "Saved 10.3 s" for a moment; it is greyed out (with why, on
hover) until the buffer holds frames or while a save is written. **Review** first asks "What do we review?":
recordings (as before), Handmade (the Shift+Win screenshots, below) or clips; the clips list ("Play a clip") shows each clip's `picture.jpg` with when, seconds,
frames, looks and screen, newest first. A clip opens on the clip screen (`app/ClipView`, the frame as large as fits,
framed like the observed screen) instead of the observed screen; the top bar then has play/pause (or Space), the
time ("0:05.9 / 0:10.1"), the frame number (the `frame` lines' numbers) and Close clip. It plays the frames at their
own times (`engine/ClipPlayer`: GPU decode through Media Foundation's source reader, with its frame-rate conversion
turned off, `MF_XVP_DISABLE_FRC`, or it repeats and skips frames of a video whose frame rate varies; the CPU decoder
if the GPU's fails; paced by a waitable timer). Opening one takes about 0.1 s on the UI thread. The app's own notes go
into `clip.txt` as `note` lines: clip saved, save last 10 s, recording started/stopped, mark (until 2026-11-12),
review and clip opened/closed, kind update made, show Characters/Kinds on/off. The clip buffer pauses while a review
or a clip is open (with recognition) and repeats the last image meanwhile. Checked 2026-11-11: every decoded frame's time
matches its `frame` line within 0.06 ms; colours within about 0.7 of 255 per channel of `picture.jpg`.
**Clip screen, second step (2026-11-11, the user asked: switch while reviewing between the guesses that came with the
clip, exactly the boxes and mistakes seen while playing, and what changed heuristics make of the same seconds; a
slider and a frame back/on, for the later rewind debugger; an X to remove a clip saved by mistake; a recording's
size).** The heuristics (what the recogniser does with the model's things) have a fixed side, `core/Heuristics` (an
interface: the model's `Detection`s in, `Tracked` things out), and the rules themselves in `src/heuristics` (`Rules`:
keep those at least 30% sure, then the `Tracker`), used by `engine/Recognizer` live and by the replay. **They reload
while the app runs** (the user asked, 2026-11-11: to see a heuristics fix on an open clip by picking Today, without
closing the app): `src/heuristics` is built on its own as `observex_heuristics.dll` (next to the exe; the Release one
in the project folder with `observex.exe`), which `engine/HeuristicsLibrary` loads from a copy in
`%TEMP%\observex-heuristics` (so the build can overwrite the file), looks at every 250 ms and loads again once it
stayed unchanged 400 ms. Then the recogniser makes new heuristics between two looks (the tracker starts over), an
open clip's Today is made again from its recorded looks (milliseconds), the clip screen says "Today's heuristics
loaded again: observex_heuristics.dll, built 13:05:22" for 4 s, and a note goes into the clip buffer. The Guesses
popup says (in red) why the newest DLL did not load if it did not. **To change the
heuristics while the user has the app open: edit `src/heuristics`, then build only
`cmake --build build/vs2022 --config Release --target observex_heuristics`** (under a second for one file; the exe is
locked while it runs and is not relinked). A change to what they hand each other (`core/Heuristics`, `Detection`,
`Tracked`, `heuristics/Abi`) needs the app rebuilt and `kObservexHeuristicsAbi` bumped: the app refuses a DLL whose
ABI or struct sizes differ and keeps the ones it has. Without the DLL, or while it does not load, the app uses the
rules built into it (`observex_rules`, as of its own build). Checked 2026-11-11 in hidden snapshots: rebuilding only
the DLL with a test rule (only boxes 85% sure and up; 0.8 s to build) while a clip was open on Today changed its
boxes by itself, and back again once the rule was restored; the built-in and DLL heuristics replay a clip the same.
Following the ground reloads too (ABI 4, 2026-11-12, the user asked: changes to the grid without closing the app;
ABI 5 that evening for the cone between follows, 6 for the screen's turn there, 7 for the depth network's maps):
the DLL makes `heuristics/GroundFollower` (core/Ground's `Grounding`; `GroundMotion`, `MinimapCone` and core/Ground's
`GroundFit` are built into the DLL with it), the live ground finder makes a new one at its next image (the camera is
found again from the start), and an open clip whose ground was followed follows it again with the new one. Its plain
structs (`GroundCamera`, `GroundHints`, `GroundNote`) cross the ABI like `Detection` and are checked the same way.
Checked 2026-11-12: the bench through the DLL follows the 3 clips frame for frame as the built-in; and in a hidden
snapshot with a clip open and Ground on, rebuilding only the DLL with a test change (the camera's yaw 0.8 radians
more; 2 s to build) turned the clip's grid by itself, and the restored DLL follows as before. The live finder's reload
is the same version check as the snapper's; not tried on a live screen. `core/ClipGuesses` reads a
whole `clip.txt` back (`readClipText`) and makes the guessers: **Recorded** (`recordedGuesses`, the `shown` lines,
then from the first `snapped` line's done time on the `snapped` ones: what the app showed) and **Today** (`heuristicGuesses`: the recorded looks through today's `core/Heuristics`, each
set shown from when the recorded look's was, so only the heuristics differ; then snapped, see above). At a frame each shows its last set from
at or before the frame's time. On the clip screen the **Guesses** button (top bar, right of the screen button; it says
Recorded, Today or Hidden) opens a popup with the live view's flags: Characters (key 3 turns it off and on; the user
asked, 2026-11-12: before, a third guesser "Nothing" did that and there was no Characters flag) and under it the two
guessers, one sentence each (keys 1 Recorded, 2 Today, which also turn Characters on; since 2026-11-12, the user asked: before,
longer text and their numbers), and the Kinds, UI, Ground, Depth and Drops checkboxes (the live view's flags, saved with it; Drops since 2026-11-15, see Drops; UI draws what the clip's own
UI finder found on the frame shown, or one at most 0.1 s before it while playing); boxes and tags are drawn by `app/Overlay`
exactly as while observing. Below the frame a timeline (click or drag to any frame; it pauses while dragging, ticks
for the notes with their text on hover), and in the top bar the first frame, a frame back, play/pause, a frame on and the
last (also Home, Left/Right held to repeat, End; first and last asked for later the same day), the time, and the
frame number in a field to type another into (Enter goes there, Escape leaves it, Ctrl+G types into it; the user
asked the same day); in a narrow window the word "Frame", then "of 1,066", some space, the clip's length and last the time make room. `ClipPlayer::seek` (engine): the next frame comes within a millisecond (the decoder has it),
any other is decoded from the key frame before it (about 6 ms for the key frame and 1.3 ms for each frame after it at
1440p): 4-5 ms in clips saved since 2026-11-11 12:20 (a key frame every 0.1 s), 40 ms on average and 90 at most in
older ones (every 0.5 s). A seek finishes the one in progress, then goes to the newest one asked for (sooner, if that
frame is on its way), so a timeline drag shows the exact frame under the mouse as fast as frames decode, both ways
(`observex_bench --clip-seek`: 40-50 pictures a second in new clips, 20-40 in old ones). The user asked, 2026-11-11,
for a fluent drag (seeing an effect while dragging matters): first a new seek cancelled the one in progress, so a
drag back showed nothing until it stopped; then it showed only key frames while dragging, which was not fluent
enough. The knob follows the mouse. Steps go from the frame shown, so a held key waits for each. The clips list has an X on each clip (red on hover): it asks first ("Remove this clip?" with when, seconds,
frames and size; Remove in red, Cancel the default, Escape cancels; the user asked the same day), then moves the clip's
folder to the Recycle Bin (`SHFileOperationW`, `FOF_ALLOWUNDO`), closing it first if open. A recording's row shows its size
(its images summed, "5.1 MB"); a clip's row shows its size on a second line, with its screen ("81.2 MB · Odyssey
G61SH": its folder's files summed; the user asked, 2026-11-11, once clips got bigger). **For Today to equal Recorded when nothing changed** (checked 2026-11-11 with
`observex_bench --clip-guesses` on the 11:02 clip's looks rewritten the new way: 100% of the sets drawn the same at
every start tried), clips since then write every number exactly (`ExactNumber`, the shortest text that reads back
as the same float or double: before, sureness had 2 decimals, which the tracker's smoothing turned into 1% off on
1 tag in 10, boxes whole pixels and times 1 decimal) and keep the looks from `kClipWarmUpSeconds` (5 s) before the
first frame, at negative times (a replay from nothing at the first frame drew some boxes differently for up to 3.7 s
on that fight); since the evening of 2026-11-11 the `snapped` and `finds` lines too. Older clips: Today differs from Recorded on most sets (by a percent or a "?" on a box or two, from
that rounding and the cold start), so compare guessers on clips saved from now on.
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
  hands `PipelineStats` to the UI. Also into the clip `ImageChannel` while observing, the snap one while the
  snapper runs, and the ui one ten times a second while the UI finder runs.
- **Recogniser thread** (`engine/Recognizer`, above-normal priority, while a screen is observed): waits on the
  recognition channel's event, copies the newest image into the `Detector`'s input texture and runs it: a compute
  shader squeezes the image to the model's size (1024 x 1024) into a buffer shared with CUDA, TensorRT runs the network,
  and only the candidate boxes come back. The network and the copies back run as one CUDA graph, recorded on the
  first look (one launch instead of hundreds: 5.56 -> 4.68 ms per image on recording files, same guesses; the
  Finder gets it too). TensorRT's highest builder optimisation level (5) was tried: slower (4.86 ms) and 141 s to
  prepare, so the default stays. Those at least 30% sure go through the `Tracker` (timed by when the
  screen showed the image). Hands `Found` (what it follows, surest first) and `RecognizerStats` to the UI, and each
  look's things to the snapper.
- **Snapper thread** (`engine/BoxSnapper`, highest priority, while the recogniser runs): on each new image of the
  snap channel (used in place, `ImageReader::useNewImage`) makes its grey levels on the GPU (`GreyMaker`), reads back
  the parts the snapping needs of it and of the newest look's image, learns from the look and snaps the boxes onto the
  image (core/Heuristics' `Snapping`, from observex_heuristics.dll, made again when it reloads). Hands `Found` and
  `SnapperStats` (images a second, ms each, how late, share snapped) to the UI and each image's boxes to the clip
  buffer.
  TensorRT is loaded at run time from `deps/` (no import libraries); the engine for this GPU is built once per new
  model.onnx and cached as `model.engine` (one Detector loads or builds at a time; a second waits, then loads it).
  Paused while reviewing (the pipeline stops handing it images). While recording it also writes each look into the
  live log (`setLog`), down to 10% sure. When the UI sets its reload flag (`reloadKinds`), it reads the kind update
  at the top of its next look and hands it to its Detector.
- **UI finder thread** (`engine/UiFinder`, below-normal priority, while a screen is observed, paused with the
  recogniser; a second one with an open clip, on its frames): on each image of the ui channel (used in place) makes its grey levels on the GPU (`GreyMaker`, one
  kept), reads back only the full-size one and finds the game's interface on it (`core/UiLayout`). Hands `UiPieces`
  and `UiFinderStats` (images a second, ms each, finding ms, why ui.txt could not be read) to the UI; checks once a
  second whether `models/ui` changed and reads it again.
- **Drop finder thread** (`engine/DropFinder`, below-normal priority, made in `observe` when `models/drop` has a
  model): its own GPU device and `Detector`; on the newest image of the drops channel (20 a second while the screen
  is observed) runs the drop model on one square: the sweep's next (`lootTiles`), or, every other image while
  `core/LootWatch` has a spot, that spot's. Hands `FoundDrops` (the newest close and sweep looks, screen pixels) and
  `DropFinderStats` to the UI.
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
  video keeps to its level (a 240 Hz screen gives about 120 frames a second: every other image). No B-frames, a key frame at least every 0.1 s (`kKeyEvery`, since
  2026-11-11 for the clip screen's timeline drag; was 0.5 s) and a still screen's last image repeated as one every
  0.5 s (`kRepeatEvery`), constant quality 22 within 120 Mbit/s, BT.709 limited range. It keeps the
  compressed frames, cut at key frames, back to the last key frame at or before 10 s ago, plus every look the
  recogniser hands it (`Recognizer::setClip`: what it found down to 10% sure and what the tracker followed, through a
  fixed 64-look queue that never waits; drops are counted) and the UI's notes. `save` hands a copy of all that (the
  frames shared, not copied) to a writer thread: `clip.mp4` (Media Foundation's sink writer, the frames as they are,
  no second encode; MP4 timescale 144000, so the frame times stay exact), `clip.txt`, `picture.jpg` (the newest
  image) and `clip.observex` last. A clip is 10.0 to 10.1 s (up to 10.5 on a still screen). Measured 2026-11-11 (Release bench, 2560 x 1440 at 240
  Hz, pulse, `--model models/character`, 4 runs each with and without `--clip`): 120 frames a second at about 4.3 ms
  each, capture unchanged (239.8 images/s), the recogniser 3.28 -> 3.57 ms a look (239 -> 238 a second), a save takes
  under 0.1 s; the plain desktop makes about 7 MB a clip (with a key frame
  every 0.5 s; every 0.1 s makes two 10 s Lineage 2 clips about 25% and 60% bigger, 93 -> 115 MB and 50 -> 80 MB, from
  their frames' sizes: a key frame is about 400 KB, the others 40-85 KB). (Before the gap rule it copied every image, 240 a second,
  and encoded 150-160 frames a second, over the level: 0.8 ms more a look.)
- **Mouse watch thread** (`engine/MouseWatch`, with the clip buffer; since the evening of 2026-11-15): Raw Input's
  every mouse report, handed to the clip buffer under its lock and to the camera by the counts (`App::mouseCamera_`,
  `core/MouseCamera`'s `SharedMouseCamera`, under its own; see Ground: the mouse).
- **UI thread** (`app/App`, the main thread): `MainWindow` (Win32 window, swap chain), ImGui, `Theme`
  (fonts, colours, logo, the boxes and tags drawn over screen images, shared by the review and the live view), `Widgets` (`BarButton`, the top-bar button look; since 2026-11-12 an open one, its popup up or recording, lights up
  under the mouse too, so a click reads as closing it), `ScreenPicker` (the miniatures),
  `ScreenImage` (a screen image the UI can draw at any size: it keeps a mip chain so shrunken views look smooth, and
  draws opaque), `Thumbnails` (the Review list's pictures), `Settings` (observex.cfg, folder picker) and `Review` (the review screen; it reads each image with
  `engine/ImageFile` on the UI thread, 11 to 17 ms for a 2560 x 1440 JPEG, and keeps marks in `core/Marks`).

The threads share data only through lock-free hand-overs, so the UI and the recorder can never slow the pipeline:

- `TripleBuffer<T>` (core): newest-value hand-over for stats.
- `ImageChannel` / `ImageReader` (engine): three shared GPU textures rotated by a TripleBuffer; keyed mutexes order
  the two devices' GPU work. The reader copies each new image out right away and hands the texture straight back.
  One channel for the UI preview, one for the recorder, one for the recogniser, one for the clip buffer, one for
  the snapper and one for the UI finder (their readers also wait on an event, so they sleep until an image arrives;
  the snapper's and the UI finder's read their image in place).
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
written), so the clip buffer starts empty on each pipeline start. So do the snapper and the `UiFinder` (made in
`observe`, destroyed in `stopObserving` before the pipeline stops). The clip screen's `UiFinder` goes with its
`ClipView`, before the channel it reads (on the UI's device, written on the UI thread).

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
- Release builds keep their PDBs (observex.pdb and the DLLs' next to them in the project folder) for the crash log
  (`engine/CrashLog`); `observex_bench --crash FOLDER [uncaught]` checks it.
- Bench: `build/vs2022/bin/Release/observex_bench.exe [--screen N] [--seconds S] [--no-pulse] [--no-preview]
  [--record FOLDER [--every S] [--unsure]] [--model FOLDER] [--clip FOLDER]`. Lists the screens and measures one (the
  primary by default). `--clip` runs the clip buffer as the app does, prints its numbers (encoded/s, encode ms, seconds
  and MB held, dropped looks), notes halfway, and at the end saves a clip into a new folder in FOLDER (a scratch
  folder: it shows the screen) and says what it wrote; `observex_bench --track FOLDER/clip.txt` replays its looks.
  `observex_bench --clip-frames CLIP --out FOLDER [--every N]` decodes a clip as the player does and writes
  `frames.txt` ("FRAME TIME_MS" per frame) and a JPEG of frame 1, every Nth and the last, to look at a clip's frames
  (1.7 ms a frame on the GPU at 1440p). `observex_bench --clip-seek CLIP` checks the clip player's seek on a clip (every
  frame's number, pixels and time; frame 1, last, random, +1/-1 steps, keyframes, 50 in a row, drags across the clip
  as the timeline does them, play after a seek, seeks while playing and after the end) and prints how long each kind
  took. `observex_bench --clip-guesses CLIP [--heuristics DLL]` (Today from that observex_heuristics.dll, loaded as
  the app loads it, instead of the bench's own) compares the
  clip screen's guessers on a clip box by box (Recorded against Today: sets drawn the same, boxes only one draws, tags
  that differ, boxes moved, when they differ), the way to check a heuristics change or that nothing changed, and
  how precise each guesser's boxes are (`core/ClipGuesses` boxPrecision, 2026-11-11): at each frame whose own image
  was looked at, its boxes against what the model found on that image (50% and up; paired by image, as a frame's
  time can be a moment after its image's): middles off on average and for nine in ten, in the model's box's middle
  half or outside it, for all boxes and those moving 0.8 px/ms or faster, boxes on nothing, and how many of the
model's things have no box. For a clip with `snapped` lines (or with `--snap`) also Today snapped as the clip screen
does (`engine/ClipSnapper`), against Recorded and how precise; with `--today-out FILE` (2026-11-11) those sets written
  into FILE as `shown` lines (the image each was snapped onto, 0 before the first) and after each snapped one a
  `finds` line of its searches, for the same scripts as a clip's lines; with `--looks-out FILE` Today's own set after
  each look (the tracker's, not snapped) as a `shown` line with the image looked at. No GPU but for that. With
  `--snap` (2026-11-11) it decodes the clip's video and snaps Today's boxes onto each frame as the snapper does live
  (the grey images on the GPU, only the parts read back; the looks learned once done, from their own frames): whether
  the GPU's grey images are the CPU's, how much was read back and whether snapping on whole images hands out the same
  boxes (it must), how many snapped and how long a frame took, the snapped boxes at once and coming or going a
  second (as for Recorded and Today), then the same precision for the snapped boxes on
  their own frame and, as handed out, on the screen's next image with leads of 0 to 12 ms and "to next";
  `--out FOLDER --at FRAME` (repeatable) writes those frames as PNGs around the boxes, Today's red, snapped green
  (to look at with your own eyes). Live, `--model FOLDER --snap` runs the snapper beside the recogniser as the app
  does and adds its numbers (snapped/s, ms an image, how late, share snapped); on the desktop nothing is snapped, so
  nothing is read back.
  `--unsure` records like Record's "Only unsure" (needs `--model`; its answers are the log's `mirror`
  lines).
  Its "pulse" is an invisible 2 x 2 pixel, click-through, never-focused window in the screen's corner that presents
  every refresh like a game, so the screen has something to capture. `--record` saves images like the Record button
  does (into a scratch folder, never the repo), so its cost shows in the numbers; with `--model` too it writes the
  live log like the app and marks once halfway (the way to test the log headless). `--model models/character` runs the
  recogniser too and prints its numbers (recognised/s, ms per image, found; with `--snap` the snapper's too). `observex_bench --model FOLDER --images
  FOLDER --out FILE` runs the C++ detector over a folder of JPEGs and writes its guesses in the marks format, kinds
  included (to compare with Python's `guess.py`), and each image's mirror test to `FILE.mirror.txt` (prints the median and how
  many Only unsure would save; for a kinds model also how many are unsure which kind something is, and the total). `observex_bench --track LOG` replays a live log (a recording's
  `character-live.txt`) through the C++ tracker and prints boxes appearing or vanishing per second with and without
  it, things followed, and its cost per look; for a recording with answers (2026-11-11), at each answered image the
  boxes handed out right after the look at it against the characters marked (overlapping 30% and up): characters
  shown (and by the model alone, 60% and up) and boxes on nothing marked, the way to judge a tracker change on the
  user's answers; for a log with `kinds` lines also how often a thing's kind changes per
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
  `observex_bench --ui CLIP [--layout FOLDER] [--out FOLDER [--every N] [--at FRAME]...]` (2026-11-12) finds the
  game's interface on every frame of a clip as the UI finder does (grey made on the CPU; layout default `models/ui`),
  prints how long a find takes, how steady the pieces are (one appearing, going or moving 4+ pixels from the frame
  before; 0 a second on a still clip) and on how many frames each name is, and with `--out` writes `ui-found.txt`
  (every frame's pieces) and JPEGs of frame 1, every Nth, the last and each `--at` with them outlined (framed windows
  yellow, ui.txt's pieces blue), plus where each anchor hit on the `--at` frames. Use it after any change to ui.txt
  or `core/UiLayout`. A picture (PNG, JPG, BMP) in place of the clip finds on it alone (`--out`: `ui-picture.jpg`);
  for overlaps, paste a window cut from a frame (with 3 pixels of its outer shadow: frame lines need darker pixels 2
  away) over another frame. The game's own screenshots (`Screenshot/Shot*.bmp`) are darker than the screen captured
  (frame lines 111-124 grey against 194-253), so their framed windows are not found: use clip frames. Frames decoded
  by OpenCV differ slightly from the GPU's (one more false small window on the hotbar). `--find-ui` in the live run adds the UI finder as the app runs it (ui/s, ms, finding ms,
  pieces); with `--model` it shows its cost to the recogniser.
  `observex_bench --geodata CLIP --out FOLDER` (2026-11-15) follows a clip as the Geodata flag does live (ground
  follower, UI finder, minimap slide, /loc reader, `WorldPosition`, the grid on `geoCamera`'s pitch with the UI left
  clear; `models/geodata`) and writes a JPEG every 100 ms with the grid drawn, then how many images had the minimap
  followed and the position sure, and the ms a minimap and a messages read take.
- The app: `observex.exe` in the project folder. The Release build writes it there (gitignored). The Debug build
  stays in `build/vs2022/bin/Debug/`.
- UI snapshot: `observex.exe --snapshot <file.png> [--click X Y | --drag X1 Y1 X2 Y2 | --hold X1 Y1 X2 Y2 |
  --right-drag X1 Y1 X2 Y2 | --scroll X Y TURNS | --key [Ctrl+|Shift+]NAME | --type TEXT]... [--mouse X Y]
  [--recordings FOLDER] [--wait SECONDS]` (`--type` types text into the field with the keyboard, e.g. a frame number
  after `--key Ctrl+G`; `--hold` is a drag never let go of, to see the middle of one: give `--mouse X2 Y2` with it). Draws the UI in a
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
  show the user's screens. Snapshot mode does not read or write observex.cfg,
  records into `recordings/` next to the PNG, and never makes the kind update (it would write into `models/`); the
  refresher does run there, so `--recordings` must be a copy.

## Layout

- `src/core/`: platform-free building blocks (`TripleBuffer`, `StatWindow`, `Costs`: the Debug tab's meters and total, `Marks`: boxes, answers, the marks file
  format, carrying answers forward, which guesses start as yes, matching guesses to answers for the kind update;
  `ReviewHistory`: the review's Ctrl+Z; `ImageView`: the review's zoom; `BoxLearner`: nudging the review's guesses' boxes the way the user boxes them;
  `KindLearner`: re-fitting the kind classifier to the review's answers and for
  the kind update, and picking kinds with it; `Features`: the `<category>-features.bin` format and when it is fresh;
  `ModelInfo`: model.json's kinds and kind classifier, and `kinds-update.json`; `MouseCamera`: the camera's yaw and
  pitch by the mouse's counts and their anchors; `LiveLines`: the live log's `look`
  and `kinds` lines, and `ExactNumber`; `ClipLines`: a clip's `frame`, `shown`, `snapped`, `finds`, `ground`, `depth`, `cost` and `note` lines; `ClipGuesses`: a
  whole `clip.txt` read back, the clip screen's guessers (Recorded, Today) and comparing them; `Heuristics`: the
  heuristics' fixed side (the model's things in, the things shown out); `ReviewLines`: the review log's words;
  `Recordings`: the `recording.observex` file, listing recordings and their images, and `clip.observex` and listing
  clips; `Detection`: a found thing, and a `Tracked` one; `UiLayout`: where the game's interface is on a grey screen
  image, framed windows and ui.txt's pieces, and PGM files; `Ground`: the game's camera, the ground's projection and
  `GroundFit`, the camera fitted to the ground's motion; `Depth`: shading the depth network's map, `Depthing`;
  `StateTree`: the bot's brain, a tree of named states; `Tiles`: where the tiles under the tabs go; `Log`: observex's
  log, its file and ring buffer; `XpWatch`: XP got, from the stats readings; `TargetWatch`: which box the target is and its death
  (the dead mark); `LootWatch`: which spot to look at closely for drops (a kill's, or drops the sweep found) and when to stop, the square rules, the sweep's and review's tiles; `ShotKey`: Shift+Win; `GameStats`: the
  numbers
  read from the game's interface, `StatsReading`, and a clip's `clip-stats.txt` lines).
  No Windows or GPU headers. Unit tested.
- `src/heuristics/`: the heuristics, reloaded while the app runs (`Rules`, `Tracker`: following found things from
  look to look; `SnapRules`: the boxes snapped onto every new image, by `Snapper`: finding a thing's look on a
  newer image, and `GreyImage`: grey levels on the CPU, for the tests and the bench; `GroundMotion`: how the ground
  moved between two images, `GroundFollower`, the camera followed over images, `TurnPitch` and `TurnHold`: its pitch
  by its turns, and `MinimapCone`: where the camera looks, from the minimap; `DepthRules`: the depth shaded; `Abi` and `Plugin`: the DLL's plain C entry, and
  `DepthAbi` and `DepthPlugin` its depth entry; `StatsReader`: the game's bars and numbers read, `StatsAbi` and
  `StatsPlugin`: observex_stats.dll's entry). Built as `observex_rules` (the tests, the bench, the app's fallback),
  `observex_heuristics.dll` and `observex_stats.dll`.
  No Windows or GPU headers. Unit tested (the tracker, the snapper, the snap rules, the ground, the minimap's cone, the
  depth's shading).
- `src/engine/`: the real-time pipeline, the recorder, the recogniser and the clip buffer (`Gpu`, `ScreenCapture`,
  `ImageChannel`, `Pipeline`, `Recorder`, `ImageFile`, `Detector`, `Recognizer`, `Finder`, `Refresher`, `KindUpdater`,
  `ClipRecorder`, `MouseWatch`: the mouse's raw counts, `ClipPlayer`, `HeuristicsLibrary`: loading
  observex_heuristics.dll and loading it again,
  `StatsLibrary`: the same for observex_stats.dll,
  `BoxSnapper`: the snapper thread, `GreyMaker`: grey levels on the GPU, parts read back, `ClipSnapper`: a clip's
  Today snapped onto its video, `UiFinder`: the UI finder thread, `DropFinder`: the drop finder thread, `GroundFinder`: the ground finder thread,
  `ClipGround`: a clip's ground followed on its video, `ClipDrops`: today's drop model on a clip's frames shown,
  `TensorRt`: loading a TensorRT engine and CUDA graphs, `CrashLog`: a crash's call chain into observex.log,
  `DepthNet`: the depth network, `DepthFinder`: the depth finder thread, `DepthStore`: depth maps packed and
  `depth.bin`). Windows, Direct3D 11,
  CUDA, TensorRT, NVENC and Media Foundation, no ImGui.
- `src/app/`: the window and UI (`App`, `MainWindow`, `Theme`, `Widgets`, `ScreenPicker`, `ScreenImage`, `Thumbnails`,
  `Settings`, `Review`, `ReviewLog`, `ClipView`, `Overlay`: boxes and tags over the screen or a clip, `StateTreeView`:
  the State tile, `StatsPanel`: the Stats tile, `LogView`: the Log tile, `DebugView`: the Debug tile, `Snapshot`, `main`), plus
  `observex.rc.in` and `Resources.h` (icon, fonts and version info built into the exe).
- `bench/`: `observex_bench`, the headless pipeline benchmark (`DepthBench`: its `--depth`; `StatsBench`: its
  `--stats PATH... [--layout FOLDER] [--list]`, the UI finder's pieces and the stats reader on recorded JPEGs or PNGs
  and folders of them: per bar how often its piece was found, its bar hidden, its number kept, doubted or not read,
  with examples, and the ms a read; run it after any change to stats.txt or `heuristics/StatsReader`).
- `tools/ui/make_digits.py`: cuts the stats reader's glyphs (`digits.pgm`, `digits-mask.pgm`, `digits.png` to look
  at) from `models/ui/stats.txt`'s `from` lines (run with `tools/train/.venv/Scripts/python.exe -I`).
- `tools/ui/make_anchors.py`: cuts ui.txt's anchors from clips (run with `tools/train/.venv/Scripts/python.exe`): for
  each `anchor` line, the mean of its frames at each place named and the mask of the pixels that never changed, as
  PGMs in `models/ui`; `away X0 Y0 X1 Y1` leaves a rectangle out of the mask, `keep LOW HIGH` keeps only pixels at
  most LOW or at least HIGH (letters over a bar); `--only NAME...` cuts just those.
- `assets/`: the logo (`logo.svg`, `logo.png`) and the app icon (`observex.ico`), all made by `tools/make_logo.py`.
  Change the logo there and rerun it.
- `tools/train/`: the training tool (Python: `marks.py` reads and writes the marks format of `core/Marks.h`,
  `train.py`, `guess.py`, `export.py`, `loot.py`: the drops' squares). The trained models go to `models/` (gitignored): `model.pth` (Python),
  `model.onnx` (live recognition; also each candidate's features), `model.json` (its kinds, score and kind
  classifier), `model.engine` (made by observex for this GPU) and `kinds-update.json` (made by observex, the kind
  update).
- `tests/`: doctest tests for `src/core` and `src/heuristics`. Add tests with any change to either.
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
