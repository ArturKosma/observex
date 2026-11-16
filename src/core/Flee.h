#pragma once

#include "core/CursorGlide.h"
#include "core/Ground.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// The state tree's Flee task, from the target (the user asked, 2026-11-16: "a task Flee, with parameter, for now just
// Target, which finds target on screen or uses stats whatevers faster, sees where character is, understands where is
// the ground in the direction FROM the monster towards player, using depth decides where to click, and keeps moving
// away"). Lineage 2 walks the character to the ground point clicked. One step a UI frame:
// 1. Where the target is: its box's feet (bottom middle) put on the flat ground through the own feet (core/Distances'
//    groundUnder; the flat ground was nearer the geodata than the terrain for distances, 2026-11-16), and kept on the
//    ground's own map (core/Ground: GroundCamera's yaw and feet), so that while it is off the screen (behind the camera,
//    lost by the tracker, under a window) it is fled from where it was last seen, as the camera turns and the character
//    walks. Never seen yet: away from the camera, up the screen (what the screen shows has no target in it).
// 2. Which way: from the target through the own feet, straight first, then kTurnStep more to either side, up to
//    kTurns steps. Along a way, points every kPathStep own heights from the feet, on the terrain the depth network shaped
//    (GroundHeights; not known: the flat ground) as long as nothing the terrain's newest map saw stands in front of
//    them (a trunk, a wall, a hill crest: groundHidden; a character there does not count) and the ground rises less
//    than kSteepest a step (falls less than kDeepest). A point can be clicked where it shows on the screen at least kEdge in from its sides and kMargin off
//    every box to avoid (the characters, the own one too: a click on a monster attacks it, on the own character targets
//    it; the drops; the interface's pieces). The farthest point that can be, up to kFarthest, is the way's reach. The
//    straightest way reaching kGoodReach is taken; with none, the one reaching farthest (at least kNearest).
// 3. Clicking: the cursor eases onto that point (core/CursorGlide, as Select Target's), following it as it is planned
//    again on every step, and once within kAt (and still off every box) it clicks; then again every kClickEvery, so the
//    character keeps moving away, turning as the target moves.
// 4. Never stuck (the user, 2026-11-15: "getting stuck is death"): the feet (on the ground's map) moved less than
//    kMoved in kStuckSeconds since a click: that way is blocked for kBlockedSeconds (no way within kBlockedSpread of it
//    is tried) and another is taken. No way at all for kNowhereSeconds: the blocked ways are forgotten.
// Done once there is nothing to flee from: no target window for kNoTargetSeconds, or the target shown dead.
class FleeWalker {
public:
    // A box on the screen image, in pixels.
    struct Box {
        float x0 = 0.0f;
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
    };
    struct Input {
        double now = 0.0; // Seconds.
        double dt = 0.0;  // Since the step before.
        float width = 0.0f; // The screen image.
        float height = 0.0f;
        // The screen image's pixels on the desktop (GetCursorPos'): x * scaleX + left, y * scaleY + top.
        double left = 0.0;
        double top = 0.0;
        double scaleX = 1.0;
        double scaleY = 1.0;
        int cursorX = 0; // On the desktop.
        int cursorY = 0;
        bool canClick = false; // No press of the app's held now.
        // The target, by the stats reader: its window shows, its HP bar (below 0: not read), its name (for the log).
        bool hasTarget = false;
        float targetHp = -1.0f;
        std::string_view targetName;
        bool targetSeen = false; // Its box on the screen now (the tracker's, the one the stats said is the target),
        Box targetBox{};
        // The camera now as the ground finder follows it (on its map), or not known: then guessed from `pitch` (if
        // known) and the own character's box.
        bool cameraFollowed = false;
        GroundCamera camera;
        bool pitchKnown = false;
        float pitch = 0.0f;
        bool own = false;
        Box ownBox{};
        const GroundHeights* heights = nullptr; // The terrain (null or not known: the flat ground).
        // Never clicked on (see above): the characters' boxes, the own one too (the depth's map sees them in front of
        // the ground behind them, which the way is not stopped by: Lineage 2's characters walk through each other),
        std::span<const Box> characters;
        std::span<const Box> avoid; // and the drops' and the interface's.
    };
    struct Output {
        int dx = 0; // The cursor's move, desktop pixels.
        int dy = 0;
        bool click = false; // A left click now.
        bool done = false;
        bool aiming = false; // The point it goes for, on the screen image.
        float aimX = 0.0f;
        float aimY = 0.0f;
        std::string log; // Lines for the log ('\n' between) when something happened; empty if nothing did.
    };

