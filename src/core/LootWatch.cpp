#include "core/LootWatch.h"

#include <algorithm>
#include <cmath>

namespace observex {

namespace {

const WatchedThing* thingOf(std::span<const WatchedThing> things, std::uint32_t id) {
    for (const WatchedThing& thing : things) {
        if (thing.id == id) {
            return &thing;
        }
    }
    return nullptr;
}

std::string named(std::string_view name) {
    return name.empty() ? std::string("the target") : std::string(name);
}

// Where some drops lie: the middle of the box round them all, its size, and the largest drop's longer side.
struct Spread {
    float middleX = 0.0f;
    float middleY = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float largest = 0.0f;
};

Spread spreadOf(std::span<const DropSeen> drops) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, largest = 0.0f;
    for (const DropSeen& drop : drops) {
        x0 = std::min(x0, static_cast<float>(drop.box.x0));
        y0 = std::min(y0, static_cast<float>(drop.box.y0));
        x1 = std::max(x1, static_cast<float>(drop.box.x1));
        y1 = std::max(y1, static_cast<float>(drop.box.y1));
        largest = std::max({largest, static_cast<float>(drop.box.x1 - drop.box.x0),
                            static_cast<float>(drop.box.y1 - drop.box.y0)});
    }
    return {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f, x1 - x0, y1 - y0, largest};
}

// The corpse size (lootSquareAt's `longest`) whose square fits drops spread so (lootSquareRound).
float spotLongest(const Spread& spread) {
    constexpr float kInside = 0.8f; // tools/train/loot.py's LOOT_INSIDE.
    return std::max(kDropsSpot * spread.largest, std::max(spread.width, spread.height) / (kInside * kLootSide));
}

} // namespace

LootSquare lootSquareAt(float middleX, float middleY, float longest, int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    const int side = std::min({static_cast<int>(std::lround(std::max(kLootSide * longest, static_cast<float>(kLootMinSide)))),
                               height, width});
    const int x = static_cast<int>(std::lround(middleX - static_cast<float>(side) * 0.5f));
    const int y = static_cast<int>(std::lround(middleY - static_cast<float>(side) * 0.5f));
    return {std::clamp(x, 0, width - side), std::clamp(y, 0, height - side), side};
}

std::vector<LootSquare> lootTiles(int width, int height) {
    std::vector<LootSquare> tiles;
    if (width <= 0 || height <= 0) {
        return tiles;
    }
    // Along one side of `length`: as few starts `side` apart at most three quarters of it, evenly spread.
    const auto starts = [](int length, int side) {
        std::vector<int> at;
        const int room = length - side;
        const int steps = room > 0 ? (room * 4 + side * 3 - 1) / (side * 3) : 0; // ceil(room / (0.75 side)).
        for (int i = 0; i <= steps; ++i) {
            at.push_back(steps > 0 ? static_cast<int>(std::lround(static_cast<double>(room) * i / steps)) : 0);
        }
        return at;
    };
    for (const int wanted : {kTileSide, height}) {
        const int side = std::min({wanted, width, height});
        for (const int y : starts(height, side)) {
            for (const int x : starts(width, side)) {
                tiles.push_back({x, y, side});
            }
        }
        if (side >= std::min(width, height)) {
            break; // Already the largest.
        }
    }
    return tiles;
}

std::vector<int> keepApart(std::span<const Detection> found) {
    std::vector<int> order(found.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<int>(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return found[a].sure > found[b].sure; });
    std::vector<int> kept;
    for (const int i : order) {
        const Detection& a = found[static_cast<std::size_t>(i)];
        bool same = false;
        for (const int k : kept) {
            const Detection& b = found[static_cast<std::size_t>(k)];
            const float w = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
            const float h = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
            if (w <= 0.0f || h <= 0.0f) {
                continue;
            }
            const float shared = w * h;
            const float areaA = a.width * a.height;
            const float areaB = b.width * b.height;
            same = shared / (areaA + areaB - shared) >= kSameOverlap || shared >= kSameInside * std::min(areaA, areaB);
            if (same) {
                break;
            }
        }
        if (!same) {
            kept.push_back(i);
        }
    }
    return kept;
}

