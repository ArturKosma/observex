#pragma once

namespace observex {

// --geodata CLIP [--layout FOLDER] [--geo FOLDER] [--scale UNITS] [--first-lines] [--out FOLDER] [--every N]: the Show
// flag Geodata over a saved clip (bench/GeodataBench.cpp). argv holds what follows --geodata.
int geodataBench(int argc, char** argv);

} // namespace observex
