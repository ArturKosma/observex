#pragma once

#include "core/GameStats.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// A character shown on the screen now (the tracker's, the own character left out): its id, which stays while it is
// followed, and its box on the screen image.
struct WatchedThing {
    std::uint32_t id = 0;
    StatsArea box{};
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
//    and it is looked for at most kSearchReadings readings in a row.
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
    // The box nearest under the plate at `plate` (orb to orb), or 0 if none is under it.
    static std::uint32_t boxUnder(StatsArea plate, std::span<const WatchedThing> things);
    // The next box to look at while looking for the target's: one not looked at since the search began.
    PlateLook nextLook(std::span<const WatchedThing> things);

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
    std::vector<std::uint32_t> marked_; // Its boxes said to be marked dead.
};

} // namespace observex
