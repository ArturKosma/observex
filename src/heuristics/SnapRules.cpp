#include "heuristics/SnapRules.h"

#include "heuristics/Tracker.h"

#include <algorithm>
#include <cmath>

namespace observex {

namespace {

// Where a thing looked for wide was found fits the camera's jump (Tracker::kShiftNear of its larger side, and
// kJumpShare of the jump).
bool fitsShift(float offsetX, float offsetY, float shiftX, float shiftY, const Tracked& thing) {
    return std::hypot(offsetX - shiftX, offsetY - shiftY) <
           Tracker::kShiftNear * std::max(thing.width, thing.height) +
               SnapRules::kJumpShare * std::hypot(shiftX, shiftY);
}

} // namespace

void SnapRules::see(Thing& thing, double timeMs, float aheadMs) {
    Tracked& at = thing.at;
    // Jumped (Tracker::kJumpPixels): its line starts again here, standing still until it is snapped again. Judged as
    // the tracker does: once it has its own line, and right after a jump once more against standing there.
    const bool again = thing.jumped;
    thing.jumped = false;
    if (thing.seenCount >= 2 || (again && thing.seenCount == 1)) {
        const Seen& before = thing.seen[static_cast<std::size_t>(thing.seenCount - 1)];
        if (std::hypot(at.x + at.width * 0.5f - (before.x + at.speedX * aheadMs),
                       at.y + at.height * 0.5f - (before.y + at.speedY * aheadMs)) > Tracker::kJumpPixels) {
            thing.seenCount = 0;
            at.speedX = 0.0f;
            at.speedY = 0.0f;
            thing.jumped = !again;
        }
    }
    if (thing.seenCount == static_cast<int>(thing.seen.size())) {
        std::move(thing.seen.begin() + 1, thing.seen.end(), thing.seen.begin());
        --thing.seenCount;
    }
    thing.seen[static_cast<std::size_t>(thing.seenCount++)] = {timeMs, at.x + at.width * 0.5f, at.y + at.height * 0.5f};
    // A straight line through the last kSpeedMs (at least the last two).
    int first = thing.seenCount - 1;
    while (first > 0 && (timeMs - thing.seen[static_cast<std::size_t>(first - 1)].ms <= kSpeedMs ||
                         thing.seenCount - first < 2)) {
        --first;
    }
    const int count = thing.seenCount - first;
    if (count < 2) {
        return; // The tracker's speed, until it was snapped twice.
    }
    double meanMs = 0.0;
    double meanX = 0.0;
    double meanY = 0.0;
    for (int i = first; i < thing.seenCount; ++i) {
        const Seen& seen = thing.seen[static_cast<std::size_t>(i)];
        meanMs += seen.ms;
        meanX += seen.x;
        meanY += seen.y;
    }
    meanMs /= count;
    meanX /= count;
    meanY /= count;
    double spread = 0.0;
    double alongX = 0.0;
    double alongY = 0.0;
    for (int i = first; i < thing.seenCount; ++i) {
        const Seen& seen = thing.seen[static_cast<std::size_t>(i)];
        const double ms = seen.ms - meanMs;
        spread += ms * ms;
        alongX += ms * (seen.x - meanX);
        alongY += ms * (seen.y - meanY);
    }
    if (spread > 0.0) {
        at.speedX = static_cast<float>(alongX / spread);
        at.speedY = static_cast<float>(alongY / spread);
    }
}

Tracked SnapRules::foundOf(const Tracked& shown) {
    // The tracker hands boxes out Tracker::kLeadMs ahead of where the look found them.
    Tracked found = shown;
    found.x -= shown.speedX * static_cast<float>(Tracker::kLeadMs);
    found.y -= shown.speedY * static_cast<float>(Tracker::kLeadMs);
    return found;
}

void SnapRules::merge(double lookMs, std::span<const Tracked> shown, std::vector<Thing>& into) const {
    into.clear();
    for (const Tracked& thing : shown) {
        const Tracked found = foundOf(thing);
        const auto known = std::find_if(things_.begin(), things_.end(),
                                        [&](const Thing& t) { return t.at.id == thing.id; });
        Thing next;
        if (known != things_.end()) {
            next = *known;
        }
        // Snapped onto a newer image than the look's, or not found by the look (held there: only the tracker's guess
        // along its speed; taking it slid a box lost in a jump away at the speed it had before, 2026-11-11): stays
        // where it was put, middle where it was, at the look's size, and as fast as it was.
        const bool held = known != things_.end() && thing.held;
        const bool snappedSince = known != things_.end() && known->atMs > lookMs;
        // Put where the look found it, after the snapping could not find it on the last image it looked on: its own
        // speed is what it looked by, so the tracker's (from the looks), and its line starts again here. Until the
        // evening of 2026-11-11 it kept its own: lost as a camera turn sped up, a thing was looked for at the speed it
        // had then and never found again, so its box slid on too slowly between looks, up to 100 pixels behind it
        // (the user: frame 159 of the 22:10 clip, while the tracker had it at 7-9 px/ms, the snapping at 1.1).
        const bool lost = !snappedSince && !held && known != things_.end() && known->stepMs > known->atMs;
        const bool ownSpeed = held || (known != things_.end() && known->seenCount >= 2 && !lost);
        Tracked at = found;
        if (snappedSince || held) {
            const Tracked& was = known->at;
            at.x = was.x + (was.width - found.width) * 0.5f;
            at.y = was.y + (was.height - found.height) * 0.5f;
        } else {
            if (known != things_.end() && lookMs >= known->stepMs) {
                next.wide = false; // The look found it after the image it was lost on.
            }
            next.atMs = lookMs;
            next.aheadMs = 0.0f;
            next.stepMs = lookMs;
            if (lost) {
                next.seenCount = 0;
                next.jumped = false;
            }
        }
        if (ownSpeed) {
            at.speedX = known->at.speedX;
            at.speedY = known->at.speedY;
        }
        next.at = at;
        into.push_back(next);
    }
}

SnapRules::Search SnapRules::searchOf(const Thing& thing, double timeMs) {
    const Tracked& at = thing.at;
    // On from the image before by at most Tracker::kMaxAheadMs: after a longer gap the game had stood still (and
    // looking as far as its speed went took 30 ms on the first image after the 19:08 clip's, with the boxes from
    // before it shown meanwhile).
    const auto step = static_cast<float>(std::clamp(timeMs - thing.stepMs, 0.0, Tracker::kMaxAheadMs));
    const float since = thing.aheadMs + step;
    // As far around as its speed took it, counting at most kMaxAheadMs (searching costs no more however long it is
    // held).
    const float went = std::hypot(at.speedX, at.speedY) * std::min(since, static_cast<float>(Tracker::kMaxAheadMs));
    float reach = kReach + kReachShare * went;
    if (thing.wide) {
        reach = std::max(reach, kJumpReach);
    }
    return {at.x + at.speedX * since, at.y + at.speedY * since, reach, since};
}

void SnapRules::looked(double lookMs, const GreyView* image, std::span<const Tracked> shown) {
    merge(lookMs, shown, next_);
    ids_.clear();
    for (const Tracked& thing : shown) {
        if (image != nullptr && !thing.held) {
            snapper_.learn(*image, thing.id, foundOf(thing));
        }
        ids_.push_back(thing.id);
    }
    things_.swap(next_);
    snapper_.keepOnly(ids_);
}

bool SnapRules::find(const GreyView& image, const Thing& thing, const Search& search, Snapper::Found& found) {
    SnapFind find;
    find.finder = Snapper::kFinder;
    find.id = thing.at.id;
    if (const Snapper::Learned* learned = snapper_.learnedOf(thing.at.id)) {
        find.learnedImage = learned->image;
        find.learnedX = learned->box.x;
        find.learnedY = learned->box.y;
        find.learnedWidth = learned->box.width;
        find.learnedHeight = learned->box.height;
    }
    find.guessX = search.x;
    find.guessY = search.y;
    find.reach = search.reach;
    const auto recorded =
        std::find_if(replay_.begin(), replay_.end(), [&](const SnapFind& other) { return other.sameSearch(find); });
    if (recorded != replay_.end()) {
        found = {recorded->x, recorded->y, recorded->match};
        find.found = recorded->found;
    } else {
        find.found = snapper_.find(image, thing.at.id, search.x, search.y, search.reach, found);
    }
    find.x = find.found ? found.x : 0.0f;
    find.y = find.found ? found.y : 0.0f;
    find.match = found.match;
    finds_.push_back(find);
    return find.found;
}

bool SnapRules::jumpShift(float& shiftX, float& shiftY) const {
    // Each offset from where a thing looked for wide was looked for to where it was found is a guess at how far the
    // camera jumped; it counts the things looked for wide whose own offset it fits. The one most fit, by the mean of
    // theirs weighted by how small they are (a small box says more exactly where it went).
    int best = 0;
    float bestLength = 0.0f;
    for (std::size_t i = 0; i < things_.size(); ++i) {
        const Try& guess = tries_[i];
        if (!things_[i].wide || !guess.hit) {
            continue;
        }
        const float offsetX = guess.found.x - guess.search.x;
        const float offsetY = guess.found.y - guess.search.y;
        int support = 0;
        float sumX = 0.0f;
        float sumY = 0.0f;
        float sumWeight = 0.0f;
        for (std::size_t j = 0; j < things_.size(); ++j) {
            const Try& other = tries_[j];
            const Tracked& thing = things_[j].at;
            if (!things_[j].wide || !other.hit) {
                continue;
            }
            const float x = other.found.x - other.search.x;
            const float y = other.found.y - other.search.y;
            if (fitsShift(x, y, offsetX, offsetY, thing)) {
                const float side = std::max(thing.width, thing.height);
                const float weight = 1.0f / (side * side);
                ++support;
                sumX += weight * x;
                sumY += weight * y;
                sumWeight += weight;
            }
        }
        const float length = offsetX * offsetX + offsetY * offsetY;
        if (support > best || (support == best && length < bestLength)) {
            best = support;
            bestLength = length;
            shiftX = sumX / sumWeight;
            shiftY = sumY / sumWeight;
        }
    }
    return best >= kJumpSupport;
}

int SnapRules::snap(double timeMs, const GreyView& image, Tracked* out, int max) {
    int written = 0;
    finds_.clear();
    // Each thing looked for first, so that those looked for wide (the camera jumped on the image before) count only
    // if they say alike how far it jumped: looked for that far around, one may be found on another thing like it.
    // One that does not fit is looked for again near where the jump takes it (within what was read for it), and held
    // there if not found.
    tries_.resize(things_.size());
    for (std::size_t i = 0; i < things_.size(); ++i) {
        const Thing& thing = things_[i];
        Try& guess = tries_[i];
        guess.search = searchOf(thing, timeMs);
        guess.expected = !thing.wide && thing.atMs >= thing.stepMs;
        guess.hit = find(image, thing, guess.search, guess.found);
    }
    float shiftX = 0.0f;
    float shiftY = 0.0f;
    const bool jumped = jumpShift(shiftX, shiftY);
    for (std::size_t i = 0; i < things_.size(); ++i) {
        Thing& thing = things_[i];
        Try& guess = tries_[i];
        if (!thing.wide ||
            (jumped && guess.hit &&
             fitsShift(guess.found.x - guess.search.x, guess.found.y - guess.search.y, shiftX, shiftY, thing.at))) {
            continue;
        }
        guess.hit = false;
        if (!jumped) {
            continue;
        }
        thing.at.x += shiftX;
        thing.at.y += shiftY;
        guess.search.x += shiftX;
        guess.search.y += shiftY;
        guess.search.reach = std::min(kReach + kJumpShare * std::hypot(shiftX, shiftY),
                                      kJumpReach - std::max(std::abs(shiftX), std::abs(shiftY)));
        if (guess.search.reach >= 1.0f) {
            guess.hit = find(image, thing, guess.search, guess.found);
        }
    }

    for (std::size_t i = 0; i < things_.size(); ++i) {
        Thing& thing = things_[i];
        Tracked& at = thing.at;
        const Search& search = tries_[i].search;
        const Snapper::Found& found = tries_[i].found;
        Tracked shown = at;
        shown.x = search.x;
        shown.y = search.y;
        shown.held = true;
        if (tries_[i].hit) {
            // Foretold from where it was last snapped: as the search, unless the look put it elsewhere since.
            float seenAhead = search.aheadMs;
            if (thing.seenCount > 0) {
                const double seenMs = thing.seen[static_cast<std::size_t>(thing.seenCount - 1)].ms;
                if (seenMs != thing.atMs) {
                    seenAhead = static_cast<float>(std::clamp(timeMs - seenMs, 0.0, Tracker::kMaxAheadMs));
                }
            }
            at.x = found.x;
            at.y = found.y;
            thing.atMs = timeMs;
            if (thing.wide) {
                // Found after the camera jumped: its line starts again here, as after any jump (one that had no line
                // of two yet otherwise got one through the jump, 34 pixels a millisecond).
                thing.seenCount = 0;
                at.speedX = 0.0f;
                at.speedY = 0.0f;
            }
            see(thing, timeMs, seenAhead);
            thing.jumped = thing.jumped || thing.wide;
            thing.aheadMs = 0.0f;
            shown = at;
            shown.held = false;
        } else {
            thing.aheadMs = search.aheadMs; // Held there: on from there on the next image.
        }
        thing.stepMs = timeMs;
        if (shown.held && timeMs - thing.atMs > kGoneMs) {
            continue; // Found neither by the looks nor on the images for that long.
        }
        shown.x += shown.speedX * static_cast<float>(kLeadMs);
        shown.y += shown.speedY * static_cast<float>(kLeadMs);
        // The same thing as one handed out already (lost by the tracker in a jump and followed again as a new thing,
        // while the snapping still had it): only one, the one the look found over one it held. Two the look both
        // found stay: the tracker tells them apart (the model's own boxes overlap so too, a small one in a larger).
        const bool lookFound = !at.held;
        const Tracked* same = std::find_if(out, out + written, [&](const Tracked& other) { return sameThing(other, shown); });
        if (same != out + written) {
            const auto index = static_cast<std::size_t>(same - out);
            if (!lookFound) {
                continue;
            }
            if (lookFound_[index] == 0) {
                out[index] = shown;
                lookFound_[index] = 1;
                continue;
            }
        }
        if (written < max) {
            lookFound_.resize(static_cast<std::size_t>(written) + 1);
            lookFound_[static_cast<std::size_t>(written)] = lookFound ? 1 : 0;
            out[written++] = shown;
        }
    }
    // Most of the things it expected to find lost at once, and at least Tracker::kShiftSupport: the camera jumped
    // (2026-11-11, the user: frame 178 of the 22:37 clip, the scene 183 pixels on from one image to the next, past
    // where any was looked for), so on the next image they are looked for wide.
    int expected = 0;
    int missed = 0;
    for (const Try& guess : tries_) {
        expected += guess.expected ? 1 : 0;
        missed += guess.expected && !guess.hit ? 1 : 0;
    }
    const bool lostScene = missed >= Tracker::kShiftSupport && 2 * missed >= expected;
    for (std::size_t i = 0; i < things_.size(); ++i) {
        things_[i].wide = lostScene && tries_[i].expected && !tries_[i].hit;
    }
    replay_.clear(); // They were this image's.
    return written;
}

bool SnapRules::sameThing(const Tracked& a, const Tracked& b) {
    // As the tracker tells: about as large, and the smaller mostly inside the other.
    const float areaA = a.width * a.height;
    const float areaB = b.width * b.height;
    if (std::min(areaA, areaB) < Tracker::kMinAreaRatio * std::max(areaA, areaB)) {
        return false;
    }
    const float overlapX = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
    const float overlapY = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
    return overlapX > 0.0f && overlapY > 0.0f && overlapX * overlapY >= Tracker::kMinInside * std::min(areaA, areaB);
}

int SnapRules::learnAreas(std::span<const Tracked> shown, GreyArea* out, int max) {
    int written = 0;
    for (const Tracked& thing : shown) {
        if (!thing.held && written < max) {
            out[written++] = Snapper::learnArea(foundOf(thing));
        }
    }
    return written;
}

int SnapRules::snapAreas(double timeMs, bool look, double lookMs, std::span<const Tracked> shown, GreyArea* out,
                         int max) {
    // The things snap() will look for: as they are, or as that look leaves them.
    if (look) {
        merge(lookMs, shown, next_);
    }
    const std::vector<Thing>& things = look ? next_ : things_;
    int written = 0;
    for (const Thing& thing : things) {
        if (written == max) {
            break;
        }
        const Search search = searchOf(thing, timeMs);
        out[written++] = snapper_.findArea(thing.at.id, thing.at.width, thing.at.height, search.x, search.y,
                                           search.reach);
    }
    return written;
}

void SnapRules::clear() {
    things_.clear();
    finds_.clear();
    replay_.clear();
    snapper_.clear();
}

} // namespace observex
