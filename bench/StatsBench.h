#pragma once

namespace observex {

// --stats PATH... [--layout FOLDER] [--list]: reads the game's bars and their numbers (core/GameStats) on recorded
// images as the UI finder does live (bench/StatsBench.cpp). argv holds what follows --stats.
int statsBench(int argc, char** argv);

} // namespace observex
