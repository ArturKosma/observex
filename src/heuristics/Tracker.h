#pragma once

#include "core/Detection.h"

#include <array>
#include <cstdint>
#include <vector>

namespace observex {

// Follows what the model finds from look to look, so each thing keeps one steady box and one identity while the
// model's own answer flickers. Measured on a live log of a Lineage 2 fight (2026-11-01, `observex_bench --track`):
// boxes appearing or vanishing went from 21 to 3.8 a second, none shown for a single look, while still covering
// 99.6% of what the model saw; a new thing shows at most about 20 ms later (at 100 looks a second).
//
// - A new thing counts once it is found on kConfirmLooks looks in a row; one- and two-look flickers never show.
// - It starts being followed only from a look where it is at least kStartSure sure; once followed, any box found
//   for it counts (the recogniser hands over 30% and up). So something the model keeps half-believing (a carved
//   ornament next to a Nightmare Weaver at 30-45%, 2026-11-01) never shows, while a real thing that dips stays.
// - A thing that is not found is held where its speed takes it (at most kMaxAheadMs of it), for as long as it was
//   seen, up to kHoldMs; so a one-look dip in a crowd does not blink, and a short false alarm is not stretched. One
//   moving fast on the screen (kFastSpeed, a camera turn) only up to kFastHoldMs: it stands still where it is held
//   while the world moves on, so held longer it took the box of something else that came by (2026-11-11, the user:
//   a monster lost 130 ms before in a turn took a 47% box above a wolf, shown as "Character? 51%" off the wolf).
// - Its kind is the one it was found as most, weighted by sureness, over about the last kSmoothKindMs, so a thing
//   the model is torn about does not flicker between kinds; how sure of it, and its second kind, come from the same
//   scores (each look counts the model's surest kind and its second), so they follow the model too.
// - Each look's things are matched to the followed ones by where those are heading, nearest first, so a fast
//   camera turn (100 pixels between looks) keeps them. A box that lies inside a followed one is the same thing
//   too, so a close character cut by the screen edge keeps its identity.
// - Its speed is a straight line through where it was found over about the last kSpeedMs, and each box is handed
//   out kLeadMs ahead along it: where the thing is by the time the box is on the screen. Until 2026-11-11 a box was
//   where the thing was on the image looked at, about 12 ms before (a Lineage 2 clip: 9.5 ms to look, then until
//   the screen's next image), so in a camera turn (3-4 pixels a millisecond) boxes were 30-50 pixels behind their
//   things and the middles of 64% of them outside the thing (the user: "precision of the boxes is SUPER important",
//   the bot will click their middles). See kLeadMs for what it does now. The rest is the game's own unevenness: in
//   a steady turn an orc moved 51, 5, 51, 15, 30 pixels on images 8, 4, 8, 4, 8 ms apart, which no speed
//   foretells; a box is exact again within two looks of the camera stopping.
// - Foretold at most kMaxAheadMs ahead, and its line started again where it jumped off it (kJumpPixels), so a game
//   that stood still and then caught up keeps its things (2026-11-11, the user: after one, a box beside an orc and a
//   second one sliding away). On 31 live logs boxes appearing or vanishing 3.97 -> 3.71 a second on average (fewer
//   on 27, one more on 1: 3.4 -> 3.5).
// - When things were not found near where they were heading but moved alike (the scene's shift: kShiftNear,
//   kShiftSupport), each is also paired where that shift takes it. The camera jumps after the game stood still, and
//   the game moves the whole scene unevenly from image to image; paired only by where each was heading, all the
//   small things were lost in such a jump and followed again as new ones (2026-11-11, the user: frame 178 of the
//   22:37 clip, the scene 76 and then 183 pixels on in two images, after the game stood still 27 ms).
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
    // A thing moving at least kFastSpeed pixels a millisecond is held at most kFastHoldMs. On the 31 live logs
    // (observex_bench --track, at their 2,399 answered images; 2026-11-11): boxes on nothing marked 2149 -> 2040,
    // characters shown 10514 -> 10505 of 14526, boxes appearing or vanishing 3.706 -> 3.710 a second. On the four
    // clips of 2026-11-11 (--clip-guesses, snapped) boxes on nothing the model found 5-9% fewer, and about as many of
    // its things without a box (+1.5%). 60 ms: 1941 on nothing, but 10493 shown and 3.75 a second; 40 ms: 1892,
    // 10478, 3.86. Moving held things on along their speed instead (beyond kMaxAheadMs, look by look) lost
    // characters (10494) and was jumpier (3.80 a second), even for fast ones alone (10500, 3.79).
    static constexpr float kFastSpeed = 0.8f;
    static constexpr double kFastHoldMs = 100.0;
    static constexpr double kSmoothSureMs = 60.0; // How quickly `sure` follows the model's.
    // How quickly the kind follows the model's: slower than sureness. A first guess (2026-11-02), before any model
    // with kinds; live logs record kinds since 2026-11-08, so observex_bench --track can measure it on the next ones.
    static constexpr double kSmoothKindMs = 300.0;
    // How long after its image a box is on the screen, about: 9.5 ms from the image to the box shown, then on average
    // 3 ms until the screen's next image, on a Lineage 2 clip of 2026-11-11 at 120-240 looks a second (12, against
    // 10 or 14, put the boxes nearest their things in a camera turn). The bot could add its own delay to click.
    static constexpr double kLeadMs = 12.0;
    // Its speed: a straight line through where it was found over the last kSpeedMs (at least its last two looks;
    // about 3 looks). On that clip 20 ms was best; the speed so far followed by 30% of each step (until 2026-11-11)
    // lagged behind a turn.
    static constexpr double kSpeedMs = 20.0;
    // How far ahead its speed foretells a thing at most. After a longer gap the game had stood still: in the 19:08
    // clip of 2026-11-11 the screen showed no new image for 50 ms (the one gap over 20 ms in two clips), and things
    // then were 16 pixels on instead of the 340 their speed said. Foretold by all of it, each was lost and found again
    // as a new thing, while the old one was held where its speed took it: a second box beside it, sliding away.
    static constexpr double kMaxAheadMs = 20.0;
    // Found farther than this from where its speed foretold it, a thing jumped (the camera did, right after that gap:
    // 120 to 160 pixels in 4 ms): its speed starts again from there, or the line through the jump said 30 pixels a
    // millisecond for the next 20 ms while it stood still. On those two clips 10 of 27,970 finds were over 60 pixels
    // from where they were foretold, the one jump among them (most within 20).
    static constexpr float kJumpPixels = 60.0f;
    // A thing found farther than kMaxDistance from where a followed one is heading (in the larger side of the two
    // boxes), or much bigger or smaller (kMinAreaRatio), is another thing. One held through a gap may be
    // kReachPerMs farther for each millisecond it was not seen.
    static constexpr float kMaxDistance = 1.0f;
    static constexpr float kReachPerMs = 0.005f;
    static constexpr float kMinAreaRatio = 0.4f;
    // Unless at least this much of the smaller box lies inside the other: then it is the same thing (cut by the
    // screen edge or partly hidden), matched after the near ones.
    static constexpr float kMinInside = 0.8f;
    // The scene's shift: when at least kShiftSupport followed things were found (or have a box not taken) about as
    // far beyond where they were heading (within kShiftNear of their larger side of each other), and at least half
    // of those just lost, each thing not near where it was heading is also paired where that takes it, nearer than
    // kShiftNear. Measured on 2026-11-11 (observex_bench --clip-guesses, Today not snapped, against the model's boxes
    // on each frame's own image): on the 22:37 clip (the game at 120 images a second) boxes on nothing 996 -> 915 and
    // the model's things without a box 532 -> 362, on the 22:10 one (fast turns in a crowd) 1683 -> 1762 and 1035 ->
    // 746; on the 31 live logs (--track) characters shown 10505 -> 10518 of 14526, boxes on nothing 2040 -> 2048,
    // boxes appearing or vanishing 3.710 -> 3.668 a second. Its tolerance growing with the shift (by a quarter of it)
    // let three lost things in a crowd pair with boxes across the screen (22:10: 2567 on nothing); kShiftSupport 2:
    // 2062 on nothing on the live logs, 4: churn 3.684.
    static constexpr float kShiftNear = 1.0f;
    static constexpr int kShiftSupport = 3;