LootSquare lootSquare(StatsArea corpse, int width, int height) {
    const float w = static_cast<float>(corpse.x1 - corpse.x0);
    const float h = static_cast<float>(corpse.y1 - corpse.y0);
    const float middleX = static_cast<float>(corpse.x0) + w * 0.5f;
    const float middleY = static_cast<float>(corpse.y1) - std::min(w, h) * 0.5f;
    return lootSquareAt(middleX, middleY, std::max(w, h), width, height);
}

LootSquare lootSquareRound(std::span<const DropSeen> drops, int width, int height) {
    if (drops.empty()) {
        return {};
    }
    const Spread spread = spreadOf(drops);
    return lootSquareAt(spread.middleX, spread.middleY, spotLongest(spread), width, height);
}

void LootWatch::start(Spot spot, const Input& in) {
    const std::uint64_t closeLooked = in.close.looked;
    const std::uint64_t sweepLooked = sweepLooked_;
    *this = {};
    spot_ = spot;
    started_ = in.ms;
    seen_ = in.ms;
    closeLooked_ = closeLooked; // Close looks before the spot say nothing about it.
    sweepLooked_ = sweepLooked;
}

LootWatch::Output LootWatch::step(const Input& in) {
    Output out;
    auto say = [&out](const std::string& line) { out.log += (out.log.empty() ? "" : "; ") + line; };

    // A box marked dead: the same kill under a new id, or a new one (over drops the sweep found, too).
    if (in.dead != 0) {
        if (spot_ == Spot::Kill && in.name == name_ && in.ms - started_ < kSameKillMs) {
            corpse_ = in.dead;
        } else {
            start(Spot::Kill, in);
            corpse_ = in.dead;
            name_ = in.name;
            say("Looking for drops round " + named(name_) + " (box " + std::to_string(corpse_) + ")");
        }
    }

    // The sweep's newest look: drops found with no spot start one round them.
    if (in.sweep.looked != 0 && in.sweep.looked != sweepLooked_) {
        sweepLooked_ = in.sweep.looked;
        if (spot_ == Spot::None && !in.sweep.drops.empty()) {
            start(Spot::Found, in);
            const Spread spread = spreadOf(in.sweep.drops);
            dropsX_ = spread.middleX;
            dropsY_ = spread.middleY;
            longest_ = spotLongest(spread);
        }
    }
    if (spot_ == Spot::None) {
        return out;
    }

    // The newest close look.
    if (in.close.looked != 0 && in.close.looked != closeLooked_) {
        closeLooked_ = in.close.looked;
        drops_ = static_cast<int>(in.close.drops.size());
        if (drops_ > 0) {
            seen_ = in.ms;
            const Spread spread = spreadOf(in.close.drops);
            dropsX_ = spread.middleX;
            dropsY_ = spread.middleY;
            if (spot_ == Spot::Found) {
                longest_ = spotLongest(spread);
            }
        }
        if (drops_ > most_) {
            most_ = drops_;
            say(spot_ == Spot::Kill ? "Drops round " + named(name_) + ": " + std::to_string(most_)
                                    : "Drops seen: " + std::to_string(most_));
        }
    }

    // Where to look: round the corpse while it is there, else round the drops found.
    const WatchedThing* corpse = spot_ == Spot::Kill ? thingOf(in.things, corpse_) : nullptr;
    if (corpse != nullptr) {
        const StatsArea& box = corpse->box;
        longest_ = static_cast<float>(std::max(box.x1 - box.x0, box.y1 - box.y0));
        square_ = lootSquare(box, in.width, in.height);
        seen_ = in.ms;
    } else if (spot_ == Spot::Found || most_ > 0) {
        square_ = lootSquareAt(dropsX_, dropsY_, longest_, in.width, in.height);
    }

    // The end.
    const bool lost = in.ms - seen_ > kLostMs;
    const bool tooLong = spot_ == Spot::Kill && in.ms - started_ > kMaxMs;
    if (lost || tooLong) {
        if (spot_ == Spot::Found) {
            if (most_ > 0) {
                say("Drops gone");
            }
        } else if (tooLong) {
            say("Stopped looking for drops round " + named(name_) + " after " +
                std::to_string(static_cast<int>(kMaxMs / 1000.0)) + " s");
        } else if (most_ == 0) {
            say("No drops round " + named(name_));
        } else {
            say("Drops round " + named(name_) + ": none left");
        }
        start(Spot::None, in);
        return out;
    }
    out.square = square_;
    out.drops = drops_;
    out.kill = spot_ == Spot::Kill;
    return out;
}

} // namespace observex
