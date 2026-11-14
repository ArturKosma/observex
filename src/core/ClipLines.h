#pragma once

#include "core/Detection.h"
#include "core/Ground.h"
#include "core/Heuristics.h"
#include "core/Log.h"

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

// Line ends in `text` become spaces, so the note stays one line.
void appendNote(std::string& out, double timeMs, std::string_view text);
bool readNote(std::string_view line, double& timeMs, std::string& text);

void appendLog(std::string& out, double timeMs, const LogLine& line);
bool readLog(std::string_view line, double& timeMs, LogLine& out);

} // namespace observex
