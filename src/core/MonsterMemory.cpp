#include "core/MonsterMemory.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace observex {

float MonsterMemory::Monster::distance() const {
    return std::hypot(x, y);
}

void MonsterMemory::relative(const Monster& monster, double heading, float& right, float& away) {
    const double c = std::cos(heading);
    const double s = std::sin(heading);
    right = static_cast<float>(monster.x * c - monster.y * s);
    away = static_cast<float>(monster.x * s + monster.y * c);
}

float MonsterMemory::bearing(const Monster& monster, double heading) {
    float right = 0.0f;
    float away = 0.0f;
    relative(monster, heading, right, away);
    return std::atan2(right, away);
}

void MonsterMemory::look(double now, double heading, const GroundCamera& camera, float width, float height,
                         std::span<const MonsterSighting> seen) {
    // Each sighting into the frame that does not turn: the camera turned `heading` right, so what it shows straight
    // ahead stands `heading` right of turn 0's straight ahead.
    const double c = std::cos(heading);
    const double s = std::sin(heading);
    struct Spot {
        float x = 0.0f;
        float y = 0.0f;
        int monster = -1; // The one it is (index), -1: none yet.
    };
    std::vector<Spot> spots;
    spots.reserve(seen.size());
    for (const MonsterSighting& one : seen) {
        spots.push_back({static_cast<float>(one.right * c + one.away * s),
                         static_cast<float>(-one.right * s + one.away * c), -1});
    }
    std::vector<bool> taken(monsters_.size(), false);
    // The same tracker id first.
    for (std::size_t i = 0; i < seen.size(); ++i) {
        for (std::size_t m = 0; m < monsters_.size(); ++m) {
            if (!taken[m] && monsters_[m].id == seen[i].id) {
                taken[m] = true;
                spots[i].monster = static_cast<int>(m);
                break;
            }
        }
    }
    // Then the nearest one not seen on this look, near enough.
    for (Spot& spot : spots) {
        if (spot.monster >= 0) {
            continue;
        }
        const float reach = std::max(kMatchNear, kMatchShare * std::hypot(spot.x, spot.y));
        float best = std::numeric_limits<float>::max();
        int nearest = -1;
        for (std::size_t m = 0; m < monsters_.size(); ++m) {
            const float off = std::hypot(monsters_[m].x - spot.x, monsters_[m].y - spot.y);
            if (!taken[m] && off <= reach && off < best) {
                best = off;
                nearest = static_cast<int>(m);
            }
        }
        if (nearest >= 0) {
            taken[static_cast<std::size_t>(nearest)] = true;
            spot.monster = nearest;
        }
    }
    for (std::size_t i = 0; i < spots.size(); ++i) {
        Spot& spot = spots[i];
        if (spot.monster < 0) {
            Monster one;
            one.key = nextKey_++;
            monsters_.push_back(one);
            taken.push_back(true);
            spot.monster = static_cast<int>(monsters_.size() - 1);
        }
        Monster& monster = monsters_[static_cast<std::size_t>(spot.monster)];
        monster.id = seen[i].id;
        monster.x = spot.x;
        monster.y = spot.y;
        monster.seenAt = now;
        monster.missingSince = -1.0;
    }
    // Those not seen: gone once their spot was in view and empty kGoneSeconds; forgotten kForgetSeconds after last seen.
    for (std::size_t m = 0; m < monsters_.size(); ++m) {
        Monster& monster = monsters_[m];
        if (taken[m]) {
            continue;
        }
        float right = 0.0f;
        float away = 0.0f;
        relative(monster, heading, right, away);
        const GroundPoint at = groundToScreen(camera, right, away);
        const bool inView = camera.known && at.ahead && monster.distance() <= kSureView &&
                            at.x >= kViewMargin * width && at.x <= (1.0f - kViewMargin) * width &&
                            at.y >= kViewMargin * height && at.y <= (1.0f - kViewMargin) * height;
        if (!inView) {
            monster.missingSince = -1.0;
        } else if (monster.missingSince < 0.0) {
            monster.missingSince = now;
        }
    }
    std::erase_if(monsters_, [&](const Monster& monster) {
        return (monster.missingSince >= 0.0 && now - monster.missingSince >= kGoneSeconds) ||
               now - monster.seenAt >= kForgetSeconds;
    });
}

void MonsterMemory::clear() {
    monsters_.clear();
}

void MonsterMemory::giveUp(std::uint32_t key, bool now) {
    for (Monster& monster : monsters_) {
        if (monster.key == key) {
            ++monster.giveUps;
            monster.givenUp = monster.givenUp || now || monster.giveUps >= kGiveUps;
        }
    }
}

void MonsterMemory::forget(std::uint32_t key) {
    std::erase_if(monsters_, [&](const Monster& monster) { return monster.key == key; });
}

const MonsterMemory::Monster* MonsterMemory::withKey(std::uint32_t key) const {
    const auto it = std::ranges::find(monsters_, key, &Monster::key);
    return it != monsters_.end() ? &*it : nullptr;
}

const MonsterMemory::Monster* MonsterMemory::withId(std::uint32_t id) const {
    const auto it = std::ranges::find(monsters_, id, &Monster::id);
    return id != 0 && it != monsters_.end() ? &*it : nullptr;
}

int MonsterMemory::count() const {
    return static_cast<int>(std::ranges::count_if(monsters_, [](const Monster& monster) { return !monster.givenUp; }));
}

const MonsterMemory::Monster* MonsterMemory::nearest() const {
    const Monster* best = nullptr;
    for (const Monster& monster : monsters_) {
        if (!monster.givenUp && (best == nullptr || monster.distance() < best->distance())) {
            best = &monster;
        }
    }
    return best;
}

} // namespace observex
