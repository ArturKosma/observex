#pragma once

#include "core/CursorGlide.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace observex {

// Looting with the state tree (the user asked, 2026-11-16: "when we enter Root, we first check if theres a drop. if
// there is - we order our character to move on top of it, and then we spam the Pick Up skill, until no drops on the
// ground"): which drops count (DropBook) and the Move To Drop task (DropPicker).

// A drop as looting sees it: one of the drop finder's newest close look (core/LootWatch), as app/App fills it.
struct LootDrop {
    float x = 0.0f;    // Its middle on the screen image (pixels).
    float y = 0.0f;
    float size = 0.0f; // Its box's longer side (pixels).
    int deskX = 0;     // Its middle on the desktop (GetCursorPos' pixels).
    int deskY = 0;
    float distance = -1.0f; // From the own character's feet along the ground (game units); below 0: not known.
    // Where it lies round the own feet in a frame that does not turn with the camera (game units; as
    // core/MonsterMemory's: right and ahead of the camera's heading at turn 0), known while the camera is: the same
    // drop keeps it while the camera turns.
    bool placed = false;
    float fixedX = 0.0f;
    float fixedY = 0.0f;
};

// Whether `a` and `b` lie at the same place round the own feet (both placed): within kSameNear game units, or
// kSameShare of the farther one's distance (the distances are about 19% off, 2026-11-16).
bool samePlace(const LootDrop& a, const LootDrop& b);
inline constexpr float kSameNear = 30.0f;
inline constexpr float kSameShare = 0.25f;

// Which drops count for the brain (the Drops and Drop Distance numbers) and Move To Drop, so looting never stays stuck
// (the user asked, 2026-11-15: "getting stuck is death"):
// - Drops counts those of the newest close look not given up on, and holds that count kHoldSeconds after they vanish:
//   one look that missed them, or the own character standing on them (it hides what lies under it), does not end the
//   loot, so Pick Up goes on being pressed over them.
// - Given up on (giveUp, or kStuckSeconds within reach while looting: not the user's to pick up, someone else's, or a
//   look-alike such as the blue click-to-move rune): left out for kGiveUpSeconds wherever it lies at the same place
//   (samePlace) round the own feet. Drops not placed (the camera not known) cannot be told apart: each counts.
class DropBook {
public:
    struct Input {
        double now = 0.0;                // Seconds.
        std::uint64_t look = 0;          // The close look's screen image (0: none); a new one is counted once.
        std::span<const LootDrop> drops; // What it found (off the game's interface).
        bool looting = false;            // The brain is in a looting branch (a Move To Drop task in it).
        int reach = 0;                   // That task's reach (game units).
    };
    struct Output {
        int count = 0;                // Drops counted (held), for the Drops number.
        float nearest = -1.0f;        // The nearest counted drop of the newest look (game units); below 0: none known.
        std::vector<LootDrop> counted; // The newest look's drops that count, for Move To Drop.
        std::string log;              // A line for the log when something happened; empty if nothing did.
    };

    Output step(const Input& in);
    // `drop` is left out for kGiveUpSeconds from `now` (Move To Drop gave up on it).
    void giveUp(const LootDrop& drop, double now);
    // Forgets everything (a new screen, nothing observed).
    void reset() { *this = {}; }
    // Whether `drop` lies where one was given up on (`now`: those still given up on).
    bool givenUp(const LootDrop& drop, double now) const;

    static constexpr double kHoldSeconds = 1.5;   // As LootWatch::kLostMs.
    static constexpr double kStuckSeconds = 5.0;  // Within reach this long while looting, still there: given up on.
    static constexpr double kGiveUpSeconds = 60.0;

private:
    struct Mark {
        LootDrop drop;
        double since = 0.0; // Stuck watch: within reach since then; given up: until then.
        double seen = 0.0;
    };

