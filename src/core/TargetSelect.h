#pragma once

#include "core/CursorGlide.h"
#include "core/GameStats.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace observex {

// A monster Select Target can choose (the tracker's, sure it is a monster and seen now): its id, which stays while it
// is followed, its box on the screen image, and its box's middle and size on the desktop (GetCursorPos' pixels).
struct SelectableMonster {
    std::uint32_t id = 0;
    StatsArea box{};
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// What the newest stats reading said, for Select Target: the name plate over the monster it asked about and the
// target window. `shown` is when the screen showed the image read, on Select Target's clock (seconds).
struct SelectSeen {
    std::uint64_t image = 0; // 0: nothing read yet.
    double shown = 0.0;
    NamePlate plate{};
    bool hasTarget = false;
    std::string targetName;
    float targetHp = -1.0f; // The target window's HP bar, 0 to 1; below 0 if not read.
};

// The state tree's Select Target task (the user asked, 2026-11-15: "smoothly but quickly moving onto the target, and
// once target is hovered over, sending click and checking if new target was selected. once a new target was
// successfully acquired, the task is done"). One step a UI frame, on what is seen then; it says how to move the
// cursor and when to click, and what the stats reader is to look at (the name plate over the monster chosen):
// 1. Choose: the monster followed before while it is seen (or lost under kLoseSeconds), else the biggest on screen
//    (Nearest Monster has no distances yet) not given up on in the last kRetryAfter.
// 2. Move: the cursor eases onto the box's middle (core/CursorGlide), and follows it there.
// 3. Click: once the cursor is in the box and an image shown since says its plate is up with grey orbs (the game shows
//    it hovered), a left click (pressed and let go); never while its orbs are coloured (it is the target already: a
//    click would attack).
// 4. Done: once an image shown after the click has its plate's orbs coloured and the target window up (its name, when
//    both are read, the plate's; a disagreement waits kNamesWait, then is taken with a note in the log); or, its plate
//    not seen coloured (a tree over it), once the target window shows another target than before the click (none then,
//    another name, or the one before dead and this one alive). Coloured orbs before any click (chosen while already the
//    target): done at once; coloured orbs with no target window: done after kSelectedWait, with a note. Never on a
//    target the window shows dead (Target HP at most kDeadHp): already the target, it is given up on at once; just
//    clicked, after kNamesWait (the window may still show the one before).
// Fallbacks, so it never stays stuck on one monster (the user asked, 2026-11-15: "getting stuck is death"):
// - Never shown hovered (its plate hidden behind a tree, or the cursor not on it as the game sees it): given up on once
//   the images shown kHoverWait after the cursor came onto it, and kHoverWait after it last showed hovered, show no
//   plate over it.
// - No sign kClickWait after a click: clicked again once hovered, at most kClicks clicks, then given up on.
// - Anything else: not the target kMonsterSeconds after it was chosen, given up on.
// Given up on, another is chosen; that one again only kRetryAfter later (also under a new id where it was last seen:
// the tracker lost it and found it again), when it may have come out from behind the tree.
class TargetSelector {
public:
    struct Input {
        double now = 0.0; // Seconds, the clock SelectSeen::shown is on.
        double dt = 0.0;  // Since the step before.
        int cursorX = 0;  // On the desktop.
        int cursorY = 0;
        std::span<const SelectableMonster> monsters;
        SelectSeen seen;
    };
    struct Output {
        int dx = 0; // The cursor's move, desktop pixels.
        int dy = 0;
        bool click = false; // A left click now.
        bool done = false;
        PlateLook look{};   // What the stats reader is to look at (id 0: nothing).
        std::string log;    // A line for the log when something happened; empty if nothing did.
    };

    // Starts afresh (the task starts).
    void start();
    Output step(const Input& in);

    // The monster chosen (0: none).
    std::uint32_t chosen() const { return chosen_; }

    static constexpr double kTopSpeed = CursorGlide::kTopSpeed; // The cursor's easing (core/CursorGlide).
    static constexpr double kAcceleration = CursorGlide::kAcceleration;
    static constexpr double kInside = 0.35;         // In the box: within this share of its size from its middle.
    static constexpr double kLoseSeconds = 0.3;     // A monster not seen this long is lost.
    static constexpr double kHoverWait = 1.0;       // On it this long, never shown hovered: given up on.
    static constexpr double kClickWait = 1.0;       // A click with no sign of selecting this long is clicked again.
    static constexpr int kClicks = 3;
    static constexpr double kNamesWait = 1.0;
    static constexpr double kSelectedWait = 1.0;    // Coloured orbs this long with no target window: done.
    static constexpr double kMonsterSeconds = 5.0;  // Not the target this long after choosing it: given up on.
    static constexpr double kRetryAfter = 2.0;      // One given up on is chosen again only this long after.
    static constexpr float kDeadHp = 0.005f;        // A target bar read at most this full: dead (as TargetWatch).
    static constexpr float kAliveHp = 0.05f;        // At least this full: alive.

private:
    struct GivenUp {
        std::uint32_t id = 0;
        StatsArea box{}; // Where it was last seen, on the screen image.
        double at = 0.0;
    };

    void choose(const Input& in, std::string& log);
    bool givenUpOn(const SelectableMonster& m, std::span<const SelectableMonster> monsters) const;
    void giveUp(double now, const std::string& why, Output& out);

    std::uint32_t chosen_ = 0;
    SelectableMonster last_{};       // The chosen one as last seen.
    double lastSeen_ = -1.0;
    double chosenAt_ = -1.0;
    std::vector<GivenUp> givenUp_;
    CursorGlide glide_;
    double insideSince_ = -1.0;      // The cursor in its box since then (-1: not in it).
    double hoveredAt_ = -1.0;        // The newest image shown with its plate up (SelectSeen::shown; -1: none).
    int clicks_ = 0;                 // On the chosen one.
    double clickedAt_ = -1.0;        // The last click (-1: none).
    SelectSeen beforeClick_{};       // The reading just before the last click: the target then.
    double namesDifferSince_ = -1.0; // Selected but the names disagree, since then.
    double selectedSince_ = -1.0;    // Coloured orbs with no target window, since then.
    double deadSince_ = -1.0;        // Selected after a click but the target window shows it dead, since then.
    bool saidNone_ = false;
    double started_ = -1.0;
};

} // namespace observex
