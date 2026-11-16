#include "core/DropPick.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace observex {

namespace {

constexpr float kOwnHeightUnits = 47.0f; // The own character's height in game units (core/Distances' kGameUnits).
constexpr float kFar = 1e9f;             // How far a drop is when it cannot be told.

std::string units(float away, bool known) {
    char text[48];
    std::snprintf(text, sizeof(text), "%s%d units", known ? "" : "about ", static_cast<int>(std::lround(away / 10.0f)) * 10);
    return text;
}

} // namespace

bool samePlace(const LootDrop& a, const LootDrop& b) {
    if (!a.placed || !b.placed) {
        return false;
    }
    const float apart = std::hypot(a.fixedX - b.fixedX, a.fixedY - b.fixedY);
    const float farther = std::max(std::hypot(a.fixedX, a.fixedY), std::hypot(b.fixedX, b.fixedY));
    return apart <= std::max(kSameNear, kSameShare * farther);
}

bool DropBook::givenUp(const LootDrop& drop, double now) const {
    return std::any_of(givenUp_.begin(), givenUp_.end(),
                       [&](const Mark& mark) { return now < mark.since && samePlace(mark.drop, drop); });
}

void DropBook::giveUp(const LootDrop& drop, double now) {
    if (drop.placed) {
        givenUp_.push_back({drop, now + kGiveUpSeconds, now});
    }
    std::erase_if(counted_, [&](const LootDrop& d) {
        return (drop.placed && samePlace(d, drop)) || (d.x == drop.x && d.y == drop.y);
    });
    if (counted_.empty()) {
        held_ = 0; // Not held: what is given up on no longer counts at once.
    } else {
        held_ = static_cast<int>(counted_.size());
    }
    nearest_ = -1.0f;
    for (const LootDrop& d : counted_) {
        if (d.distance >= 0.0f && (nearest_ < 0.0f || d.distance < nearest_)) {
            nearest_ = d.distance;
        }
    }
}

DropBook::Output DropBook::step(const Input& in) {
    Output out;
    std::erase_if(givenUp_, [&](const Mark& mark) { return in.now >= mark.since; });
    if (in.drops.empty()) {
        counted_.clear(); // No close look shows drops now (none there, or the spot ended): nothing to walk to.
        nearest_ = -1.0f;
    } else if (in.look != 0 && in.look != look_) {
        look_ = in.look;
        counted_.clear();
        for (const LootDrop& drop : in.drops) {
            if (!givenUp(drop, in.now)) {
                counted_.push_back(drop);
            }
        }
        // Stuck: within reach this long while looting, still there.
        if (in.looting) {
            std::vector<Mark> close;
            std::vector<LootDrop> stuck;
            std::vector<bool> used(near_.size(), false);
            for (const LootDrop& drop : counted_) {
                if (!drop.placed || drop.distance < 0.0f || drop.distance > static_cast<float>(in.reach)) {
                    continue;
                }
                double since = in.now;
                for (std::size_t i = 0; i < near_.size(); ++i) {
                    if (!used[i] && samePlace(near_[i].drop, drop)) {
                        used[i] = true;
                        since = near_[i].since;
                        break;
                    }
                }
                if (in.now - since >= kStuckSeconds) {
                    stuck.push_back(drop);
                } else {
                    close.push_back({drop, since, in.now});
                }
            }
            // One not seen on this look (under the own character, or a look that missed it) is kept a while.
            for (std::size_t i = 0; i < near_.size(); ++i) {
                if (!used[i] && in.now - near_[i].seen <= kHoldSeconds) {
                    close.push_back(near_[i]);
                }
            }
            near_ = std::move(close);
            for (const LootDrop& drop : stuck) {
                giveUp(drop, in.now);
                char line[200];
                std::snprintf(line, sizeof(line),
                              "Looting: gave up on a drop %s away, still there after %.0f s within reach (not ours to "
                              "pick up, or not a drop); left out for %.0f s",
                              units(drop.distance, true).c_str(), kStuckSeconds, kGiveUpSeconds);
                out.log += (out.log.empty() ? "" : "\n") + std::string(line);
            }
        } else {
            near_.clear();
        }
        nearest_ = -1.0f;
        for (const LootDrop& drop : counted_) {
            if (drop.distance >= 0.0f && (nearest_ < 0.0f || drop.distance < nearest_)) {
                nearest_ = drop.distance;
            }
        }
        if (!counted_.empty()) {
            held_ = static_cast<int>(counted_.size());
            heldAt_ = in.now;
        }
    }
    if (!in.looting) {
        near_.clear();
    }
    out.count = in.now - heldAt_ <= kHoldSeconds ? held_ : 0;
    out.nearest = nearest_;
    out.counted = counted_;
    return out;
}

void DropPicker::start() {
    *this = {};
}

float DropPicker::howFar(const LootDrop& drop, const Input& in) const {
    if (drop.distance >= 0.0f) {
        return drop.distance;
    }
    if (in.own && in.ownHeight > 0.0f) {
        const float pixels = std::hypot(static_cast<float>(drop.deskX - in.ownX), static_cast<float>(drop.deskY - in.ownY));
        return pixels / in.ownHeight * kOwnHeightUnits;
    }
    return kFar;
}

bool DropPicker::withinReach(const LootDrop& drop, const Input& in) const {
    return howFar(drop, in) <= static_cast<float>(in.reach);
}

