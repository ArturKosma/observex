#include "core/TargetSelect.h"

#include "core/Distances.h"
#include "core/MouseCamera.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace observex {

namespace {

const SelectableMonster* find(std::span<const SelectableMonster> monsters, std::uint32_t id) {
    for (const SelectableMonster& m : monsters) {
        if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

// The target window shows another target than before the click: none then, another name (both read), or the one then
// dead and this one alive (another of the same name).
bool newTarget(const SelectSeen& before, const SelectSeen& now) {
    if (!now.hasTarget || (now.targetHp >= 0.0f && now.targetHp <= TargetSelector::kDeadHp)) {
        return false; // None, or a dead one: nothing to fight.
    }
    if (!before.hasTarget) {
        return true;
    }
    if (!before.targetName.empty() && !now.targetName.empty() && !sameName(before.targetName, now.targetName)) {
        return true;
    }
    return before.targetHp >= 0.0f && before.targetHp <= TargetSelector::kDeadHp &&
           now.targetHp >= TargetSelector::kAliveHp;
}

const char* plural(int n) { return n == 1 ? "" : "s"; }

constexpr double kDegrees = 57.29577951308232;

// How far `distance` (own heights) is in game units, rounded to 10 as the live tags write it.
int unitsOf(float distance) {
    return static_cast<int>(std::lround(distance * kGameUnits / 10.0f)) * 10;
}

// Whether `m` is nearer than `best`: by its distance, one with none after all that have one, the bigger first then.
bool nearer(const SelectableMonster& m, const SelectableMonster* best) {
    if (best == nullptr) {
        return true;
    }
    if ((m.distance >= 0.0f) != (best->distance >= 0.0f)) {
        return m.distance >= 0.0f;
    }
    if (m.distance >= 0.0f) {
        return m.distance < best->distance;
    }
    return static_cast<long>(m.w) * m.h > static_cast<long>(best->w) * best->h;
}

// Where a turn starts when the cursor is on the interface: kClearAbove of the screen's height over its middle, or the
// clear point of a coarse grid over the screen nearest that (the point itself if none is).
void clearPoint(const StatsArea& screen, std::span<const StatsArea> ui, int& x, int& y) {
    const int w = screen.x1 - screen.x0;
    const int h = screen.y1 - screen.y0;
    const int wantX = screen.x0 + w / 2;
    const int wantY = screen.y0 + h / 2 - static_cast<int>(std::lround(TargetSelector::kClearAbove * h));
    x = wantX;
    y = wantY;
    if (!onInterface(x, y, ui)) {
        return;
    }
    constexpr int kAcross = 16;
    constexpr int kDown = 9;
    long best = -1;
    for (int j = 0; j < kDown; ++j) {
        for (int i = 0; i < kAcross; ++i) {
            const int px = screen.x0 + (2 * i + 1) * w / (2 * kAcross);
            const int py = screen.y0 + (2 * j + 1) * h / (2 * kDown);
            const long d = static_cast<long>(px - wantX) * (px - wantX) + static_cast<long>(py - wantY) * (py - wantY);
            if (!onInterface(px, py, ui) && (best < 0 || d < best)) {
                best = d;
                x = px;
                y = py;
            }
        }
    }
}

} // namespace

bool onInterface(int x, int y, std::span<const StatsArea> ui) {
    return std::ranges::any_of(ui, [&](const StatsArea& a) {
        return x >= a.x0 - kUiMargin && x < a.x1 + kUiMargin && y >= a.y0 - kUiMargin && y < a.y1 + kUiMargin;
    });
}

bool clearOfInterface(SelectableMonster& m, std::span<const StatsArea> ui) {
    if (!onInterface(m.x, m.y, ui)) {
        return true;
    }
    // Points of the box within kInside of its size from the middle (0.05 apart), the nearest clear one.
    constexpr int kSteps = 7; // Either way: 7 x 0.05 = 0.35.
    static_assert(kSteps * 5 <= static_cast<int>(TargetSelector::kInside * 100.0 + 0.5));
    long best = -1;
    int bestX = m.x;
    int bestY = m.y;
    for (int j = -kSteps; j <= kSteps; ++j) {
        for (int i = -kSteps; i <= kSteps; ++i) {
            const int dx = static_cast<int>(std::lround(m.w * 0.05 * i));
            const int dy = static_cast<int>(std::lround(m.h * 0.05 * j));
            const long d = static_cast<long>(dx) * dx + static_cast<long>(dy) * dy;
            if ((best < 0 || d < best) && !onInterface(m.x + dx, m.y + dy, ui)) {
                best = d;
                bestX = m.x + dx;
                bestY = m.y + dy;
            }
        }
    }
    if (best < 0) {
        return false;
    }
    m.x = bestX;
    m.y = bestY;
    return true;
}

void TargetSelector::start() { *this = TargetSelector(); }

const char* TargetSelector::beginTurn(const Input& in, Output& out) {
    if (in.screen.x1 > in.screen.x0 && in.screen.y1 > in.screen.y0 && onInterface(in.cursorX, in.cursorY, in.ui)) {
        placing_ = true;
        placeSince_ = in.now;
        placeGlide_.stop();
        clearPoint(in.screen, in.ui, placeX_, placeY_);
        return "; the cursor first off the game's interface, a press there turns nothing";
    }
    placing_ = false;
    turner_.begin(in.now, pendingTurn_, 0, in.cursorAtMiddle);
    out.right = true;
    return "";
}

bool TargetSelector::givenUpOn(const SelectableMonster& m, std::span<const SelectableMonster> monsters) const {
    const int cx = (m.box.x0 + m.box.x1) / 2;
    const int cy = (m.box.y0 + m.box.y1) / 2;
    for (const GivenUp& g : givenUp_) {
        if (g.id == m.id) {
            return true;
        }
        // A new id whose middle is in the box where one given up on was last seen, while that one is not seen: the
        // tracker lost it and found it again.
        if (find(monsters, g.id) == nullptr && cx >= g.box.x0 && cx <= g.box.x1 && cy >= g.box.y0 &&
            cy <= g.box.y1) {
            return true;
        }
    }
    return false;
}

void TargetSelector::giveUp(double now, const std::string& why, Output& out) {
    const std::string line = "Select Target: gave up on monster " + std::to_string(chosen_) + ", " + why;
    out.log = out.log.empty() ? line : out.log + " | " + line;
    givenUp_.push_back({chosen_, last_.box, now});
    out.gaveUpKey = last_.key;
    chosen_ = 0;
    out.look = {};
}

void TargetSelector::chooseShown(const Input& in, const SelectableMonster& m, const char* why, Output& out) {
    chosen_ = m.id;
    last_ = m;
    lastSeen_ = in.now;
    chosenAt_ = in.now;
    insideSince_ = -1.0;
    hoveredAt_ = -1.0;
    clicks_ = 0;
    clickedAt_ = -1.0;
    namesDifferSince_ = -1.0;
    selectedSince_ = -1.0;
    deadSince_ = -1.0;
    saidNone_ = false;
    noneSince_ = -1.0;
    char line[200];
    if (m.distance >= 0.0f) {
        std::snprintf(line, sizeof(line), "Select Target: going for monster %u (%d away, %s)",
                      static_cast<unsigned>(chosen_), unitsOf(m.distance), why);
    } else {
        std::snprintf(line, sizeof(line), "Select Target: going for monster %u (%d x %d px, %s)",
                      static_cast<unsigned>(chosen_), m.w, m.h, why);
    }
    out.log = out.log.empty() ? std::string(line) : out.log + " | " + line;
}

bool TargetSelector::face(const Input& in, Output& out) {
    if (facing_ == 0) {
        return false;
    }
    char line[300];
    const int away = unitsOf(facingDistance_);
    if (placing_) {
        // Off the interface first (beginTurn): eased to the clear point, then the press. Held back (the user's own
        // button down) the cursor waits; not there kPlaceWait after, the press goes all the same.
        const double toX = placeX_ - in.cursorX;
        const double toY = placeY_ - in.cursorY;
        if (std::hypot(toX, toY) > 2.0 && in.now - placeSince_ < kPlaceWait) {
            if (in.mouseFree) {
                const CursorGlide::Move move = placeGlide_.step(toX, toY, in.dt);
                out.dx = move.dx;
                out.dy = move.dy;
            } else {
                placeGlide_.stop();
            }
            return true;
        }
        if (in.mouseFree) {
            placing_ = false; // Pressed where it is now (there, or kPlaceWait passed).
            turner_.begin(in.now, pendingTurn_, 0, in.cursorAtMiddle);
            out.right = true;
        }
        return true;
    }
    if (!turner_.idle()) {
        if (!in.mouseFree) { // The user's own button down: let go at once; once settled it is chosen again afresh.
            if (turner_.letGo(in.now)) {
                interrupted_ = true;
                out.right = false;
                if (!saidWanted_) {
                    saidWanted_ = true;
                    out.log = "Select Target: let go of the right button, the mouse is wanted";
                }
            }
            return true;
        }
        saidWanted_ = false;
        const CameraTurner::Step s = turner_.step(in.now, in.cursorAtMiddle);
        out.dx = s.dx;
        out.dy = s.dy;
        out.right = s.right;
        if (s.event == CameraTurner::Event::NotTaken) {
            ++facePresses_;
            std::snprintf(line, sizeof(line),
                          "Select Target: the game did not start turning the camera (the cursor never went to its "
                          "middle within 0.5 s); let go of the right button%s",
                          facePresses_ >= kPresses ? "" : ", trying again");
            out.log = line;
        } else if (s.event == CameraTurner::Event::Settled) {
            if (interrupted_) {
                interrupted_ = false;
                facing_ = 0; // Chosen again from where the camera looks now.
            } else if (facePresses_ >= kPresses) {
                std::snprintf(line, sizeof(line),
                              "Select Target: gave up turning to the monster remembered %d away, the game never took "
                              "the right button (%d presses)",
                              away, facePresses_);
                out.log = line;
                out.gaveUpKey = facing_;
                givenUp_.push_back({facingId_, {}, in.now});
                facing_ = 0;
            } else if (!turner_.taken()) {
                beginTurn(in, out); // Pressed again (off the interface first, if the cursor is on it now).
            } else {
                settledAt_ = in.now;
            }
        }
        return true;
    }
    // Turned: the one on the screen the memory says is it, unless one nearer is on the screen now (the user's clip
    // 2026-11-16_13-29-15: a goblin walked in while the camera looked away, came into view on this turn 0.3 s before
    // it was chosen, about 400 away, and the one turned to, 860 away, was taken all the same). The nearest of those on
    // it with a distance, not given up on; the one turned to by its own distance there, else the memory's.
    const SelectableMonster* turnedTo = nullptr;
    const SelectableMonster* best = nullptr;
    for (const SelectableMonster& m : in.monsters) {
        if (m.key == facing_) {
            turnedTo = &m;
        }
        if (m.distance >= 0.0f && !givenUpOn(m, in.monsters) && nearer(m, best)) {
            best = &m;
        }
    }
    const float turnedToDistance = turnedTo != nullptr && turnedTo->distance >= 0.0f ? turnedTo->distance
                                                                                      : facingDistance_;
    if (best != nullptr && best != turnedTo && best->distance < turnedToDistance) {
        std::snprintf(line, sizeof(line), "nearer than the one turned to, remembered %d away%s", away,
                      turnedTo != nullptr ? "" : " and not found yet");
        facing_ = 0;
        chooseShown(in, *best, line, out);
        return false;
    }
    if (turnedTo != nullptr) {
        facing_ = 0;
        chooseShown(in, *turnedTo, "turned to it, remembered off the screen", out);
        return false;
    }
    if (in.now - settledAt_ < kFindWait) {
        return true;
    }
    std::snprintf(line, sizeof(line),
                  "Select Target: turned to the monster remembered %d away, but it is not there (%.1f s); forgotten",
                  away, in.now - settledAt_);
    out.log = line;
    out.forgetKey = facing_;
    facing_ = 0;
    return false;
}

void TargetSelector::choose(const Input& in, Output& out) {
    std::string& log = out.log;
    // Those given up on are tried again kRetryAfter later; until then, followed where they are seen.
    std::erase_if(givenUp_, [&](const GivenUp& g) { return in.now - g.at >= kRetryAfter; });
    for (GivenUp& g : givenUp_) {
        if (const SelectableMonster* seen = find(in.monsters, g.id)) {
            g.box = seen->box;
        }
    }
    char line[300];
    if (chosen_ != 0) {
        if (const SelectableMonster* seen = find(in.monsters, chosen_)) {
            last_ = *seen;
            lastSeen_ = in.now;
            return;
        }
        if (in.now - lastSeen_ < kLoseSeconds) {
            return; // Out of sight a moment (behind another, the tracker lost it): still the one.
        }
        std::snprintf(line, sizeof(line), "Select Target: lost sight of monster %u, choosing another",
                      static_cast<unsigned>(chosen_));
        log = line;
        chosen_ = 0;
    }
    // The nearest on the screen (those with no distance after, the biggest first), of those not given up on.
    const SelectableMonster* best = nullptr;
    int skipped = 0;
    for (const SelectableMonster& m : in.monsters) {
        if (givenUpOn(m, in.monsters)) {
            ++skipped;
            continue;
        }
        if (nearer(m, best)) {
            best = &m;
        }
    }
    // And the nearest the memory keeps off the screen: turned to first, when it is nearer by kTurnWorth.
    const RememberedMonster* off = nullptr;
    for (const RememberedMonster& r : in.offScreen) {
        const bool gaveUp = std::ranges::any_of(givenUp_, [&](const GivenUp& g) { return g.id == r.id; });
        // (Not one forgotten this step: the memory forgets it only after it.)
        if (in.canTurn && !gaveUp && r.key != out.forgetKey && r.distance >= 0.0f &&
            (off == nullptr || r.distance < off->distance)) {
            off = &r;
        }
    }
    if (off != nullptr && (best == nullptr || best->distance < 0.0f || off->distance < kTurnWorth * best->distance)) {
        const float side = off->bearing >= 0.0f ? 1.0f : -1.0f;
        pendingTurn_ = static_cast<int>(std::lround((off->bearing - side * kFaceBearing) / MouseCamera::kCountRadians));
        facing_ = off->key;
        facingId_ = off->id;
        facingDistance_ = off->distance;
        settledAt_ = -1.0;
        facePresses_ = 0;
        interrupted_ = false;
        saidNone_ = false;
        noneSince_ = -1.0;
        const char* first = beginTurn(in, out);
        char said[400];
        std::snprintf(said, sizeof(said),
                      "Select Target: turning %s %.0f degrees to a monster remembered %d away, %.0f degrees %s (%s)%s",
                      pendingTurn_ >= 0 ? "right" : "left",
                      std::abs(pendingTurn_) * MouseCamera::kCountRadians * kDegrees, unitsOf(off->distance),
                      std::fabs(off->bearing) * kDegrees, off->bearing >= 0.0f ? "right" : "left",
                      best == nullptr          ? "none to select on the screen"
                      : best->distance < 0.0f ? "those on the screen have no distance"
                                               : ("the nearest on the screen is " +
                                                  std::to_string(unitsOf(best->distance)) + " away")
                                                     .c_str(),
                      first);
        log = log.empty() ? std::string(said) : log + " | " + said;
        return;
    }
    if (best == nullptr) {
        // Nothing to select (none on the screen, none off it to turn to): done without a target kNoneWait later (one
        // look missing them is not enough), so the tree goes on (the user asked, 2026-11-16: "just finish when nothing
        // to select"; it waited until one showed, and the tree's When Tasks Done transitions never went). While some
        // on it were given up on a moment ago it waits for them instead: they are tried again within kRetryAfter, and
        // a new start would forget they were given up on.
        if (skipped == 0) {
            if (noneSince_ < 0.0) {
                noneSince_ = in.now;
            }
            if (in.now - noneSince_ >= kNoneWait) {
                std::snprintf(line, sizeof(line), "Select Target: done without a target, nothing to select for %.1f s",
                              in.now - noneSince_);
                log = log.empty() ? std::string(line) : log + " | " + line;
                out.done = true;
                return;
            }
        } else {
            noneSince_ = -1.0;
        }
        if (!saidNone_) {
            if (skipped == 0) {
                char under[80] = "";
                if (in.underInterface > 0) {
                    std::snprintf(under, sizeof(under), " (%d under the game's interface: no click reaches %s)",
                                  in.underInterface, in.underInterface == 1 ? "it" : "them");
                }
                std::snprintf(line, sizeof(line), "Select Target: no monster to select on screen%s%s", under,
                              !in.offScreen.empty() && !in.canTurn
                                  ? " (some remembered off it, but the camera is not known well enough to turn there)"
                                  : "");
            } else {
                std::snprintf(line, sizeof(line),
                              "Select Target: no monster to select on screen but %d given up on a moment ago, tried "
                              "again within %.0f s",
                              skipped, kRetryAfter);
            }
            log = log.empty() ? std::string(line) : log + " | " + line;
            saidNone_ = true;
        }
        return;
    }
    const int shown = static_cast<int>(in.monsters.size());
    std::snprintf(line, sizeof(line), best->distance >= 0.0f ? "the nearest of %d" : "the biggest of %d, no distance",
                  shown);
    chooseShown(in, *best, line, out);
}

TargetSelector::Output TargetSelector::step(const Input& in) {
    Output out;
    out.right = turner_.holds();
    if (started_ < 0.0) {
        started_ = in.now;
    }
    if (!face(in, out)) { // Not turning to one off the screen (any more).
        choose(in, out);
    }
    if (facing_ != 0 || chosen_ == 0) {
        glide_.stop();
        insideSince_ = -1.0;
        return out;
    }
    char line[300];
    // Whatever keeps it from being selected (the fallbacks below are the known ways), it is never followed longer.
    if (in.now - chosenAt_ >= kMonsterSeconds) {
        std::snprintf(line, sizeof(line), "not the target %.0f s after choosing it (%d click%s, %s)", kMonsterSeconds,
                      clicks_, plural(clicks_),
                      in.seen.image == 0         ? "nothing read from the screen"
                      : hoveredAt_ >= 0.0        ? "shown hovered"
                      : insideSince_ >= 0.0      ? "never shown hovered"
                                                 : "the cursor not on it");
        giveUp(in.now, line, out);
        return out;
    }
    const SelectableMonster& m = last_;
    out.look = {chosen_, m.box};
    const bool seenNow = lastSeen_ == in.now;

    // Onto the box's middle: eased in (faster the farther, never faster than kTopSpeed, its speed changing at most
    // kAcceleration, and slow enough to brake at that by the middle: no stop at speed), then following it.
    if (seenNow) {
        const double toX = m.x - in.cursorX;
        const double toY = m.y - in.cursorY;
        const CursorGlide::Move move = glide_.step(toX, toY, in.dt);
        out.dx = move.dx;
        out.dy = move.dy;
    }
    // In its box as last seen: a monster missing from a frame (the tracker holding it) keeps the cursor on it, or every
    // reading would be older than the cursor's arrival and it would never be clicked.
    const bool inside = std::abs(in.cursorX - m.x) <= kInside * m.w && std::abs(in.cursorY - m.y) <= kInside * m.h;
    if (!inside) {
        insideSince_ = -1.0;
    } else if (insideSince_ < 0.0) {
        insideSince_ = in.now;
    }

    // What the newest reading says of its plate (when it was about this one).
    const NamePlate& plate = in.seen.plate;
    const bool about = in.seen.image != 0 && plate.look == chosen_;
    const bool selected = about && plate.seen && plate.selected;
    if (about && plate.seen) {
        hoveredAt_ = std::max(hoveredAt_, in.seen.shown);
    }
    const double took = in.now - started_;
    // The target window says the target is dead: no target to fight (2026-11-15, the log: twice it chose the target
    // as it died, took it as "the target already", and the brain stood in Seek Monster, its Combat transition wanting
    // Target HP > 0, until the user stopped it). Already the target: given up on at once, another is chosen. Just
    // clicked: the window may still show the one before a moment, as for the names.
    const bool targetDead = in.seen.targetHp >= 0.0f && in.seen.targetHp <= kDeadHp;
    if (selected && in.seen.hasTarget && targetDead && (clickedAt_ < 0.0 || in.seen.shown > clickedAt_)) {
        if (clickedAt_ < 0.0) {
            giveUp(in.now, "it is the target already, but dead", out);
            return out;
        }
        if (deadSince_ < 0.0) {
            deadSince_ = in.now;
        }
        if (in.now - deadSince_ >= kNamesWait) {
            giveUp(in.now, "the target window shows it dead", out);
        }
        return out;
    }
    deadSince_ = -1.0;
    if (selected && in.seen.hasTarget && (clickedAt_ < 0.0 || in.seen.shown > clickedAt_)) {
        const std::string plateName = plate.name.data();
        const std::string& target = in.seen.targetName;
        if (!plateName.empty() && !target.empty() && !sameName(plateName, target)) {
            if (namesDifferSince_ < 0.0) {
                namesDifferSince_ = in.now;
            }
            if (in.now - namesDifferSince_ < kNamesWait) {
                return out; // The target window may still show the one before.
            }
            std::snprintf(line, sizeof(line),
                          "Select Target: done in %.2f s, though its plate reads '%s' and the target window '%s'", took,
                          plateName.c_str(), target.c_str());
        } else if (clickedAt_ < 0.0) {
            std::snprintf(line, sizeof(line), "Select Target: done, %s was the target already",
                          target.empty() ? "it" : target.c_str());
        } else {
            std::snprintf(line, sizeof(line), "Select Target: done, %s targeted in %.2f s (%d click%s)",
                          target.empty() ? (plateName.empty() ? "the monster" : plateName.c_str()) : target.c_str(),
                          took, clicks_, plural(clicks_));
        }
        out.log = out.log.empty() ? std::string(line) : out.log + " | " + line;
        out.done = true;
        return out;
    }
    if (selected) {
        // Its orbs coloured, the target window not up (or only on an image from before the click): never clicked
        // (that attacks); the orbs say it is the target, so after a moment it is taken as one.
        if (selectedSince_ < 0.0) {
            selectedSince_ = in.now;
        }
        if (in.now - selectedSince_ >= kSelectedWait) {
            std::snprintf(line, sizeof(line),
                          "Select Target: done in %.2f s: its plate shows monster %u as the target, but no target "
                          "window was seen",
                          took, static_cast<unsigned>(chosen_));
            out.log = out.log.empty() ? std::string(line) : out.log + " | " + line;
            out.done = true;
        }
        return out;
    }
    selectedSince_ = -1.0;

    // Its plate not seen coloured (hidden, behind a tree), but the target window shows a new target since the click.
    if (clickedAt_ >= 0.0 && in.seen.image != 0 && in.seen.shown > clickedAt_ && newTarget(beforeClick_, in.seen)) {
        const std::string& target = in.seen.targetName;
        std::snprintf(line, sizeof(line),
                      "Select Target: done, %s targeted in %.2f s (%d click%s; seen by the target window, its name "
                      "plate %s)",
                      target.empty() ? "a new target" : target.c_str(), took, clicks_, plural(clicks_),
                      about && plate.seen ? "not coloured" : "not seen");
        out.log = out.log.empty() ? std::string(line) : out.log + " | " + line;
        out.done = true;
        return out;
    }

    // Hovered (its plate up, grey orbs, on an image shown since the cursor came onto it): clicked, unless a click is
    // still waiting for its sign.
    const bool waiting = clickedAt_ >= 0.0 && in.now - clickedAt_ < kClickWait;
    if (!waiting && insideSince_ >= 0.0 && about && plate.seen && in.seen.shown >= insideSince_) {
        if (clicks_ >= kClicks) {
            std::snprintf(line, sizeof(line), "%d clicks and never shown as the target", clicks_);
            giveUp(in.now, line, out);
            return out;
        }
        out.click = true;
        ++clicks_;
        clickedAt_ = in.now;
        beforeClick_ = in.seen;
        if (clicks_ > 1) {
            std::snprintf(line, sizeof(line), "Select Target: clicked monster %u again (click %d), no sign it was selected",
                          static_cast<unsigned>(chosen_), clicks_);
            out.log = out.log.empty() ? std::string(line) : out.log + " | " + line;
        }
        return out;
    }

    // Not shown hovered on the images shown kHoverWait after the cursor came onto it and after it last was: its plate
    // hidden (a tree) or the cursor not on it as the game sees it. Another one is tried.
    if (insideSince_ >= 0.0 && about && !plate.seen && in.seen.shown >= insideSince_ + kHoverWait &&
        in.seen.shown >= hoveredAt_ + kHoverWait) {
        // Never clicked yet: once all the same, the game may hover it with its plate hidden (the user's clips
        // 2026-11-16_14-22-00: tree leaves drawn over it; 14-25-14: under the Status window), and the target window
        // says (done above). Not with a living target: this one might be it, unseen, and a click would attack.
        const bool liveTarget = in.seen.hasTarget && !targetDead;
        if (clicks_ == 0 && !liveTarget) {
            out.click = true;
            ++clicks_;
            clickedAt_ = in.now;
            beforeClick_ = in.seen;
            std::snprintf(line, sizeof(line),
                          "Select Target: clicked monster %u without seeing its name plate (none over it for %.1f s, %d "
                          "orb%s round it: hidden?); the target window is to tell",
                          static_cast<unsigned>(chosen_), in.now - insideSince_, plate.orbs, plural(plate.orbs));
            out.log = out.log.empty() ? std::string(line) : out.log + " | " + line;
            return out;
        }
        if (waiting) {
            return out; // That click's sign awaited.
        }
        if (hoveredAt_ < 0.0 && clicks_ > 0) {
            std::snprintf(line, sizeof(line),
                          "the game never showed it hovered (no name plate over it, %d orb%s round it), and the click "
                          "made all the same brought no new target in the target window",
                          plate.orbs, plural(plate.orbs));
        } else if (hoveredAt_ < 0.0) {
            std::snprintf(line, sizeof(line),
                          "the cursor was on it %.1f s but the game never showed it hovered (no name plate over it, %d "
                          "orb%s round it; not clicked all the same, a living target in the target window)",
                          in.now - insideSince_, plate.orbs, plural(plate.orbs));
        } else {
            std::snprintf(line, sizeof(line),
                          "the game stopped showing it hovered (no name plate over it for %.1f s, %d click%s)",
                          in.seen.shown - hoveredAt_, clicks_, plural(clicks_));
        }
        giveUp(in.now, line, out);
    }
    return out;
}

} // namespace observex
