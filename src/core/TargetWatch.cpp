#include "core/TargetWatch.h"

#include <algorithm>
#include <cstdlib>

namespace observex {

namespace {

bool has(const std::vector<std::uint32_t>& ids, std::uint32_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

const WatchedThing* thingOf(std::span<const WatchedThing> things, std::uint32_t id) {
    for (const WatchedThing& thing : things) {
        if (thing.id == id) {
            return &thing;
        }
    }
    return nullptr;
}

std::string named(std::string_view target) {
    return target.empty() ? std::string("(name not read)") : std::string(target);
}

} // namespace

void TargetWatch::reset(std::string_view target) {
    target_ = target;
    box_ = 0;
    found_ = 0;
    misses_ = 0;
    looked_.clear();
    alive_ = false;
    empty_ = 0;
    died_ = false;
    searching_ = 0;
    marked_.clear();
    corpse_ = 0;
    corpseAt_ = {};
}

std::uint32_t TargetWatch::boxUnder(StatsArea plate, std::span<const WatchedThing> things, bool notOwn) {
    const int middle = (plate.x0 + plate.x1) / 2;
    const int row = (plate.y0 + plate.y1) / 2;
    std::uint32_t best = 0;
    int bestScore = 0;
    for (const WatchedThing& thing : things) {
        if (notOwn && thing.own) {
            continue;
        }
        const StatsArea& box = thing.box;
        const int h = box.y1 - box.y0;
        // As findPlate: over its sides, give or take kSidePlay, above a third down it, and no higher than plateArea
        // looks.
        if (middle < box.x0 - kSidePlay || middle > box.x1 + kSidePlay || row > box.y0 + h / 3 ||
            row < box.y0 - std::max(180, h * 6 / 10)) {
            continue;
        }
        // Twice findPlate's score, in whole pixels: nearest over its middle, and just over its head.
        const int score = 2 * std::abs(middle - (box.x0 + box.x1) / 2) + std::abs(row - (box.y0 - 30));
        if (best == 0 || score < bestScore) {
            best = thing.id;
            bestScore = score;
        }
    }
    return best;
}

PlateLook TargetWatch::nextLook(std::span<const WatchedThing> things, bool notOwn, const StatsArea* near) {
    // How far a box's middle is from `near`'s (squared, doubled pixels); 0 without one.
    const auto far = [near](const StatsArea& box) {
        if (near == nullptr) {
            return 0LL;
        }
        const long long dx = (box.x0 + box.x1) - (near->x0 + near->x1);
        const long long dy = (box.y0 + box.y1) - (near->y0 + near->y1);
        return dx * dx + dy * dy;
    };
    const auto pick = [&](bool skipLooked) {
        const WatchedThing* next = nullptr;
        for (const WatchedThing& thing : things) {
            if ((notOwn && thing.own) || (skipLooked && has(looked_, thing.id))) {
                continue;
            }
            if (next == nullptr || far(thing.box) < far(next->box)) {
                next = &thing; // The first (no `near`: the first stays), or nearer.
            }
        }
        return next;
    };
    const WatchedThing* next = pick(true);
    if (next == nullptr) { // All looked at: round again.
        looked_.clear();
        next = pick(false);
    }
    if (next == nullptr) {
        return {};
    }
    looked_.push_back(next->id);
    return {next->id, next->box};
}

TargetWatch::Output TargetWatch::step(const Input& in) {
    if (in.image != 0 && in.image == image_) {
        Output same;
        same.look = last_.look;
        same.target = last_.target;
        return same;
    }
    image_ = in.image;
    Output out;
    if (in.target != target_) {
        reset(in.target);
    }
    if (!in.hasTarget) {
        // No target window (GameStats::target says None only once that held): nothing to look at meanwhile.
        empty_ = 0;
        out.target = box_;
        last_ = out;
        return out;
    }

    // Its HP bar: read above empty, then empty on kEmptyReadings readings in a row (unknown readings say nothing).
    if (in.targetHp > kEmpty) {
        if (died_ && in.targetHp >= kAlive) {
            reset(in.target); // Another of the same name (the target only changes with its name).
        }
        alive_ = true;
        empty_ = 0;
    } else if (in.targetHp >= 0.0f) {
        ++empty_;
    }
    if (!died_ && alive_ && empty_ >= kEmptyReadings) {
        died_ = true;
        out.died = true;
        searching_ = 0;
        out.log = "Target died: " + named(target_) +
                  (box_ != 0 ? "" : " (its box not known yet: looking for its plate)");
    }

    // The plate read, whoever asked for it: selected, the box under it is the target's.
    const NamePlate& plate = in.plate;
    std::uint32_t under = 0; // The box the plate was read over on this reading (0: none).
    if (plate.look != 0) {
        // A dead target is never the own character (the corpse at its feet puts its plate over the own box too).
        under = plate.seen && plate.selected ? boxUnder(plate.at, in.things, died_) : 0;
        if (under != 0) {
            box_ = under;
            misses_ = 0;
            looked_.clear();
            searching_ = 0;
        } else if (plate.look == box_) {
            ++misses_;
        }
    }
    const WatchedThing* boxThing = box_ != 0 ? thingOf(in.things, box_) : nullptr;
    if (box_ != 0 && (misses_ >= kMisses || boxThing == nullptr || (died_ && boxThing->own))) {
        box_ = 0; // Its plate not over it any more, its box gone, or dead and the own character's: looked for again.
        misses_ = 0;
        looked_.clear();
    }

    // What was learned, and what to look at next.
    auto say = [&out](const std::string& line) { out.log += (out.log.empty() ? "" : "; ") + line; };
    if (box_ != 0 && died_ && !has(marked_, box_)) {
        out.dead = box_;
        marked_.push_back(box_);
        found_ = box_;
        say((out.died ? "box " : "Dead target " + named(target_) + " is box ") + std::to_string(box_) +
            ": marked dead");
        // The box marked before, still on the screen, is not the corpse: the plate is over this one.
        if (corpse_ != 0 && corpse_ != box_ && thingOf(in.things, corpse_) != nullptr) {
            out.alive = corpse_;
            marked_.erase(std::remove(marked_.begin(), marked_.end(), corpse_), marked_.end());
            say("box " + std::to_string(corpse_) + " is not it: its dead mark taken back");
        }
        corpse_ = box_;
        under = box_; // Where the corpse is seen (below).
    } else if (box_ != 0 && box_ != found_) {
        found_ = box_;
        say("Target " + named(target_) + " is box " + std::to_string(box_));
    }
    if (box_ != 0) {
        const StatsArea& at = thingOf(in.things, box_)->box;
        out.look = {box_, at}; // Its plate checked on every reading, dead too (the tracker may hand its id on).
        if (died_ && box_ == corpse_ && under == box_) {
            corpseAt_ = at; // Only where its plate was over it: not where a live one that took its id walks.
        }
    } else if (!died_) {
        out.look = nextLook(in.things, false);
    } else if (searching_ < kSearchReadings) {
        // The corpse lies where it was: the boxes nearest its last one first.
        out.look = nextLook(in.things, true, corpse_ != 0 ? &corpseAt_ : nullptr);
        if (++searching_ == kSearchReadings) {
            say("Dead target " + named(target_) + ": its box not found");
        }
    }
    out.target = box_;
    last_ = out;
    return out;
}

} // namespace observex