bool DropPicker::clickPoint(const LootDrop& drop, const Input& in, int& x, int& y) const {
    const auto covered = [&in](int px, int py) {
        return std::any_of(in.characters.begin(), in.characters.end(),
                           [&](const Box& box) { return px >= box.x0 && px <= box.x1 && py >= box.y0 && py <= box.y1; });
    };
    if (!covered(drop.deskX, drop.deskY)) {
        x = drop.deskX;
        y = drop.deskY;
        return true;
    }
    if (!in.own) {
        return false;
    }
    // Along the way back to the own feet, in steps of 5% of it.
    for (float t = 0.05f; t <= kShortOf + 1e-4f; t += 0.05f) {
        const int px = static_cast<int>(std::lround(static_cast<float>(drop.deskX) + t * static_cast<float>(in.ownX - drop.deskX)));
        const int py = static_cast<int>(std::lround(static_cast<float>(drop.deskY) + t * static_cast<float>(in.ownY - drop.deskY)));
        if (!covered(px, py)) {
            x = px;
            y = py;
            return true;
        }
    }
    return false;
}

void DropPicker::giveUp(const std::string& why, Output& out) {
    out.done = true;
    out.gaveUp = true;
    out.givenUp = last_;
    out.log += (out.log.empty() ? "" : "\n") + std::string("Move To Drop: gave up on the drop, ") + why;
}

DropPicker::Output DropPicker::step(const Input& in) {
    Output out;
    if (chosen_) {
        // The chosen drop on this look: the nearest to where it was last seen on the screen, or at the same place round
        // the own feet (the camera turned).
        const float radius = std::max(kMatchPixels, 4.0f * last_.size);
        const LootDrop* best = nullptr;
        float bestApart = kFar;
        for (const LootDrop& drop : in.drops) {
            const float apart = std::hypot(drop.x - last_.x, drop.y - last_.y);
            if ((apart <= radius || samePlace(drop, last_)) && apart < bestApart) {
                best = &drop;
                bestApart = apart;
            }
        }
        if (best != nullptr) {
            last_ = *best;
            lastSeen_ = in.now;
        } else if (in.now - lastSeen_ >= kGoneSeconds) {
            out.done = true;
            if (clicks_ > 0) {
                char line[200];
                std::snprintf(line, sizeof(line),
                              "Move To Drop: the drop is gone (picked up, or under the own character), %.1f s after "
                              "choosing it (%d click%s)",
                              in.now - chosenAt_, clicks_, clicks_ == 1 ? "" : "s");
                out.log = line;
            }
            return out;
        } else {
            return out; // Not on this look: waits a moment where it was.
        }
    } else {
        // The nearest: those with a distance first, then by the screen.
        const LootDrop* best = nullptr;
        for (const LootDrop& drop : in.drops) {
            if (best == nullptr) {
                best = &drop;
                continue;
            }
            const bool known = drop.distance >= 0.0f;
            const bool bestKnown = best->distance >= 0.0f;
            if ((known && !bestKnown) || (known == bestKnown && howFar(drop, in) < howFar(*best, in))) {
                best = &drop;
            }
        }
        if (best == nullptr) {
            out.done = true; // Nothing to walk to.
            return out;
        }
        chosen_ = true;
        last_ = *best;
        lastSeen_ = in.now;
        chosenAt_ = in.now;
        if (withinReach(last_, in)) {
            out.done = true; // There already: nothing to walk.
            return out;
        }
        char line[200];
        std::snprintf(line, sizeof(line), "Move To Drop: going for a drop %s away (the nearest of %zu)",
                      units(howFar(last_, in), last_.distance >= 0.0f).c_str(), in.drops.size());
        out.log = line;
    }

    if (withinReach(last_, in)) {
        out.done = true;
        if (clicks_ > 0) {
            char line[200];
            std::snprintf(line, sizeof(line), "Move To Drop: there, the drop %s away, %.1f s after choosing it (%d click%s)",
                          units(howFar(last_, in), last_.distance >= 0.0f).c_str(), in.now - chosenAt_, clicks_,
                          clicks_ == 1 ? "" : "s");
            out.log = line;
        }
        return out;
    }
    if (in.now - chosenAt_ >= kGiveUp) {
        char why[160];
        std::snprintf(why, sizeof(why), "not there %.0f s after choosing it (%d click%s, still %s away)", kGiveUp, clicks_,
                      clicks_ == 1 ? "" : "s", units(howFar(last_, in), last_.distance >= 0.0f).c_str());
        giveUp(why, out);
        return out;
    }
    bool again = false;
    if (clickedAt_ >= 0.0) {
        if (in.now - clickedAt_ < kRetry) {
            return out; // Walking there.
        }
        const float away = howFar(last_, in);
        if (away <= kNearer * farAtClick_) {
            clickedAt_ = in.now; // Nearer: it walks on.
            farAtClick_ = away;
            return out;
        }
        if (clicks_ >= kClicks) {
            char why[160];
            std::snprintf(why, sizeof(why), "%d clicks and it came no nearer (%s away)", clicks_,
                          units(away, last_.distance >= 0.0f).c_str());
            giveUp(why, out);
            return out;
        }
        again = true;
    }
    int x = 0;
    int y = 0;
    if (!clickPoint(last_, in, x, y)) {
        giveUp("a character stands over it and over the ground short of it", out);
        return out;
    }
    if (std::abs(x - in.cursorX) <= kOnPoint && std::abs(y - in.cursorY) <= kOnPoint) {
        out.click = true;
        ++clicks_;
        clickedAt_ = in.now;
        farAtClick_ = howFar(last_, in);
        glide_.stop();
        if (again) {
            char line[160];
            std::snprintf(line, sizeof(line), "Move To Drop: clicked again (click %d), it came no nearer in %.1f s",
                          clicks_, kRetry);
            out.log += (out.log.empty() ? "" : "\n") + std::string(line);
        }
        return out;
    }
    const CursorGlide::Move move = glide_.step(x - in.cursorX, y - in.cursorY, in.dt);
    out.dx = move.dx;
    out.dy = move.dy;
    return out;
}

} // namespace observex
