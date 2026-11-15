#include "core/SkillUse.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace observex {

namespace {

bool sameText(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::string seconds(double s) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f s", s);
    return text;
}

std::string points(float mp) {
    return std::to_string(static_cast<int>(mp + 0.5f));
}

} // namespace

void SkillUser::start() {
    *this = SkillUser();
}

void SkillUser::finish(Output& out, std::string why) {
    done_ = true;
    out.done = true;
    out.log = "Use Skill " + chosen_.name + ": " + why;
}

std::string SkillUser::pressesNote() const {
    return presses_ > 1 ? ", after " + std::to_string(presses_) + " presses over " +
                              seconds(pressedAt_ - firstPressAt_)
                        : std::string();
}

void SkillUser::press(const Input& in, int index, Output& out) {
    out.press = index;
    ++presses_;
    if (firstPressAt_ < 0.0) {
        firstPressAt_ = in.now;
    }
    pressedAt_ = in.now;
    lastShown_ = -1.0;
    castAt_ = -1.0;
    coolingAt_ = -1.0;
    // The MP before it: the most from here on is what its cost comes off.
    mpTop_ = in.variables != nullptr && in.variables->mpPointsKnown ? in.variables->mpPoints : -1.0f;
}

SkillUser::Output SkillUser::step(const Input& in) {
    Output out;
    if (done_) {
        out.done = true;
        return out;
    }
    const bool fresh = in.variables != nullptr && in.image != 0 && in.image != lastImage_;
    if (fresh) {
        lastImage_ = in.image;
    }

    // 1. Choose the first ready one, top to bottom, and press it.
    if (chosen_.name.empty()) {
        if (in.variables == nullptr) {
            return out; // Nothing read yet.
        }
        int pick = -1;
        std::string notReady;
        std::string unpressable;
        for (std::size_t i = 0; i < in.skills.size() && pick < 0; ++i) {
            const UsableSkill& skill = in.skills[i];
            if (!statetree::isReady(*in.variables, skill.name)) {
                notReady += (notReady.empty() ? "" : ", ") + skill.name;
            } else if (!skill.pressable) {
                unpressable += (unpressable.empty() ? "" : ", ") + skill.name + " ('" + skill.key + "')";
            } else {
                pick = static_cast<int>(i);
            }
        }
        if (pick < 0) {
            if (!said_) {
                said_ = true;
                out.log = in.skills.empty()     ? std::string("Use Skill: no skill chosen, the task waits")
                          : !unpressable.empty() ? "Use Skill: can't press the shortcut of " + unpressable +
                                                       ", the task waits"
                                                 : "Use Skill: " + notReady + " not ready, the task waits";
            }
            return out;
        }
        if (!in.canPress) {
            return out;
        }
        chosen_ = in.skills[static_cast<std::size_t>(pick)];
        press(in, pick, out);
        out.log = "Use Skill " + chosen_.name + ": pressed " + chosen_.key +
                  (notReady.empty() ? std::string() : " (not ready: " + notReady + ")");
        if (!chosen_.cooldown && chosen_.mp <= 0) {
            done_ = true; // Nothing to watch it by.
            out.done = true;
            out.log += ", done (no cooldown or MP cost to watch)";
        }
        return out;
    }

    // 2. Watch the readings of images shown after the press.
    if (fresh && in.shown >= pressedAt_) {
        const statetree::Variables& v = *in.variables;
        lastShown_ = in.shown;
        // Its MP cost taken (done), or the most MP since the press (regeneration before the cost comes off).
        if (v.mpPointsKnown) {
            if (chosen_.mp > 0 && mpTop_ >= 0.0f &&
                mpTop_ - v.mpPoints >= kMpShare * static_cast<float>(chosen_.mp)) {
                finish(out, "done, its MP taken (" + points(mpTop_) + " -> " + points(v.mpPoints) + ", it costs " +
                                std::to_string(chosen_.mp) + pressesNote() + ")");
                return out;
            }
            mpTop_ = std::max(mpTop_, v.mpPoints);
        }
        // Its cast: the cast bar with its name (or a cast not named: the bar's skill is the one whose cooldown began
        // last, unknown for one without), until it shows no cast.
        const bool ours = v.castShown && (v.casting.empty() || sameText(v.casting, chosen_.name));
        if (ours && castAt_ < 0.0) {
            castAt_ = in.shown;
        } else if (!ours && castAt_ >= 0.0) {
            finish(out, "done, its cast ended (" + seconds(in.shown - castAt_) + " after its cast bar showed" +
                            pressesNote() + ")");
            return out;
        }
        // Its pressed look is no sign: the game flashes the slot also when it refuses the skill.
        const statetree::SkillSeen* seen = v.skill(chosen_.name);
        const bool cooling = seen != nullptr && seen->found && !seen->pressed && seen->cooldown > 0.0f;
        if (cooling && castAt_ < 0.0 && coolingAt_ < 0.0) {
            coolingAt_ = in.shown;
        }
    }

    // 3. When to stop waiting.
    if (firstPressAt_ >= 0.0 && in.now - firstPressAt_ >= kMostSeconds) {
        finish(out, "gave up waiting, no cast end or MP taken seen in " + seconds(kMostSeconds));
        return out;
    }
    if (castAt_ >= 0.0) {
        if (lastShown_ - castAt_ >= kCastSeconds) {
            finish(out, "done, its cast bar did not end in " + seconds(kCastSeconds));
        }
        return out;
    }
    if (coolingAt_ >= 0.0) {
        const bool mpWatched = chosen_.mp > 0 && mpTop_ >= 0.0f;
        if (mpWatched && lastShown_ - coolingAt_ >= kMpWait) {
            finish(out, "done, its cooldown began but its MP was not seen taken" + pressesNote());
        } else if (!mpWatched && lastShown_ - coolingAt_ >= kInstantWait) {
            finish(out, "done, its cooldown began with no cast bar (instant" + pressesNote() + ")");
        }
        return out;
    }
    // No sign the game took it yet: given up on kGiveUpSeconds after the first press, else pressed again on every
    // reading of an image shown kRepress after the last press.
    if (lastShown_ >= 0.0 && lastShown_ - firstPressAt_ >= kGiveUpSeconds) {
        finish(out, "gave up, pressed " + std::to_string(presses_) + " times in " + seconds(lastShown_ - firstPressAt_) +
                        " and the game never showed it taken");
        return out;
    }
    if (lastShown_ >= 0.0 && lastShown_ - pressedAt_ >= kRepress) {
        const auto it = std::ranges::find_if(in.skills, [&](const UsableSkill& s) { return s.name == chosen_.name; });
        if (it == in.skills.end() || !it->pressable) {
            finish(out, "gave up, no longer in the task or its shortcut can't be pressed");
            return out;
        }
        if (in.canPress) {
            press(in, static_cast<int>(it - in.skills.begin()), out);
            if (presses_ == 2) { // Said once; the line it ends with says how many it took.
                char again[64];
                std::snprintf(again, sizeof(again), "pressing it every %.1f s until it does", kRepress);
                out.log = "Use Skill " + chosen_.name + ": pressed " + chosen_.key +
                          " again, no sign the game took it; " + again;
            }
        }
    }
    return out;
}

} // namespace observex
