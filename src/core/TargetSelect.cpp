#include "core/TargetSelect.h"

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

} // namespace

void TargetSelector::start() { *this = TargetSelector(); }

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
    chosen_ = 0;
    out.look = {};
}

void TargetSelector::choose(const Input& in, std::string& log) {
    // Those given up on are tried again kRetryAfter later; until then, followed where they are seen.
    std::erase_if(givenUp_, [&](const GivenUp& g) { return in.now - g.at >= kRetryAfter; });
    for (GivenUp& g : givenUp_) {
        if (const SelectableMonster* seen = find(in.monsters, g.id)) {
            g.box = seen->box;
        }
    }
    char line[200];
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
    // The biggest (no distances yet), of those not given up on.
    const SelectableMonster* best = nullptr;
    int skipped = 0;
    for (const SelectableMonster& m : in.monsters) {
        if (givenUpOn(m, in.monsters)) {
            ++skipped;
            continue;
        }
        if (best == nullptr || static_cast<long>(m.w) * m.h > static_cast<long>(best->w) * best->h) {
            best = &m;
        }
    }
    if (best == nullptr) {
        if (!saidNone_) {
            if (skipped == 0) {
                std::snprintf(line, sizeof(line), "Select Target: no monster to select on screen");
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
    chosen_ = best->id;
    last_ = *best;
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
    std::snprintf(line, sizeof(line), "Select Target: going for monster %u (%d x %d px, the biggest of %d)",
                  static_cast<unsigned>(chosen_), best->w, best->h, static_cast<int>(in.monsters.size()));
    log = log.empty() ? std::string(line) : log + " | " + line;
}

TargetSelector::Output TargetSelector::step(const Input& in) {
    Output out;
    if (started_ < 0.0) {
        started_ = in.now;
    }
    choose(in, out.log);
    if (chosen_ == 0) {
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
        if (hoveredAt_ < 0.0) {
            std::snprintf(line, sizeof(line),
                          "the cursor was on it %.1f s but the game never showed it hovered (no name plate over it, %d "
                          "orb%s round it)",
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