    // Starts afresh (the task starts).
    void start();
    Output step(const Input& in);

    static constexpr float kPathStep = 0.5f;      // Own heights.
    static constexpr float kFarthest = 10.0f;     // 470 game units.
    static constexpr float kGoodReach = 3.0f;     // 140 game units: clicked again before it gets there.
    static constexpr float kNearest = 1.5f;
    static constexpr float kTurnStep = 0.261799f; // 15 degrees.
    static constexpr int kTurns = 6;              // To 90 degrees either side.
    static constexpr float kSteepest = 0.5f;      // A step's rise, own heights (45 degrees).
    static constexpr float kDeepest = 1.0f;       // A step's fall.
    static constexpr float kEdge = 0.02f;         // Of the screen's width and height.
    static constexpr float kMargin = 8.0f;        // Pixels round a box to avoid.
    static constexpr float kNearFeet = 0.3f;      // The target closer than this (own heights): no way from it.
    static constexpr double kAt = 6.0;            // Desktop pixels from the point: click.
    static constexpr double kClickEvery = 0.7;
    static constexpr double kStuckSeconds = 1.5;
    static constexpr float kMoved = 0.3f;         // Own heights.
    static constexpr double kBlockedSeconds = 3.0;
    static constexpr float kBlockedSpread = 0.4f; // Radians (23 degrees).
    static constexpr double kNowhereSeconds = 1.0;
    static constexpr double kNoTargetSeconds = 0.5;
    static constexpr float kDeadHp = 0.005f;      // As TargetWatch.
    static constexpr float kLogTurn = 0.5236f;    // A new way this far off the one last said: said (30 degrees).
    static constexpr float kGuessPitch = 0.349066f; // As Focus Camera's guesses.
    static constexpr float kGuessDistance = 8.0f;

    // A way planned (public for the tests): the point clicked, as own heights right and away of the feet, and where
    // it shows; how far it reaches; how far it turned from straight away from the target (radians, + right).
    struct Way {
        bool found = false;
        float right = 0.0f;
        float away = 0.0f;
        float reach = 0.0f;
        float turn = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
    };

private:
    struct Blocked {
        float mapX = 0.0f; // The way's direction on the ground's map (a unit vector).
        float mapY = 0.0f;
        double until = 0.0;
    };

    Way plan(const Input& in, const GroundCamera& camera, float fromRight, float fromAway, bool fromKnown) const;
    bool blocked(const GroundCamera& camera, float right, float away, double now) const;

    static constexpr double kLostSeconds = 0.5; // Not seen this long: said lost (the tracker drops a box a frame).

    CursorGlide glide_;
    bool known_ = false;     // The target's feet were seen,
    bool onMap_ = false;     // on the ground's map (else right and away of a guessed camera),
    float targetX_ = 0.0f;   // there,
    float targetY_ = 0.0f;
    double seenAt_ = -1.0;   // then.
    bool saidLost_ = false;
    double noTargetSince_ = -1.0;
    double nextClick_ = 0.0;
    double movedAt_ = -1.0;  // The feet last moved kMoved since (or the first click).
    float feetX_ = 0.0f;     // Where they were then (on the map).
    float feetY_ = 0.0f;
    float wayX_ = 0.0f;      // The way of the last click, on the map.
    float wayY_ = 0.0f;
    std::vector<Blocked> blocked_;
    double nowhereSince_ = -1.0;
    bool saidWay_ = false;   // A way was said in the log,
    float saidX_ = 0.0f;     // this one (on the map, or the guessed camera's right and away).
    float saidY_ = 0.0f;
    bool saidNowhere_ = false;
    bool saidUnseen_ = false;
};

} // namespace observex
