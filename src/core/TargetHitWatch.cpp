#include "core/TargetHitWatch.h"

#include <cmath>
#include <cstdio>

namespace observex {

std::optional<TargetHitWatch::Hit> TargetHitWatch::read(const Reading& r) {
    // The own hits: MP taken, a cast ended.
    if (r.mp >= 0) {
        if (mp_ >= 0 && r.mp < mp_) {
            ours_ = true;
            oursSeconds_ = r.seconds;
        }
        mp_ = r.mp;
    }
    if (casting_ && !r.casting) {
        ours_ = true;
        oursSeconds_ = r.seconds;
    }
    casting_ = r.casting;

    // Another target (or none): afresh, a fall not said yet dropped.
    if (r.target != target_) {
        target_ = r.target;
        held_ = -1.0f;
        seen_ = -1.0f;
        falling_ = false;
    }
    if (r.hasTarget && r.targetHp >= 0.0f) {
        const float hp = r.targetHp;
        if (held_ < 0.0f) {
            held_ = hp; // The first only starts it.
        } else if (std::fabs(hp - held_) <= kSame) {
            seen_ = -1.0f;
        } else if (seen_ >= 0.0f && std::fabs(hp - seen_) <= kSame) {
            // Read twice in a row: taken.
            if (hp <= held_ - kMinFall) {
                if (!falling_) {
                    falling_ = true;
                    fallFrom_ = held_;
                    fallFirst_ = seenSeconds_;
                }
                fallTo_ = hp;
                fallLast_ = seenSeconds_;
            }
            held_ = hp;
            seen_ = -1.0f;
        } else {
            seen_ = hp;
            seenSeconds_ = r.seconds;
        }
    }

    if (!falling_) {
        return std::nullopt;
    }
    if (ours_ && oursSeconds_ >= fallFirst_ - kOurs) {
        falling_ = false; // The own MP taken or cast ended near it: the own hit.
        return std::nullopt;
    }
    if (r.seconds - fallLast_ < kOurs) {
        return std::nullopt; // The own sign may still come.
    }
    falling_ = false;
    char text[96];
    std::snprintf(text, sizeof(text), "Target hit by others: %s %d%% to %d%%",
                  target_.empty() ? "(name not read)" : target_.c_str(), static_cast<int>(std::lround(fallFrom_ * 100.0f)),
                  static_cast<int>(std::lround(fallTo_ * 100.0f)));
    return Hit{fallFrom_, fallTo_, fallFirst_, text};
}

} // namespace observex