    std::uint64_t look_ = 0;
    std::vector<LootDrop> counted_;
    float nearest_ = -1.0f;
    int held_ = 0;          // The last count above 0,
    double heldAt_ = -1e9;  // and when it was seen.
    std::vector<Mark> near_;    // Drops within reach while looting.
    std::vector<Mark> givenUp_; // `since` is until when.
};

// The state tree's Move To Drop task (the user asked, 2026-11-16: "order our character to move on top of it"). One step
// a UI frame, on the drops DropBook counts:
// 1. Choose: the nearest drop (by distance; one with no distance by how far it is from the own feet on the screen,
//    after all that have one); the same one while it is seen (matched to where it was last seen), else none: done.
// 2. Near enough (within the task's reach, game units; no distance: by the screen against the own character's height),
//    or gone (not seen kGoneSeconds: picked up, or under the own character): done.
// 3. Else the cursor eases onto it (core/CursorGlide) and, there, a left click: the game walks there (a click on an item
//    picks it up too). A character's box over it (a click there would select that character): the ground just short of
//    it on the way to the own feet, the nearest point clear of every box within kShortOf of the way; none: given up on.
// 4. Walking: kRetry after a click, no nearer by kNearer of its distance then: clicked again; nearer, waits on.
// Fallbacks: kClicks clicks with no nearer, or not near enough kGiveUp after choosing it: given up on (DropBook leaves
// it out), done.
class DropPicker {
public:
    struct Box { // Desktop pixels.
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
    };
    struct Input {
        double now = 0.0; // Seconds.
        double dt = 0.0;  // Since the step before.
        int cursorX = 0;  // On the desktop.
        int cursorY = 0;
        std::span<const LootDrop> drops; // DropBook's counted.
        int reach = 0;                   // Game units.
        bool own = false;                // The own character's box is seen: its feet (bottom middle) and height,
        int ownX = 0;                    // desktop pixels.
        int ownY = 0;
        float ownHeight = 0.0f;
        std::span<const Box> characters; // Other characters' boxes (desktop pixels), not the own.
    };
    struct Output {
        int dx = 0; // The cursor's move, desktop pixels.
        int dy = 0;
        bool click = false; // A left click now.
        bool done = false;
        bool gaveUp = false;  // Done by giving up on this drop (as last seen): DropBook::giveUp it.
        LootDrop givenUp{};
        std::string log;      // A line for the log when something happened; empty if nothing did.
    };

    // Starts afresh (the task starts).
    void start();
    Output step(const Input& in);

    static constexpr double kGoneSeconds = 0.5;
    static constexpr double kRetry = 1.0;
    static constexpr float kNearer = 0.9f;
    static constexpr int kClicks = 3;
    static constexpr double kGiveUp = 6.0;
    static constexpr float kShortOf = 0.5f; // At most this share of the way back to the own feet.
    static constexpr int kOnPoint = 3;      // The cursor this near the point (pixels): clicked.
    static constexpr float kMatchPixels = 80.0f; // The chosen drop seen again within this (or 4 of its sizes).

private:
    // How far `drop` is (game units), with no distance by the screen; a large number when that cannot be told.
    float howFar(const LootDrop& drop, const Input& in) const;
    bool withinReach(const LootDrop& drop, const Input& in) const;
    // Where to click for `drop`: on it, or just short of it clear of the characters' boxes; false if nowhere.
    bool clickPoint(const LootDrop& drop, const Input& in, int& x, int& y) const;
    void giveUp(const std::string& why, Output& out);

    bool chosen_ = false;
    LootDrop last_{};         // The chosen drop as last seen.
    double lastSeen_ = 0.0;
    double chosenAt_ = 0.0;
    int clicks_ = 0;
    double clickedAt_ = -1.0; // The last click, or the last time it came nearer after one (-1: no click yet).
    float farAtClick_ = 0.0f; // How far it was then.
    CursorGlide glide_;
};

} // namespace observex
