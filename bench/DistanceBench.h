#pragma once

namespace observex {

// --distances CLIP [--heuristics DLL] [--layout FOLDER] [--out FILE]: the distances the live view writes on characters
// and drops (core/Distances), over a saved clip (bench/DistanceBench.cpp). argv holds what follows --distances.
int distanceBench(int argc, char** argv);

} // namespace observex
