#pragma once

#include "core/GameStats.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// A character shown on the screen now (the tracker's): its id, which stays while it is followed, its box on the screen
// image, and whether it is the own character (core/Ground's ownCharacter).
struct WatchedThing {
    std::uint32_t id = 0;
    StatsArea box{};
    bool own = false;
};

// The target's death, and which box on the screen it is (the user asked, 2026-11-15: "bias the guesser that something
// is dead once the target's red hp bar reaches 0 without switching targets"; "find the box of that target due to 2
// blue dots on the left and right of its name on the screen, and choose the box nearest to it ... once we discover the
// death from stats we can shift the weights of the box towards dead"). Fed each new stats reading, it says which box
// the stats reader is to look at for a name plate (heuristics/NamePlate), and so learns which box is the target:
// 1. While there is a target and its box is not known, the boxes on the screen are looked at in turn, one a reading,
//    until a plate with coloured orbs (the target's) is read over one, whoever asked for that look; the target is then
//    the box nearest under that plate (as findPlate chooses: its middle over the box's sides, give or take kSidePlay,
//    and above a third down it).
// 2. Known, its box is looked at on every reading; kMisses answers in a row without its plate selected over it, or its
//    box gone from the screen, and it is looked for again.
// 3. Died: its HP bar, once read above empty, read empty (not unknown) on kEmptyReadings readings in a row, the target
//    unchanged (GameStats::target). Once per target. From then on every box found to be the target is to be marked
//    dead once (the game keeps a dead target selected, its plate up; a box the tracker lost comes back as another),
//    and it is looked for at most kSearchReadings readings in a row, the boxes nearest the corpse's last box first.
//    Never the own character's box (2026-11-16: a corpse at its feet put the dead target's plate over it, so it was
//    shown dead, and the ground finder, which fits the camera round it, lost it: no grid, no distances, Select
//    Target turning to stale memories): it is not looked at then, and a plate over it and another box goes to the
//    other.
// 4. Dead, its box is still looked at on every reading, as while alive (the user's clip 2026-11-16_14-04-25: an Imp
//    walked over the corpse it was killed beside, the two showed as one box a moment, and when they parted the tracker
//    kept the corpse's id on the live Imp; marked dead for good, it was never selected while it hit the own
//    character). Its plate read over another box (looking at it, or looked for again after kMisses), that box is the
//    corpse: it is marked dead, and the box marked before, if still on the screen, is not the corpse: its mark is
//    taken back (Output::alive).
// A new target (or none), or the same name's HP bar full again after it died (another of that name), starts afresh.
class TargetWatch {
public:
    struct Input {
        std::uint64_t image = 0;  // The reading's screen image (GameStats::image); 0: nothing read yet.
        bool hasTarget = false;   // GameStats::hasTarget.
        std::string_view target;  // GameStats::target: the target's name, or empty for none.
        float targetHp = -1.0f;   // The Target window's HP bar, how full (below 0: not seen).
        NamePlate plate{};        // The plate read over the box asked for (by anyone: plate.look; 0: none asked).
        std::span<const WatchedThing> things; // On the screen now, the own character too (it can be the target).
    };
    struct Output {
        PlateLook look{};         // What the stats reader is to look at next (id 0: nothing).
        std::uint32_t target = 0; // The target's box (WatchedThing::id), 0 while not known.
        bool died = false;        // The target died on this reading.
        std::uint32_t dead = 0;   // A box to mark dead now (once each), 0 for none.
        std::uint32_t alive = 0;  // A box marked dead before that is not the corpse after all: its mark taken back.
        std::string log;          // A line for the log when something happened; empty if nothing did.
    };

    // A new reading (the same image again changes nothing).
    Output step(const Input& in);

    static constexpr int kSidePlay = 40;
    static constexpr int kMisses = 5;          // Half a second at the UI finder's 10 readings a second.
    static constexpr int kEmptyReadings = 2;
    static constexpr float kEmpty = 0.005f;    // How full a bar read empty is at most.
    static constexpr float kAlive = 0.05f;     // How full one read alive is at least.
    static constexpr int kSearchReadings = 30; // Three seconds.

private:
    void reset(std::string_view target);
    // The box nearest under the plate at `plate` (orb to orb), or 0 if none is under it; with `notOwn`, never the
    // own character's.
    static std::uint32_t boxUnder(StatsArea plate, std::span<const WatchedThing> things, bool notOwn);
    // The next box to look at while looking for the target's: one not looked at since the search began (with `near`,
    // the one whose middle is nearest its middle; else the first); with `notOwn`, never the own character's.
    PlateLook nextLook(std::span<const WatchedThing> things, bool notOwn, const StatsArea* near = nullptr);

    std::uint64_t image_ = 0;
    Output last_{};                     // What the last reading gave (its look and box), for the same image again.
    std::string target_;                // The target watched (its name; empty for none, or not read).
    std::uint32_t box_ = 0;             // Its box, 0 while not known.
    std::uint32_t found_ = 0;           // The box last said in the log to be it.
    int misses_ = 0;
    std::vector<std::uint32_t> looked_; // Looked at since the search began.
    bool alive_ = false;                // Its HP bar was read above empty.
    int empty_ = 0;                     // Readings in a row its HP bar read empty.
    bool died_ = false;
    int searching_ = 0;                 // Readings in a row it was looked for since it died.
    std::vector<std::uint32_t> marked_; // Its boxes said to be marked dead (a mark taken back: no longer).
    std::uint32_t corpse_ = 0;          // The box marked dead last, 0 for none,
    StatsArea corpseAt_{};              // and where it was last seen.
};

} // namespace observex
