#include "heuristics/Tracker.h"

#include <algorithm>
#include <cmath>

namespace observex {

namespace {

float centreX(const Detection& box) { return box.x + box.width * 0.5f; }
float centreY(const Detection& box) { return box.y + box.height * 0.5f; }

std::size_t kindOf(const Detection& box) { return std::min<std::size_t>(box.kind, kMaxKinds - 1); }

// How sure the model is that `box` is of kind `k`: of its surest kind and, for a model with kinds, of its second (it
// hands no more); 0 for the others.
float kindScore(const Detection& box, std::size_t k) {
    if (k == kindOf(box)) {
        return kindSureOf(box);
    }
    if (box.second >= 0.0f && k == std::min<std::size_t>(box.secondKind, kMaxKinds - 1)) {
        return box.second;
    }
    return 0.0f;
}

} // namespace

void Tracker::showKinds(Track& track) {
    // The kind it has been surest of lately; for a model with kinds also how sure, and the next kind and how sure of
    // that, from the same scores. Until 2026-11-11 these two stayed as the first look had them, so a thing first seen
    // 58% sure of its kind showed "can't tell" for as long as it was followed (a wolf in a Lineage 2 clip, 9 s, while
    // the model said monster, 80% on average over 1136 looks; in that clip 1182 of 15913 boxes shown said can't tell
    // and 271 told a kind against what the smoothed scores said).
    Tracked& thing = track.thing;
    std::size_t first = 0;
    for (std::size_t k = 1; k < track.kinds.size(); ++k) {
        if (track.kinds[k] > track.kinds[first]) {
            first = k;
        }
    }
    std::size_t next = first == 0 ? 1 : 0;
    for (std::size_t k = 0; k < track.kinds.size(); ++k) {
        if (k != first && track.kinds[k] > track.kinds[next]) {
            next = k;
        }
    }
    thing.kind = static_cast<std::uint8_t>(first);
    if (thing.second >= 0.0f) { // A model with kinds; without, `sure` is how sure it is of the one kind.
        thing.kindSure = track.kinds[first];
        thing.secondKind = static_cast<std::uint8_t>(next);
        thing.second = track.kinds[next];
    }
}

void Tracker::see(Track& track, double timeMs) {
    Tracked& thing = track.thing;
    // Jumped (kJumpPixels): its line starts again here, standing still until the next look (keeping the speed it had
    // put moving boxes on the 19:08 clip 1.6 pixels farther from their things on the next image). Judged once it has a
    // line (a new one moves as far as it likes), and right after a jump once more against standing there: in that
    // clip the first image after the gap had things 16 pixels on and the next one 120 more, which as a line of two
    // said 30 pixels a millisecond.
    const bool again = track.jumped;
    track.jumped = false;
    if (track.seenCount >= 2 || (again && track.seenCount == 1)) {
        const Seen& before = track.seen[static_cast<std::size_t>(track.seenCount - 1)];
        const auto ahead = static_cast<float>(std::min(timeMs - before.ms, kMaxAheadMs));
        if (std::hypot(centreX(thing) - (before.x + thing.speedX * ahead),
                       centreY(thing) - (before.y + thing.speedY * ahead)) > kJumpPixels) {
            track.seenCount = 0;
            thing.speedX = 0.0f;
            thing.speedY = 0.0f;
            track.jumped = !again; // Judged again at most once, so a fast one gets its speed back.
        }
    }
    if (track.seenCount == static_cast<int>(track.seen.size())) {
        std::move(track.seen.begin() + 1, track.seen.end(), track.seen.begin());
        --track.seenCount;
    }
    track.seen[static_cast<std::size_t>(track.seenCount++)] = {timeMs, centreX(thing), centreY(thing)};

    // A straight line through the looks of the last kSpeedMs, at least the last two (least squares).
    const int last = track.seenCount - 1;
    if (last < 1) {
        return;
    }
    int from = last;
    while (from > 0 && timeMs - track.seen[static_cast<std::size_t>(from - 1)].ms <= kSpeedMs) {
        --from;
    }
    from = std::min(from, last - 1);
    const auto points = static_cast<double>(last - from + 1);
    double meanMs = 0.0;
    double meanX = 0.0;
    double meanY = 0.0;
    for (int i = from; i <= last; ++i) {
        const Seen& seen = track.seen[static_cast<std::size_t>(i)];
        meanMs += seen.ms - timeMs;
        meanX += seen.x;
        meanY += seen.y;
    }
    meanMs /= points;
    meanX /= points;
    meanY /= points;
    double spread = 0.0;
    double alongX = 0.0;
    double alongY = 0.0;
    for (int i = from; i <= last; ++i) {
        const Seen& seen = track.seen[static_cast<std::size_t>(i)];
        const double ms = seen.ms - timeMs - meanMs;
        spread += ms * ms;
        alongX += ms * (seen.x - meanX);
        alongY += ms * (seen.y - meanY);
    }
    if (spread > 0.0) {
        thing.speedX = static_cast<float>(alongX / spread);
        thing.speedY = static_cast<float>(alongY / spread);
    }
}

bool Tracker::alike(const Tracked& thing, const Detection& box) {
    const float area = thing.width * thing.height;
    const float boxArea = box.width * box.height;
    return std::min(area, boxArea) >= kMinAreaRatio * std::max(area, boxArea);
}

float Tracker::sideOf(const Tracked& thing, const Detection& box) {
    return std::max({thing.width, thing.height, box.width, box.height});
}

void Tracker::pair(const Detection* found, int count, bool shifted) {
    // Each followed thing with what was found near where it is heading (and, shifted, near where the scene's shift
    // takes it): boxes at least kStartSure sure first (so a sure box is not taken by a weak one's neighbour), nearest
    // pairs first.
    const int trackCount = static_cast<int>(tracks_.size());
    pairs_.clear();
    for (int t = 0; t < trackCount; ++t) {
        const Tracked& thing = tracks_[static_cast<std::size_t>(t)].thing;
        const Heading& heading = heading_[static_cast<std::size_t>(t)];
        const float left = heading.x - thing.width * 0.5f;
        const float top = heading.y - thing.height * 0.5f;
        const float area = thing.width * thing.height;
        for (int f = 0; f < count; ++f) {
            const Detection& box = found[f];
            const float side = sideOf(thing, box);
            const float distance = std::hypot(centreX(box) - heading.x, centreY(box) - heading.y) / side;
            const bool weak = box.sure < kStartSure;
            const bool same = alike(thing, box);
            float nearest = same && distance < heading.reach ? distance : -1.0f;
            bool viaShift = false;
            if (shifted && same && nearest < 0.0f) {
                const float moved =
                    std::hypot(centreX(box) - heading.x - shiftX_, centreY(box) - heading.y - shiftY_) / side;
                if (moved < kShiftNear) {
                    nearest = moved;
                    viaShift = true;
                }
            }
            if (nearest >= 0.0f) {
                pairs_.push_back({nearest, t, f, weak, viaShift});
                continue;
            }
            const float boxArea = box.width * box.height;
            const float overlapX = std::min(left + thing.width, box.x + box.width) - std::max(left, box.x);
            const float overlapY = std::min(top + thing.height, box.y + box.height) - std::max(top, box.y);
            if (overlapX > 0.0f && overlapY > 0.0f && overlapX * overlapY >= kMinInside * std::min(area, boxArea)) {
                pairs_.push_back({distance + 1.0f, t, f, weak, false}); // After the near ones.
            }
        }
    }
    std::sort(pairs_.begin(), pairs_.end(), [](const Pair& a, const Pair& b) {
        if (a.weak != b.weak) {
            return b.weak;
        }
        return a.distance != b.distance ? a.distance < b.distance
                                        : (a.track != b.track ? a.track < b.track : a.found < b.found);
    });
    matched_.assign(static_cast<std::size_t>(trackCount), 0);
    shiftedPair_.assign(static_cast<std::size_t>(trackCount), 0);
    trackTo_.assign(static_cast<std::size_t>(trackCount), -1);
    matchedTo_.assign(static_cast<std::size_t>(count), -1);
    for (const Pair& pair : pairs_) {
        char& trackTaken = matched_[static_cast<std::size_t>(pair.track)];
        int& foundTaken = matchedTo_[static_cast<std::size_t>(pair.found)];
        if (trackTaken == 0 && foundTaken < 0) {
            trackTaken = 1;
            foundTaken = pair.track;
            trackTo_[static_cast<std::size_t>(pair.track)] = pair.found;
            shiftedPair_[static_cast<std::size_t>(pair.track)] = pair.shifted ? 1 : 0;
        }
    }
}

bool Tracker::sceneShift(double timeMs, const Detection* found, int count) {
    // Each offset from a followed thing not found to a box not taken, about as large, is a guess at how far the scene
    // moved beyond where things were heading. It counts the followed things it fits: ones found about that far from
    // where they were heading, and ones not found with a box not taken there (within kShiftNear of their larger side),
    // if nearer there than where they were heading (so one that stood still, like the player's own character in a
    // camera turn, does not count). The one most fit, by the mean of theirs weighted by how small they are (a small box
    // says more exactly where it went; in a plain mean the player's own character and a large monster nearby, both
    // where they were heading, held the shift of a 183-pixel jump at 50). It counts if at least kShiftSupport fit, and
    // at least half of the things lost that were found within kMaxAheadMs (in a crowd of small things in a fast turn,
    // three lost things with boxes not taken about as far apart by chance paired others with boxes across the screen).
    const int trackCount = static_cast<int>(tracks_.size());
    const auto recent = [&](int t) { return timeMs - tracks_[static_cast<std::size_t>(t)].lastMs <= kMaxAheadMs; };
    int recentLost = 0;
    for (int t = 0; t < trackCount; ++t) {
        recentLost += matched_[static_cast<std::size_t>(t)] == 0 && recent(t) ? 1 : 0;
    }
    int best = 0;
    int bestLost = 0;
    float bestLength = 0.0f;
    for (int t = 0; t < trackCount; ++t) {
        if (matched_[static_cast<std::size_t>(t)] != 0) {
            continue;
        }
        const Tracked& thing = tracks_[static_cast<std::size_t>(t)].thing;
        for (int f = 0; f < count; ++f) {
            if (matchedTo_[static_cast<std::size_t>(f)] >= 0 || !alike(thing, found[f])) {
                continue;
            }
            const float offsetX = centreX(found[f]) - heading_[static_cast<std::size_t>(t)].x;
            const float offsetY = centreY(found[f]) - heading_[static_cast<std::size_t>(t)].y;
            int support = 0;
            int lostSupport = 0;
            float sumX = 0.0f;
            float sumY = 0.0f;
            float sumWeight = 0.0f;
            for (int u = 0; u < trackCount; ++u) {
                const Tracked& other = tracks_[static_cast<std::size_t>(u)].thing;
                const Heading& heading = heading_[static_cast<std::size_t>(u)];
                const int own = trackTo_[static_cast<std::size_t>(u)];
                float nearest = kShiftNear;
                float fitX = 0.0f;
                float fitY = 0.0f;
                float fitSide = 1.0f;
                for (int g = own >= 0 ? own : 0; g < (own >= 0 ? own + 1 : count); ++g) {
                    if ((own < 0 && matchedTo_[static_cast<std::size_t>(g)] >= 0) || !alike(other, found[g])) {
                        continue;
                    }
                    const float x = centreX(found[g]) - heading.x;
                    const float y = centreY(found[g]) - heading.y;
                    const float apart = std::hypot(x - offsetX, y - offsetY);
                    if (apart >= std::hypot(x, y)) {
                        continue;
                    }
                    const float side = sideOf(other, found[g]);
                    if (apart / side < nearest) {
                        nearest = apart / side;
                        fitX = x;
                        fitY = y;
                        fitSide = side;
                    }
                }
                if (nearest < kShiftNear) {
                    const float weight = 1.0f / (fitSide * fitSide);
                    ++support;
                    lostSupport += own < 0 && recent(u) ? 1 : 0;
                    sumX += weight * fitX;
                    sumY += weight * fitY;
                    sumWeight += weight;
                }
            }
            const float length = offsetX * offsetX + offsetY * offsetY;
            if (support > best || (support == best && length < bestLength)) {
                best = support;
                bestLost = lostSupport;
                bestLength = length;
                shiftX_ = sumX / sumWeight;
                shiftY_ = sumY / sumWeight;
            }
        }
    }
    return best >= kShiftSupport && 2 * bestLost >= recentLost;
}

int Tracker::update(double timeMs, const Detection* found, int count, Tracked* out, int max) {
    const int trackCount = static_cast<int>(tracks_.size());

    // Where each followed thing is heading, and how far from there it may be found.
    heading_.resize(static_cast<std::size_t>(trackCount));
    for (int t = 0; t < trackCount; ++t) {
        const Track& track = tracks_[static_cast<std::size_t>(t)];
        const Tracked& thing = track.thing;
        const auto unseen = static_cast<float>(timeMs - track.lastMs);
        const float ahead = std::min(unseen, static_cast<float>(kMaxAheadMs));
        heading_[static_cast<std::size_t>(t)] = {centreX(thing) + thing.speedX * ahead,
                                                 centreY(thing) + thing.speedY * ahead,
                                                 kMaxDistance + kReachPerMs * unseen};
    }
    pair(found, count, false);
    // The camera jumped, or the game moved the scene unevenly (things moved alike beyond where they were heading):
    // paired again, each not near where it was heading also where the scene's shift takes it.
    bool lost = false;
    for (int t = 0; t < trackCount && !lost; ++t) {
        lost = matched_[static_cast<std::size_t>(t)] == 0;
    }
    bool spare = false;
    for (int f = 0; f < count && !spare; ++f) {
        spare = matchedTo_[static_cast<std::size_t>(f)] < 0;
    }
    if (lost && spare && sceneShift(timeMs, found, count)) {
        pair(found, count, true);
    }

    // Move the matched ones to what was found, learning their speed and smoothing their sureness.
    for (int f = 0; f < count; ++f) {
        const int t = matchedTo_[static_cast<std::size_t>(f)];
        if (t < 0) {
            continue;
        }
        Track& track = tracks_[static_cast<std::size_t>(t)];
        Tracked& thing = track.thing;
        const Detection& box = found[f];
        const double elapsed = timeMs - track.lastMs;
        if (elapsed > 0.0) {
            thing.sure += static_cast<float>(1.0 - std::exp(-elapsed / kSmoothSureMs)) * (box.sure - thing.sure);
            const float follow = static_cast<float>(1.0 - std::exp(-elapsed / kSmoothKindMs));
            for (std::size_t k = 0; k < track.kinds.size(); ++k) {
                track.kinds[k] += follow * (kindScore(box, k) - track.kinds[k]);
            }
            showKinds(track);
        }
        thing.x = box.x;
        thing.y = box.y;
        thing.width = box.width;
        thing.height = box.height;
        const Heading& heading = heading_[static_cast<std::size_t>(t)];
        const bool viaShift = shiftedPair_[static_cast<std::size_t>(t)] != 0 &&
                              std::hypot(centreX(box) - heading.x, centreY(box) - heading.y) > kJumpPixels;
        if (viaShift) {
            // Paired by the scene's shift where it jumped (kJumpPixels): its line starts again here, also when it has
            // no line of two yet (through the jump one said 150 pixels a millisecond).
            track.seenCount = 0;
            thing.speedX = 0.0f;
            thing.speedY = 0.0f;
        }
        if (elapsed > 0.0) {
            see(track, timeMs);
        }
        track.jumped = track.jumped || viaShift;
        ++track.looks;
        track.lastMs = timeMs;
    }

    // Let go of what was not found: new things at once, the others once held as long as they were seen (at most
    // kHoldMs; one moving fast on the screen at most kFastHoldMs).
    std::size_t kept = 0;
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
        const Track& track = tracks_[t];
        if (matched_[t] == 0) {
            const double unseen = timeMs - track.lastMs;
            const bool fast = std::hypot(track.thing.speedX, track.thing.speedY) >= kFastSpeed;
            if (track.looks < kConfirmLooks || unseen > std::min(kHoldMs, track.lastMs - track.firstMs) ||
                (fast && unseen > kFastHoldMs)) {
                continue;
            }
        }
        tracks_[kept++] = track;
    }
    tracks_.resize(kept);

