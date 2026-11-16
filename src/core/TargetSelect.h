#pragma once

#include "core/CameraTurn.h"
#include "core/CursorGlide.h"
#include "core/GameStats.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace observex {

// A monster Select Target can choose (the tracker's, sure it is a monster and seen now): its id, which stays while it
// is followed, its box on the screen image, its box's middle and size on the desktop (GetCursorPos' pixels), how far
// it is from the own character (own heights; below 0: not known) and its key in the monster memory (0: not in it).
struct SelectableMonster {
    std::uint32_t id = 0;
    StatsArea box{};
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float distance = -1.0f;
    std::uint32_t key = 0;
};

// The game's interface for Select Target (the UI finder's pieces, on the desktop): a click or a right-button press on
// it goes to the interface, not to the world behind it (the user's clip 2026-11-16_14-25-14: an imp seen through the
// see-through minimap got the cursor; on the minimap the game hovers nothing, and the right button pressed there to
// turn the camera turned nothing, three times). A point within kUiMargin of a piece counts as on it.
inline constexpr int kUiMargin = 4;
bool onInterface(int x, int y, std::span<const StatsArea> ui);
// Moves a monster's click point (x, y) off the interface: its box's middle if clear, else the clear point of its box
// nearest the middle, within TargetSelector::kInside of its size from it (so the cursor there counts as on it). False
// when no part of it there is clear (it is left out).
bool clearOfInterface(SelectableMonster& m, std::span<const StatsArea> ui);

// A monster the monster memory (core/MonsterMemory) keeps off the screen now: its key, the tracker id it was last seen
// with, how far it is (own heights) and its bearing from the camera's straight ahead now (radians, + right).
struct RememberedMonster {
    std::uint32_t key = 0;
    std::uint32_t id = 0;
    float distance = 0.0f;
    float bearing = 0.0f;
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
// 1. Choose: the monster followed before while it is seen (or lost under kLoseSeconds), else the nearest (the user
//    asked, 2026-11-16: "Select Target nearest monster should now use the distances") not given up on in the last
//    kRetryAfter: of those on the screen by their distance (those with none after, the biggest first), unless the
//    monster memory keeps one off the screen nearer by kTurnWorth: then the camera is turned to it first (one
//    core/CameraTurn turn, the right button, to kFaceBearing of straight ahead on the side it is on, so the own
//    character does not stand in front of it) and, once the turn settled, it is the one on the screen the memory says
//    is it, unless one on the screen with a distance is nearer now (one walked in while the camera looked away: it is
//    taken instead, 2026-11-16); not there within kFindWait (or the game never took the press, kPresses times), it is
//    forgotten (Output::forgetKey) and another chosen. While the right button is held the cursor moves not (dx, dy turn the camera then).
//    The cursor on the game's interface (Input::ui) when a turn is to begin: it is first eased off it, to the point
//    kClearAbove of the screen's height over its middle (or the clear point nearest that), then the button pressed.
// 2. Move: the cursor eases onto the box's middle (core/CursorGlide), or the point of it clear of the interface
//    (clearOfInterface, done by the caller), and follows it there.
// 3. Click: once the cursor is in the box and an image shown since says its plate is up with grey orbs (the game shows
//    it hovered), a left click (pressed and let go); never while its orbs are coloured (it is the target already: a
//    click would attack). No plate over it kHoverWait after the cursor came onto it (the user's clips
//    2026-11-16_14-22-00: tree leaves drawn over the plate; 14-25-14: the plate under the Status window, the game
//    hovering it all the same) and no living target in the target window: clicked once all the same, and the target
//    window says (4.); never with a living target (that one might be it: a click would attack).
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
//   plate over it, and the click made then (3.) had no sign within kClickWait (or none could be made: a living target).
// - No sign kClickWait after a click: clicked again once hovered, at most kClicks clicks, then given up on.
// - Anything else: not the target kMonsterSeconds after it was chosen, given up on.
// Given up on, another is chosen; that one again only kRetryAfter later (also under a new id where it was last seen:
// the tracker lost it and found it again), when it may have come out from behind the tree. The memory is told
// (Output::gaveUpKey): one given up on twice is no longer counted in Monsters Around, the tree's way back to Look
// Around.
// Nothing to select at all (none on the screen, none remembered off it to turn to) kNoneWait long: done without a
// target (the user asked, 2026-11-16: "just finish when nothing to select"), so the tree's When Tasks Done transitions
// go. Not while some on the screen were given up on a moment ago: it waits for them (tried again within kRetryAfter).
class TargetSelector {
public:
    struct Input {
        double now = 0.0; // Seconds, the clock SelectSeen::shown is on.
        double dt = 0.0;  // Since the step before.
        int cursorX = 0;  // On the desktop.
        int cursorY = 0;
        std::span<const SelectableMonster> monsters;
        SelectSeen seen;
        std::span<const RememberedMonster> offScreen; // The memory's monsters not on the screen now.
        bool canTurn = false;        // The camera's turn (core/MouseCamera) and the ground camera are known:
                                     // offScreen can be turned to, and the one there known again.
        bool cursorAtMiddle = false; // The cursor is where the game puts it when it turns the camera.
        bool mouseFree = true;       // None of the user's own buttons down (a turn lets go of the right one then).
        std::span<const StatsArea> ui; // The game's interface on the desktop (the UI finder's pieces).
        StatsArea screen{};            // The observed screen on the desktop (none: the turns start where the cursor is).
        int underInterface = 0;        // Monsters on the screen left out of `monsters`: under the interface.
    };
    struct Output {
        int dx = 0; // The cursor's move, desktop pixels; while `right` is held the camera's turn, counts.
        int dy = 0;
        bool right = false; // The right button held (turning the camera to a monster off the screen).
        bool click = false; // A left click now.
        bool done = false;
        PlateLook look{};   // What the stats reader is to look at (id 0: nothing).
        std::uint32_t gaveUpKey = 0; // The memory's key of a monster given up on now (0: none),
        std::uint32_t forgetKey = 0; // and of one to forget (turned to and not there).
        std::string log;    // A line for the log when something happened; empty if nothing did.
    };

