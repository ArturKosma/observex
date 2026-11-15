#pragma once

#include "core/Costs.h"
#include "core/Detection.h"
#include "core/Ground.h"
#include "core/Heuristics.h"
#include "core/Log.h"
#include "core/LootWatch.h"
#include "core/StateTree.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// The lines of a clip's clip.txt (engine/ClipRecorder) besides the live log's look and kinds lines (core/LiveLines),
// so the clip can be read back line by line, e.g. by observex_bench --track (which skips the lines it does not know).
// Times are milliseconds since the clip's first video frame. Every number is written exactly (core/LiveLines'
// ExactNumber), so a clip reads back as exactly what the app had live (since 2026-11-11; clips before rounded
// sureness to 2 decimals, boxes to whole pixels and times to 1 decimal).
//   frame FRAME IMAGE TIME   video frame FRAME of clip.mp4 (from 1) is screen image number IMAGE, which the screen
//                            showed at TIME. A frame with the image of the frame before repeats it: the screen did not
//                            change for a while (so the video can be cut there).
//   shown IMAGE DONE [ID SURE KIND KINDSURE SECOND SECONDSURE X Y WIDTH HEIGHT SPEEDX SPEEDY HELD]...
//                            what the app showed after its look at image IMAGE (the tracker's things, Tracked), from
//                            DONE on (when the recogniser had it): each thing's id, how sure it is (smoothed), its
//                            kind and how sure of it, its second kind and how sure of that (-1 without kinds), its box
//                            in screen pixels, its speed in pixels per millisecond, and 1 if it was not found on this
//                            look but held where its speed takes it.
//   snapped IMAGE TIME DONE LEAD [the same as shown]...
//                            what the app showed after its snapper put the boxes onto screen image IMAGE (since
//                            2026-11-11, engine/BoxSnapper), which the screen showed at TIME, from DONE on: the same as
//                            shown, each box LEAD milliseconds ahead of where it is on that image along its speed, and
//                            1 for HELD if it was not found on the image (where its speed takes it).
//   finds IMAGE LOOK FINDER [ID LEARNED LX LY LWIDTH LHEIGHT GUESSX GUESSY REACH FOUND X Y MATCH]...
//                            right after its snapped line (since the evening of 2026-11-11): how the snapper got there.
//                            LOOK is the look (its image) it took in just before (0: none), FINDER how it searches
//                            (heuristics/Snapper's kFinder), then each search it made (core/Heuristics' SnapFind): the
//                            thing's id, the image and box it was learned from (0 0 0 0 0: nothing learned), where its
//                            box's top left was looked for and how far, 1 if found and where, and the best match (-2:
//                            nothing to search with). So a replay is exactly what live showed (core/ClipGuesses).
//   ground IMAGE TIME FOLLOWED KNOWN MEASURED PITCH DISTANCE FOCAL MIDDLEX MIDDLEY YAW FEETX FEETY
//                            the grid the Show flag Ground drew over screen image IMAGE, which the screen showed at
//                            TIME (since 2026-11-12, the user asked: a clip's Recorded shows the grid exactly as it was
//                            seen live): the camera drawn (core/Ground's GroundCamera, already moved on to the image:
//                            groundAt), from the ground finder's camera on image FOLLOWED. KNOWN 0: nothing drawn from
//                            then on (the flag off, the camera not known, or the live view not shown).
//   depth IMAGE TIME MAP     the depth map the Show flag Depth drew over screen image IMAGE, which the screen showed at
//                            TIME (since 2026-11-12, the user asked: a clip's Recorded shows the depth exactly as it
//                            was seen live): the depth finder's map of screen image MAP, kept in the clip's depth.bin
//                            (engine/DepthStore) as shown. MAP 0: nothing drawn from then on.
//   drops IMAGE DONE KILL X Y SIDE [SURE X Y WIDTH HEIGHT]...
//                            what the Show flag Drops draws (since 2026-11-15, the user asked: the clip screen's Drops
//                            flag; written whatever the flag says, as the drop finder looks either way), from DONE on
//                            (when the app had it), a line each time it changes: the spot looked at closely, its
//                            square's top left and side in screen pixels (SIDE 0: nothing drawn from then on), KILL 1
//                            round a kill (0: round drops the sweep found), then each drop the newest close look found
//                            there on screen image IMAGE (0: none yet), how sure and its box.
//   brain IMAGE TIME ACTING TASK [STATE]
//                            the brain as the State tile showed it from when the screen showed image IMAGE, at TIME
//                            (since 2026-11-15, the user asked: a clip's review showed no active state or task), a
//                            line each time it changed and one at the clip's start: ACTING 1 while it was active (0:
//                            nothing green), STATE the current state's names below Root with " > " between (none:
//                            Root; core/StateTree's statePath), TASK its task that ran (from 0; -1: all done, or none).
//   camera IMAGE TIME KNOWN YAWKNOWN YAW PITCHKNOWN PITCH BY CONEOFF TURNSOFF
//                            the Stats tab's Camera card from when the screen showed image IMAGE, at TIME (since the
//                            night of 2026-11-15, the user asked: the last 10 s keep the camera too; core/MouseCamera),
//                            a line each time it changed: KNOWN 0 no card (the game's camera mode not seen yet), YAW the
//                            heading in degrees (0 to 360, anchored by the minimap) if YAWKNOWN, else how far it turned,
//                            PITCH in degrees if PITCHKNOWN, BY what anchored it (0 nothing, 1 the ground finder's
//                            guess, 2 the turns, 3 the top limit), and how far off the minimap's cone and the turns said
//                            it was lately (degrees).
//   mouse TIME DX DY HELD BUTTONS WHEEL CURSORX CURSORY
//                            a report of the mouse then (since 2026-11-15, the user asked: the camera turns while the
//                            right button is held, so its counts say how far, to work out how much a count turns it;
//                            engine/MouseWatch): moved DX counts right and DY down as Raw Input has them (before
//                            Windows' pointer speed and acceleration), HELD the buttons down after it (1 left, 2 right,
//                            4 middle), BUTTONS what it pressed or let go (Raw Input's usButtonFlags), WHEEL the wheel
//                            turned (120 a notch, + away from the user), and where the cursor was (screen pixels).
//   cost TIME PART EACH SLOWEST PERSECOND [STEP EACH SLOWEST PERSECOND]...
//                            what PART (a thread of observex) spent over about the second before TIME (since
//                            2026-11-15, the user asked: the Debug tab's numbers, so a clip says what to optimize;
//                            core/Costs): its run on each image it took, on average and at the slowest (ms), and how
//                            many it took a second; then each step of that run likewise. Spaces in names are _. The
//                            lines at one TIME are one look at the Debug tab (written once a second and at a save).
//   note TIME TEXT           something the app did then, e.g. "recording started" or "mark 2".
//   log TIME WHEN  TEXT      a line of observex's log made then (since 2026-11-13, the user asked: the last 10 s keep
//                            the log too; core/Log): its local time and text, as the log file has them.

