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
}

std::uint32_t TargetWatch::boxUnder(StatsArea plate, std::span<const WatchedThing> things) {
    const int middle = (plate.x0 + plate.x1) / 2;
    const int row = (plate.y0 + plate.y1) / 2;
    std::uint32_t best = 0;
    int bestScore = 0;
    for (const WatchedThing& thing : things) {
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

PlateLook TargetWatch::nextLook(std::span<const WatchedThing> things) {
    if (things.empty()) {
        return {};
    }
    const WatchedThing* next = nullptr;
    for (const WatchedThing& thing : things) {
        if (!has(looked_, thing.id)) {
            next = &thing;
            break;
        }
    }
    if (next == nullptr) { // All looked at: round again.
        looked_.clear();
        next = &things.front();
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
    if (plate.look != 0) {
        const std::uint32_t under = plate.seen && plate.selected ? boxUnder(plate.at, in.things) : 0;
        if (under != 0) {
            box_ = under;
            misses_ = 0;
            looked_.clear();
            searching_ = 0;
        } else if (plate.look == box_) {
            ++misses_;
        }
    }
    if (box_ != 0 && (misses_ >= kMisses || thingOf(in.things, box_) == nullptr)) {
        box_ = 0; // Its plate not over it any more, or its box gone: looked for again.
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
    } else if (box_ != 0 && box_ != found_) {
        found_ = box_;
        say("Target " + named(target_) + " is box " + std::to_string(box_));
    }
    if (box_ != 0) {
        if (!died_) {
            out.look = {box_, thingOf(in.things, box_)->box}; // Its plate checked on every reading.
        }
    } else if (!died_) {
        out.look = nextLook(in.things);
    } else if (searching_ < kSearchReadings) {
        out.look = nextLook(in.things);
        if (++searching_ == kSearchReadings) {
            say("Dead target " + named(target_) + ": its box not found");
        }
    }
    out.target = box_;
    last_ = out;
    return out;
}

} // namespace observex
