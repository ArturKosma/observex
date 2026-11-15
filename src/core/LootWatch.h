#pragma once

#include "core/Detection.h"
#include "core/TargetWatch.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// A square part of the screen image (pixels): where drops are looked for. A side of 0: nowhere.
struct LootSquare {
    int x = 0;
    int y = 0;
    int side = 0;

    bool empty() const { return side <= 0; }
    bool operator==(const LootSquare&) const = default;
};

// The square round a corpse whose box is `corpse`, on a screen `width` x `height` (the user asked, 2026-11-15: "we
// check for loot only around the target that we recognized as dead"). The game drops a kill's loot within 70 game
// units of the corpse each way (the own character is 47 tall), so the square is kLootSide times the box's longer side
// (at least kLootMinSide, at most the screen's height), its middle the box's middle across and, down, half the box's
// shorter side above its bottom (a corpse lying down: its middle; one still standing: near its feet); moved, not
// shrunk, to lie inside the screen. A drop is then about a twentieth to a quarter of it across, near or far. The drop
// model was trained on such squares: tools/train/loot.py holds the same rule (keep the two alike).
LootSquare lootSquare(StatsArea corpse, int width, int height);
// The same square with its middle at `middleX`, `middleY` for a corpse `longest` pixels long.
LootSquare lootSquareAt(float middleX, float middleY, float longest, int width, int height);
constexpr float kLootSide = 4.0f;
constexpr int kLootMinSide = 192;

// Squares that cover a whole `width` x `height` image, for looking for drops on it with no corpse known (the review's
// finder on a screenshot): sides of kTileSide and of the image's height, each overlapping the next by a quarter, as
// evenly spread as that allows. A drop is whole in at least one of them.
std::vector<LootSquare> lootTiles(int width, int height);
constexpr int kTileSide = 512;

// Of things found on overlapping squares (lootTiles), the surest of each place: the indices of `found` to keep, surest
// first. Another is the same thing when it overlaps a kept one by kSameOverlap (intersection over union), or lies
// kSameInside inside it or it inside the other (a drop cut at a square's edge).
std::vector<int> keepApart(std::span<const Detection> found);
constexpr float kSameOverlap = 0.5f;
constexpr float kSameInside = 0.6f;

// A drop the drop finder saw (engine/DropFinder), sure enough to count.
struct DropSeen {
    StatsArea box{};
    float sure = 0.0f;
};

// The square round drops found with no corpse: as round a corpse kDropsSpot times the largest drop's longer side (a
// drop then a tenth of its side across, the middle of what the model learned on), and wide enough for their spread to
// fill at most its middle 80%.
LootSquare lootSquareRound(std::span<const DropSeen> drops, int width, int height);
constexpr float kDropsSpot = 2.5f;

// Where to look closely for drops, and what was found there (the user asked, 2026-11-15: first round a kill, then "at
// all times"). The drop finder sweeps the whole screen on its own; this picks the spot it also looks at closely, every
// other image, so its drops are fresh while the camera moves. Fed on every UI frame:
// 1. A box marked dead (core/TargetWatch's Output::dead: the target died and that is its box) starts a kill's spot:
//    the square round that box, followed while the box is on the screen. The same target's box marked again within
//    kSameKillMs (the tracker lost it and found it again under a new id) is the same kill; another death starts anew.
//    Once the corpse is gone (the game lets it vanish after some seconds), the square follows the drops last found
//    (their middle, the corpse's size), as the camera moves. It ends once neither the corpse nor a drop has been seen
//    for kLostMs (the drops picked up, or there were none), or kMaxMs after the death.
// 2. With no spot, drops the sweep found start a spot round them (lootSquareRound), which follows them while the close
//    looks find them and ends kLostMs after they were last seen (picked up, or out of view). A kill takes over.
// Each end and each new largest number of drops found on a close look says a line for the log (a spot the sweep
// started says nothing until a close look found drops there too: one look alone may have been wrong).
class LootWatch {
public:
    // The drop finder's newest look of one sort.
    struct Look {
        std::uint64_t looked = 0;        // Its screen image; 0: none yet. Each counted once.
        std::span<const DropSeen> drops; // What it found.
    };
    struct Input {
        double ms = 0.0;               // Now, in milliseconds (any steady clock).
        int width = 0;                 // The screen's size.
        int height = 0;
        std::uint32_t dead = 0;        // A box marked dead now (TargetWatch::Output::dead), 0 for none.
        std::string_view name;         // The target's name (GameStats::target).
        std::span<const WatchedThing> things; // On the screen now.
        Look close;                    // On the square this asked for.
        Look sweep;                    // On the sweep's square.
    };
    struct Output {
        LootSquare square{}; // Where to look closely now; empty: nowhere, only the sweep.
        int drops = 0;       // Drops seen on the newest close look of this spot.
        bool kill = false;   // The spot is a kill's (else drops the sweep found, maybe wrongly until drops > 0).
        std::string log;     // A line for the log when something happened; empty if nothing did.
    };

    Output step(const Input& in);
    // Stops looking (a new screen, nothing observed), saying nothing.
    void reset() { *this = {}; }

    static constexpr double kSameKillMs = 10'000.0;
    static constexpr double kLostMs = 1'500.0;
    static constexpr double kMaxMs = 60'000.0;

private:
    enum class Spot : std::uint8_t { None, Kill, Found };
    // Starts a spot, the looks before it not counted.
    void start(Spot spot, const Input& in);

    Spot spot_ = Spot::None;
    std::uint32_t corpse_ = 0;
    std::string name_;
    double started_ = 0.0;
    double seen_ = 0.0;     // When the corpse or a drop was last seen.
    float longest_ = 0.0f;  // The corpse box's longer side, last seen (kill); the drops' spot's (found).
    float dropsX_ = 0.0f;   // The middle of the drops last found.
    float dropsY_ = 0.0f;
    int drops_ = 0;         // On the newest close look.
    int most_ = 0;          // The most seen on one close look.
    std::uint64_t closeLooked_ = 0;
    std::uint64_t sweepLooked_ = 0;
    LootSquare square_{};
};

} // namespace observex
