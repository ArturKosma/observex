#pragma once

#include "core/Detection.h"

#include <array>
#include <cstdint>
#include <vector>

namespace observex {

// A thing the tracker follows. Its box is where it is now; `sure` is smoothed over the last few looks, and `kind` is
// the one the model has been surest of lately.
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
// - It starts being followed only from a look where it is at least kStartSure sure; once followed, any box found
//   for it counts (the recogniser hands over 30% and up). So something the model keeps half-believing (a carved
//   ornament next to a Nightmare Weaver at 30-45%, 2026-11-01) never shows, while a real thing that dips stays.
// - A thing that is not found is held where its speed takes it, for as long as it was seen, up to kHoldMs; so a
//   one-look dip in a crowd does not blink, and a short false alarm is not stretched.
// - Its kind is the one it was found as most, weighted by sureness, over about the last kSmoothKindMs, so a thing
//   the model is torn about does not flicker between kinds.
// - Each look's things are matched to the followed ones by where those are heading, nearest first, so a fast
//   camera turn (100 pixels between looks) keeps them. A box that lies inside a followed one is the same thing
//   too, so a close character cut by the screen edge keeps its identity.
// About 0.2 microseconds per look. Use it from one thread.
class Tracker {
public:
    static constexpr int kConfirmLooks = 3;
    // Measured on five live logs (2026-11-01): the ornament shown 0.2 s instead of 5.6 s in 34 s, boxes appearing
    // or vanishing 6.0 -> 3.0 a second on that log and fewer on the others, real things still covered as before;
    // a thing first seen at 30-50% shows a little later (at most 54 ms instead of 21 on the fight log).
    // 0.5 -> 0.6 on 2026-11-08 (the user asked for steadier live boxes): replaying the tenth model's three live logs
    // of 2026-11-07, boxes appearing or vanishing 2.14 -> 1.36 a second, at the 220 answered images 993 -> 983 of
    // 1051 characters shown and 107 -> 89 boxes on nothing marked; on six older logs 6.47 -> 5.31 a second, 3736 ->
    // 3691 of 4459 shown, 872 -> 774 on nothing. kConfirmLooks 2 found more but was jumpier on the older logs.
    static constexpr float kStartSure = 0.6f;
    static constexpr double kHoldMs = 150.0;
    static constexpr double kSmoothSureMs = 60.0; // How quickly `sure` follows the model's.
    // How quickly the kind follows the model's: slower than sureness. A first guess (2026-11-02), before any model
    // with kinds; live logs record kinds since 2026-11-08, so observex_bench --track can measure it on the next ones.
    static constexpr double kSmoothKindMs = 300.0;
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
        Tracked thing;       // As last found (box), plus speed, smoothed sureness and kind.
        std::array<float, kMaxKinds> kinds{}; // Per kind: how sure the model has been lately that it is that kind.
        double firstMs = 0.0; // When it was first found, and last found.
        double lastMs = 0.0;
        int looks = 0; // Looks it was found on.
    };
    struct Pair {
        float distance;
        int track;
        int found;
        bool weak; // The found box is less than kStartSure sure: paired after the sure ones.
    };

    std::vector<Track> tracks_;
    std::vector<Pair> pairs_;    // Reused each look, so following allocates nothing once warm.
    std::vector<int> matchedTo_; // Per found thing: the track it went to, or -1.
    std::vector<char> matched_;  // Per track: found on this look.
    std::uint32_t nextId_ = 1;
};

} // namespace observex
