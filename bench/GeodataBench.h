#pragma once

namespace observex {

// --geodata CLIP [--layout FOLDER] [--geo FOLDER] [--scale UNITS] [--first-lines] [--out FOLDER] [--every N]: the Show
// flag Geodata over a saved clip (bench/GeodataBench.cpp). argv holds what follows --geodata.
int geodataBench(int argc, char** argv);
// --pickups IMAGE... [--layout FOLDER]: what the pickup reader (heuristics/PickupReader) reads off each screenshot's
// Messages window, every line shown taken as new (bench/GeodataBench.cpp). argv holds what follows --pickups.
int pickupsBench(int argc, char** argv);

} // namespace observex
