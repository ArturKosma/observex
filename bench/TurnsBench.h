#pragma once

namespace observex {

// --turns CLIP [--lag MS] [--layout FOLDER] [--step N]: how far the camera turned and tilted between video frames of a
// saved clip, by the points the screen shows (heuristics/TurnPitch), each pair looked for where the mouse's counts
// (clip.txt's mouse lines) say the camera went, against what the counts say (bench/TurnsBench.cpp). argv holds what
// follows --turns.
int turnsBench(int argc, char** argv);

} // namespace observex
