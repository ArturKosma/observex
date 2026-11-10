#pragma once

#include "core/Tracker.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// The lines of a clip's clip.txt (engine/ClipRecorder) besides the live log's look and kinds lines (core/LiveLines),
// so the clip can be read back line by line, e.g. by observex_bench --track (which skips the lines it does not know).
// Times are milliseconds since the clip's first video frame.
//   frame FRAME IMAGE TIME   video frame FRAME of clip.mp4 (from 1) is screen image number IMAGE, which the screen
//                            showed at TIME. A frame with the image of the frame before repeats it: the screen did not
//                            change for a while (so the video can be cut there).
//   shown IMAGE DONE [ID SURE KIND KINDSURE SECOND SECONDSURE X Y WIDTH HEIGHT SPEEDX SPEEDY HELD]...
//                            what the app showed after its look at image IMAGE (the tracker's things, Tracked), from
//                            DONE on (when the recogniser had it): each thing's id, how sure it is (smoothed), its
//                            kind and how sure of it, its second kind and how sure of that (-1 without kinds), its box
//                            in screen pixels, its speed in pixels per millisecond, and 1 if it was not found on this
//                            look but held where its speed takes it.
//   note TIME TEXT           something the app did then, e.g. "recording started" or "mark 2".

void appendFrame(std::string& out, unsigned frame, unsigned long long image, double timeMs);
bool readFrame(std::string_view line, unsigned& frame, unsigned long long& image, double& timeMs);

void appendShown(std::string& out, unsigned long long image, double doneMs, std::span<const Tracked> things);
// Replaces `things`.
bool readShown(std::string_view line, unsigned long long& image, double& doneMs, std::vector<Tracked>& things);

// Line ends in `text` become spaces, so the note stays one line.
void appendNote(std::string& out, double timeMs, std::string_view text);
bool readNote(std::string_view line, double& timeMs, std::string& text);

} // namespace observex