    // Starts afresh (the task starts).
    void start();
    Output step(const Input& in);

    // The monster chosen (0: none).
    std::uint32_t chosen() const { return chosen_; }
    // Lets go of the right button at once if it holds it (the task stopped mid-turn); whether it did.
    bool letGo(double now) { return turner_.letGo(now); }

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
    static constexpr float kTurnWorth = 0.85f;      // One off the screen is turned to if nearer than this share of
                                                    // the nearest on it (a turn costs a second or so).
    static constexpr float kFaceBearing = 0.349066f; // 20 degrees: where the turn brings it, beside the own character.
    static constexpr double kFindWait = 0.6;        // Turned to: found on the screen within this, or forgotten.
    static constexpr int kPresses = 3;              // Presses the game did not take before it is given up on.
    static constexpr double kNoneWait = 0.5;        // Nothing to select this long: done without a target.
    static constexpr double kClearAbove = 0.2;      // A turn starts with the cursor this share of the screen's height
                                                    // over its middle (above the own character), when it was on the
                                                    // interface;
    static constexpr double kPlaceWait = 1.0;       // not there this long (held back), the turn begins all the same.

private:
    struct GivenUp {
        std::uint32_t id = 0;
        StatsArea box{}; // Where it was last seen, on the screen image.
        double at = 0.0;
    };

    void choose(const Input& in, Output& out);
    void chooseShown(const Input& in, const SelectableMonster& m, const char* why, Output& out);
    // While turning to one off the screen: the turn's step, and once settled finding it; true while that goes on.
    bool face(const Input& in, Output& out);
    // Presses the right button for the turn to facing_ (pendingTurn_), or first eases the cursor off the interface;
    // what it did, for the log ("" when it pressed).
    const char* beginTurn(const Input& in, Output& out);
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
    double noneSince_ = -1.0; // Nothing to select since then (-1: something was).
    double started_ = -1.0;
    // Turning to a monster the memory keeps off the screen (facing_ its key; 0: not turning).
    CameraTurner turner_;
    std::uint32_t facing_ = 0;
    std::uint32_t facingId_ = 0;
    float facingDistance_ = 0.0f;
    int pendingTurn_ = 0;     // Its counts (+ right).
    double settledAt_ = -1.0; // The turn settled then (-1: not yet).
    int facePresses_ = 0;     // Not taken by the game.
    bool interrupted_ = false; // Let go mid-turn (the mouse wanted): chosen again once settled.
    bool saidWanted_ = false;
    // Easing the cursor off the interface before the turn's press (placing_), to placeX_, placeY_ (desktop).
    bool placing_ = false;
    int placeX_ = 0;
    int placeY_ = 0;
    double placeSince_ = 0.0;
    CursorGlide placeGlide_;
};

} // namespace observex