    // Start following what is new and sure enough.
    for (int f = 0; f < count; ++f) {
        if (matchedTo_[static_cast<std::size_t>(f)] >= 0 || found[f].sure < kStartSure) {
            continue;
        }
        Track track;
        static_cast<Detection&>(track.thing) = found[f];
        for (std::size_t k = 0; k < track.kinds.size(); ++k) {
            track.kinds[k] = kindScore(found[f], k);
        }
        track.thing.id = nextId_++;
        track.firstMs = timeMs;
        track.lastMs = timeMs;
        track.looks = 1;
        see(track, timeMs);
        tracks_.push_back(track);
    }

    // Hand out the confirmed ones, surest first, where their speed takes them by the time they are on the screen
    // (held ones also over the looks they were not found on).
    int written = 0;
    for (const Track& track : tracks_) {
        if (track.looks < kConfirmLooks) {
            continue;
        }
        Tracked thing = track.thing;
        const auto unseen = static_cast<float>(timeMs - track.lastMs);
        thing.held = unseen > 0.0f;
        const float ahead = std::min(unseen, static_cast<float>(kMaxAheadMs));
        const float lead = ahead + static_cast<float>(kLeadMs);
        thing.x += thing.speedX * lead;
        thing.y += thing.speedY * lead;
        if (written < max) {
            out[written++] = thing;
        } else {
            // More than fit: replace the least sure one if this one is surer.
            Tracked* least = std::min_element(out, out + max, [](const Tracked& a, const Tracked& b) {
                return a.sure < b.sure;
            });
            if (max > 0 && least->sure < thing.sure) {
                *least = thing;
            }
        }
    }
    std::sort(out, out + written,
              [](const Tracked& a, const Tracked& b) { return a.sure != b.sure ? a.sure > b.sure : a.id < b.id; });
    return written;
}

} // namespace observex
