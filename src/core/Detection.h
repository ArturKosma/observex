#pragma once

#include <cstdint>
#include <span>

namespace observex {

// The most kinds a model can tell apart, e.g. monster, player, NPC and the user's own character.
inline constexpr int kMaxKinds = 8;

// Something a model found on a screen image: a box in image pixels, how sure the model is that it is something of
// its category (0 to 1), and, for a model trained with kinds, which kind it is surest of and which next, with how
// sure it is of each.
struct Detection {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float sure = 0.0f;           // That it is something of the category, e.g. a character (with kinds: eitherKind).
    std::uint8_t kind = 0;       // Which of the model's kinds (Detector::kinds()); 0 for a model without kinds.
    std::uint8_t secondKind = 0; // Its second-surest kind,
    float second = -1.0f;        // and how sure it is of that one; -1 for a model without kinds.
    float kindSure = -1.0f;      // How sure it is of `kind`, where that is not `sure` (with kinds); -1: it is.
    std::uint16_t candidate = 0; // Which of the model's candidates it is (0 to 299), for Detector::features.
};

// How sure the model is that `thing` is of its kind.
inline float kindSureOf(const Detection& thing) {
    return thing.kindSure >= 0.0f ? thing.kindSure : thing.sure;
}

// How sure a model with kinds is that something is of its category: that it is one of its two surest kinds, from how
// sure it is of each (as if they were apart: 1 - both unsure). A model with kinds scores each kind on its own, so
// when it cannot tell two kinds apart (e.g. an NPC that looks like a monster: 40% and 35%), its surest kind alone
// says less than it saw. Measured 2026-11-03 on a recording held out of training whole (110 images, 558 marked,
// mostly NPCs): at 50% it finds 400 instead of 297 (36 false alarms instead of 20), AP50 0.897 instead of 0.879; on
// the usual held-out images about the same (0.872, 0.870).
inline float eitherKind(float first, float second) {
    return second > 0.0f ? 1.0f - (1.0f - first) * (1.0f - second) : first;
}

// A thing's two surest kinds less than this apart (e.g. Monster 55%, Player 40%): the model is unsure which kind it
// is. Recording only unsure saves such images, and the review asks about them instead of presuming them yes. Measured
// on the ninth model, the first with kinds (2026-11-03): on its 211 held-out images it flags 48 (23%), which hold 11
// of its 26 wrong kinds; on a recording it never trained on (mostly NPCs it took for monsters), 68 of 230.
inline constexpr float kKindsClose = 0.2f;

// Whether the model is unsure which kind something is: its two surest kinds are close. Never without kinds.
inline bool kindsClose(float sure, float second) {
    return second >= 0.0f && sure - second < kKindsClose;
}
inline bool kindsClose(const Detection& thing) {
    return kindsClose(kindSureOf(thing), thing.second);
}

// Under this sure of any one kind, the model cannot tell which kind something is: "a character, can't tell which"
// (the user asked, 2026-11-08: far-off blobs of pixels the user cannot tell either, for the bot's rules to go and look
// closer). Measured on the live output (kind update and all, observex_bench --images) of two reviewed recordings,
// 1873 characters found: when at least this sure, the kind was right 99.0% of the time on small ones (under 60 pixels
// tall), 98.4% on middling, 98.2% on big ones (130 and up); under it holds 27 of the 29 wrong kinds on small ones
// (and 152 of their 347 right ones: most far-off ones), 8 of 18 middling, 21 of 33 big. At 50%: 21 of the 29, 96.9%
// right when sure. Its two surest kinds close (kindsClose) holds only 11 of the 29.
inline constexpr float kTellKind = 0.6f;

// Whether the model cannot tell which kind `thing` is: under kTellKind sure of its kind, or its kind is `unknownKind`
// (the "can't tell" kind, core/Marks' kUnknownKind, that a model learns from the review's answers: its place in the
// model's kinds; -1: the model does not know it). How sure it is that it is a character at all is `thing.sure`. Never
// for a model without kinds.
inline bool cantTellKind(const Detection& thing, int unknownKind = -1) {
    return thing.second >= 0.0f && (kindSureOf(thing) < kTellKind || static_cast<int>(thing.kind) == unknownKind);
}

// The closest any of `things` at least `minSure` sure has its two surest kinds (how much surer of the first); -1 for
// a model without kinds, or no such things. Under kKindsClose, the model is unsure which kind one of them is.
inline float closestKinds(std::span<const Detection> things, float minSure) {
    float closest = -1.0f;
    for (const Detection& thing : things) {
        const float gap = kindSureOf(thing) - thing.second;
        if (thing.sure >= minSure && thing.second >= 0.0f && (closest < 0.0f || gap < closest)) {
            closest = gap;
        }
    }
    return closest;
}
// The same over two looks at one image, e.g. plain and mirrored: the closer of the two.
inline float closestKinds(std::span<const Detection> one, std::span<const Detection> other, float minSure) {
    const float first = closestKinds(one, minSure);
    const float second = closestKinds(other, minSure);
    return first < 0.0f || (second >= 0.0f && second < first) ? second : first;
}
// Whether a closestKinds says the model is unsure which kind something is.
inline bool unsureOfKind(float closest) {
    return closest >= 0.0f && closest < kKindsClose;
}

// A thing the heuristics show (src/heuristics: the tracker follows it from look to look). Its box is where it is now;
// `sure` is smoothed over the last few looks, `kind` is the one the model has been surest of lately, and `kindSure`,
// `secondKind` and `second` are how sure it has been lately of that kind and of the next.
struct Tracked : Detection {
    std::uint32_t id = 0; // The same while it is followed; never given to another thing.
    float speedX = 0.0f;  // How fast it moves on the screen, in pixels per millisecond.
    float speedY = 0.0f;
    bool held = false; // Not found on this look: kept through a gap, its box moved on by its speed.
};

} // namespace observex