    // Takes what the model found on the image the screen showed at `timeMs` (any order; times must not go back),
    // and writes up to `max` of the things it follows into `out`, surest first. Returns how many.
    int update(double timeMs, const Detection* found, int count, Tracked* out, int max);

    void clear() { tracks_.clear(); }

private:
    struct Seen {
        double ms = 0.0; // When its middle was found where.
        float x = 0.0f;
        float y = 0.0f;
    };
    struct Track {
        Tracked thing;       // As last found (box), plus speed, smoothed sureness and kinds.
        std::array<float, kMaxKinds> kinds{}; // Per kind: how sure the model has been lately that it is that kind.
        std::array<Seen, 8> seen{}; // Where it was found on its last looks, oldest first (enough for kSpeedMs at up
        int seenCount = 0;          // to 350 looks a second; with more, the last 8 count).
        double firstMs = 0.0; // When it was first found, and last found.
        double lastMs = 0.0;
        int looks = 0; // Looks it was found on.
        bool jumped = false; // Its line started again on its last look (kJumpPixels).
    };
    struct Pair {
        float distance;
        int track;
        int found;
        bool weak; // The found box is less than kStartSure sure: paired after the sure ones.
        bool shifted; // Paired where the scene's shift takes it (it was not near where it was heading).
    };
    struct Heading {
        float x = 0.0f; // Where its middle is heading on this look,
        float y = 0.0f;
        float reach = 0.0f; // and how far from there (in its larger side) a box of it may be.
    };

    // About as large (kMinAreaRatio), and the larger side of the two.
    static bool alike(const Tracked& thing, const Detection& box);
    static float sideOf(const Tracked& thing, const Detection& box);
    // Pairs the followed things with `found` (matched_, matchedTo_, trackTo_), with `shifted` also where shiftX_,
    // shiftY_ take them.
    void pair(const Detection* found, int count, bool shifted);
    // After pair() on the look at `timeMs`: how far the scene moved beyond where things were heading (shiftX_,
    // shiftY_), if enough followed things say alike.
    bool sceneShift(double timeMs, const Detection* found, int count);

    // Sets the kind shown, and for a model with kinds how sure of it and the second, from the track's kind scores.
    static void showKinds(Track& track);
    // Notes where it was found now, and learns its speed from that and the looks before.
    static void see(Track& track, double timeMs);

    std::vector<Track> tracks_;
    std::vector<Pair> pairs_;    // Reused each look, so following allocates nothing once warm.
    std::vector<Heading> heading_; // Per track.
    std::vector<int> matchedTo_; // Per found thing: the track it went to, or -1.
    std::vector<char> matched_;  // Per track: found on this look,
    std::vector<int> trackTo_;   // and the found thing it went to, or -1,
    std::vector<char> shiftedPair_; // paired by the scene's shift.
    float shiftX_ = 0.0f; // sceneShift()'s.
    float shiftY_ = 0.0f;
    std::uint32_t nextId_ = 1;
};

} // namespace observex