void appendFrame(std::string& out, unsigned frame, unsigned long long image, double timeMs);
bool readFrame(std::string_view line, unsigned& frame, unsigned long long& image, double& timeMs);

void appendShown(std::string& out, unsigned long long image, double doneMs, std::span<const Tracked> things);
// Replaces `things`.
bool readShown(std::string_view line, unsigned long long& image, double& doneMs, std::vector<Tracked>& things);

void appendSnapped(std::string& out, unsigned long long image, double timeMs, double doneMs, double leadMs,
                   std::span<const Tracked> things);
// Replaces `things`.
bool readSnapped(std::string_view line, unsigned long long& image, double& timeMs, double& doneMs, double& leadMs,
                 std::vector<Tracked>& things);

void appendFinds(std::string& out, unsigned long long image, unsigned long long lookImage,
                 std::span<const SnapFind> finds);
// Replaces `finds`.
bool readFinds(std::string_view line, unsigned long long& image, unsigned long long& lookImage,
               std::vector<SnapFind>& finds);

// The camera's timeMs is not written (it is TIME); read back, it is `timeMs`.
void appendGround(std::string& out, unsigned long long image, double timeMs, unsigned long long followed,
                  const GroundCamera& camera);
bool readGround(std::string_view line, unsigned long long& image, double& timeMs, unsigned long long& followed,
                GroundCamera& camera);

void appendDepth(std::string& out, unsigned long long image, double timeMs, unsigned long long map);
bool readDepth(std::string_view line, unsigned long long& image, double& timeMs, unsigned long long& map);

// Each drop's box and sureness; the rest of a Detection is not written (read back as 0).
void appendDrops(std::string& out, unsigned long long image, double doneMs, bool kill, const LootSquare& square,
                 std::span<const Detection> drops);
// Replaces `drops`.
bool readDrops(std::string_view line, unsigned long long& image, double& doneMs, bool& kill, LootSquare& square,
               std::vector<Detection>& drops);

// Line ends in its state's names become spaces, so it stays one line.
void appendBrain(std::string& out, unsigned long long image, double timeMs, const statetree::BrainShown& brain);
bool readBrain(std::string_view line, unsigned long long& image, double& timeMs, statetree::BrainShown& brain);

// What the Stats tab's Camera card shows: the camera by the mouse's counts on one frame (core/MouseCamera's
// cameraOf), in degrees; a camera line (see above).
struct CameraShown {
    bool known = false; // Not known: no card.
    bool yawKnown = false;
    bool pitchKnown = false;
    int pitchBy = 0;      // MouseCamera::PitchBy: 0 nothing, 1 the ground finder's guess, 2 the turns, 3 the top limit.
    float yaw = 0.0f;     // The heading (0 to 360) if yawKnown, else how far it turned.
    float pitch = 0.0f;   // If pitchKnown.
    float coneOff = 0.0f; // How far off the minimap's cone and the turns said it was lately.
    float turnsOff = 0.0f;
    bool operator==(const CameraShown&) const = default;
};
void appendCamera(std::string& out, unsigned long long image, double timeMs, const CameraShown& camera);
bool readCamera(std::string_view line, unsigned long long& image, double& timeMs, CameraShown& camera);

// One mouse line (see above).
struct ClipMouse {
    double timeMs = 0.0;
    int dx = 0;
    int dy = 0;
    unsigned held = 0;    // 1 left, 2 right, 4 middle.
    unsigned buttons = 0; // Raw Input's usButtonFlags.
    int wheel = 0;
    int cursorX = 0;
    int cursorY = 0;
    bool operator==(const ClipMouse&) const = default;
};
void appendMouse(std::string& out, const ClipMouse& mouse);
bool readMouse(std::string_view line, ClipMouse& mouse);

// One cost line: a part and its steps (steps that did not run that second are left out).
void appendCost(std::string& out, double timeMs, const CostPart& part);
bool readCost(std::string_view line, double& timeMs, CostPart& part);

// Line ends in `text` become spaces, so the note stays one line.
void appendNote(std::string& out, double timeMs, std::string_view text);
bool readNote(std::string_view line, double& timeMs, std::string& text);

void appendLog(std::string& out, double timeMs, const LogLine& line);
bool readLog(std::string_view line, double& timeMs, LogLine& out);

} // namespace observex
