#include "core/Tracker.h"

#include <algorithm>
#include <cmath>

namespace observex {

namespace {

// How much of each new speed measurement is taken (the rest is the speed so far), so one jittery box does not
// throw a thing off course.
constexpr float kSpeedFollow = 0.3f;

float centreX(const Detection& box) { return box.x + box.width * 0.5f; }
float centreY(const Detection& box) { return box.y + box.height * 0.5f; }

} // namespace

int Tracker::update(double timeMs, const Detection* found, int count, Tracked* out, int max) {
    const int trackCount = static_cast<int>(tracks_.size());

    // Pair each followed thing with what was found near where it is heading: boxes at least kStartSure sure first
    // (so a sure box is not taken by a weak one's neighbour), nearest pairs first.
    pairs_.clear();
    for (int t = 0; t < trackCount; ++t) {
        const Track& track = tracks_[static_cast<std::size_t>(t)];
        const Tracked& thing = track.thing;
        const float ahead = static_cast<float>(timeMs - track.lastMs);
        const float x = centreX(thing) + thing.speedX * ahead;
        const float y = centreY(thing) + thing.speedY * ahead;
        const float left = x - thing.width * 0.5f;
        const float top = y - thing.height * 0.5f;
        const float area = thing.width * thing.height;
        const float reach = kMaxDistance + kReachPerMs * ahead;
        for (int f = 0; f < count; ++f) {
            const Detection& box = found[f];
            const float boxArea = box.width * box.height;
            const float side = std::max({thing.width, thing.height, box.width, box.height});
            const float distance = std::hypot(centreX(box) - x, centreY(box) - y) / side;
            const bool weak = box.sure < kStartSure;
            if (std::min(area, boxArea) >= kMinAreaRatio * std::max(area, boxArea) && distance < reach) {
                pairs_.push_back({distance, t, f, weak});
                continue;
            }
            const float overlapX = std::min(left + thing.width, box.x + box.width) - std::max(left, box.x);
            const float overlapY = std::min(top + thing.height, box.y + box.height) - std::max(top, box.y);
            if (overlapX > 0.0f && overlapY > 0.0f && overlapX * overlapY >= kMinInside * std::min(area, boxArea)) {
                pairs_.push_back({distance + 1.0f, t, f, weak}); // After the near ones.
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
    matchedTo_.assign(static_cast<std::size_t>(count), -1);
    for (const Pair& pair : pairs_) {
        char& trackTaken = matched_[static_cast<std::size_t>(pair.track)];
        int& foundTaken = matchedTo_[static_cast<std::size_t>(pair.found)];
        if (trackTaken == 0 && foundTaken < 0) {
            trackTaken = 1;
            foundTaken = pair.track;
        }
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
            const float speedX = (centreX(box) - centreX(thing)) / static_cast<float>(elapsed);
            const float speedY = (centreY(box) - centreY(thing)) / static_cast<float>(elapsed);
            if (track.looks > 1) {
                thing.speedX += kSpeedFollow * (speedX - thing.speedX);
                thing.speedY += kSpeedFollow * (speedY - thing.speedY);
            } else {
                thing.speedX = speedX;
                thing.speedY = speedY;
            }
            thing.sure += static_cast<float>(1.0 - std::exp(-elapsed / kSmoothSureMs)) * (box.sure - thing.sure);
        }
        thing.x = box.x;
        thing.y = box.y;
        thing.width = box.width;
        thing.height = box.height;
        ++track.looks;
        track.lastMs = timeMs;
    }

    // Let go of what was not found: new things at once, the others once held as long as they were seen (at most
    // kHoldMs).
    std::size_t kept = 0;
    for (std::size_t t = 0; t < tracks_.size(); ++t) {
        const Track& track = tracks_[t];
        if (matched_[t] == 0) {
            const double unseen = timeMs - track.lastMs;
            if (track.looks < kConfirmLooks || unseen > std::min(kHoldMs, track.lastMs - track.firstMs)) {
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
        track.thing.id = nextId_++;
        track.firstMs = timeMs;
        track.lastMs = timeMs;
        track.looks = 1;
        tracks_.push_back(track);
    }

    // Hand out the confirmed ones, surest first; held ones where their speed has taken them.
    int written = 0;
    for (const Track& track : tracks_) {
        if (track.looks < kConfirmLooks) {
            continue;
        }
        Tracked thing = track.thing;
        const float ahead = static_cast<float>(timeMs - track.lastMs);
        thing.held = ahead > 0.0f;
        thing.x += thing.speedX * ahead;
        thing.y += thing.speedY * ahead;
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
