#pragma once

#include "core/Detection.h"

#include <cstdint>
#include <vector>

namespace observex {

// A thing the tracker follows. Its box is where it is now; `sure` is smoothed over the last few looks.
struct Tracked : Detection {
    std::uint32_t id = 0; // The same while it is followed; never given to another thing.
    float speedX = 0.0f;  // How fast it moves on the screen, in pixels per millisecond.
    float speedY = 0.0f;
    bool held = false; // Not found on this look: kept through a gap, its box moved on by its speed.
};

// Follows what the model finds from look to look, so each thing keeps one steady box and one identity while the
// model's own answer flickers. Measured on a live log of a Lineage 2 fight (2026-11-01, `observex_bench --track`):
// boxes appearing or vanishing went from 21 to 3.8 a second, none shown for a single look, while still covering
// 99.6% of what the model saw; a new thing shows at most about 20 ms later (at 100 looks a second).
//
// - A new thing counts once it is found on kConfirmLooks looks in a row; one- and two-look flickers never show.
// - A thing that is not found is held where its speed takes it, for as long as it was seen, up to kHoldMs; so a
//   one-look dip in a crowd does not blink, and a short false alarm is not stretched.
// - Each look's things are matched to the followed ones by where those are heading, nearest first, so a fast
//   camera turn (100 pixels between looks) keeps them. A box that lies inside a followed one is the same thing
//   too, so a close monster cut by the screen edge keeps its identity.
// About 0.2 microseconds per look. Use it from one thread.
class Tracker {
public:
    static constexpr int kConfirmLooks = 3;
    static constexpr double kHoldMs = 150.0;
    static constexpr double kSmoothSureMs = 60.0; // How quickly `sure` follows the model's.
    // A thing found farther than kMaxDistance from where a followed one is heading (in the larger side of the two
    // boxes), or much bigger or smaller (kMinAreaRatio), is another thing. One held through a gap may be
    // kReachPerMs farther for each millisecond it was not seen.
    static constexpr float kMaxDistance = 1.0f;
    static constexpr float kReachPerMs = 0.005f;
    static constexpr float kMinAreaRatio = 0.4f;
    // Unless at least this much of the smaller box lies inside the other: then it is the same thing (cut by the
    // screen edge or partly hidden), matched after the near ones.
    static constexpr float kMinInside = 0.8f;

    // Takes what the model found on the image the screen showed at `timeMs` (any order; times must not go back),
    // and writes up to `max` of the things it follows into `out`, surest first. Returns how many.
    int update(double timeMs, const Detection* found, int count, Tracked* out, int max);

    void clear() { tracks_.clear(); }

private:
    struct Track {
        Tracked thing;       // As last found (box), plus speed and smoothed sureness.
        double firstMs = 0.0; // When it was first found, and last found.
        double lastMs = 0.0;
        int looks = 0; // Looks it was found on.
    };
    struct Pair {
        float distance;
        int track;
        int found;
    };

    std::vector<Track> tracks_;
    std::vector<Pair> pairs_;    // Reused each look, so following allocates nothing once warm.
    std::vector<int> matchedTo_; // Per found thing: the track it went to, or -1.
    std::vector<char> matched_;  // Per track: found on this look.
    std::uint32_t nextId_ = 1;
};

} // namespace observex
