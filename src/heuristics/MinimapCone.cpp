#include "heuristics/MinimapCone.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace observex {

namespace {

constexpr float kTwoPi = 6.28318531f;

// The grey of `level` at (`x`, `y`), between the four pixels around; false off it.
bool greyAt(const GreyLevel& level, float x, float y, float& grey) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);
    if (level.pixels == nullptr || ix < 0 || iy < 0 || ix + 1 >= level.width || iy + 1 >= level.height) {
        return false;
    }
    const float tx = x - fx;
    const float ty = y - fy;
    const std::uint8_t* top = level.pixels + level.stride * iy + ix;
    const std::uint8_t* bottom = top + level.stride;
    const float upper = static_cast<float>(top[0]) + tx * static_cast<float>(top[1] - top[0]);
    const float lower = static_cast<float>(bottom[0]) + tx * static_cast<float>(bottom[1] - bottom[0]);
    grey = upper + ty * (lower - upper);
    return true;
}

// The steps of a profile `steps` round that make `radians`.
int stepsOf(float radians, int steps) {
    return static_cast<int>(std::lround(radians / kTwoPi * static_cast<float>(steps)));
}

} // namespace

int MinimapCone::areas(double timeMs, std::span<const GreyArea> windows, GreyArea* out, int max) {
    timeMs_ = timeMs;
    const int around = static_cast<int>(std::ceil(kOuter)) + 2;
    if (locked_) {
        candidateCount_ = 0;
        if (max < 1) {
            return 0;
        }
        const auto x = static_cast<int>(std::lround(tipX_));
        const auto y = static_cast<int>(std::lround(tipY_));
        out[0] = {x - around, y - around, x + around + 1, y + around + 1};
        return 1;
    }
    candidateCount_ = 0;
    if (timeMs < retryMs_) {
        return 0;
    }
    int count = 0;
    for (const GreyArea& window : windows) {
        const int wide = window.x1 - window.x0;
        const int high = window.y1 - window.y0;
        if (wide < kLeastSide || high < kLeastSide ||
            static_cast<float>(std::max(wide, high)) > kMostLong * static_cast<float>(std::min(wide, high)) ||
            candidateCount_ == kCandidates || count == max) {
            continue;
        }
        const float x = 0.5f * static_cast<float>(window.x0 + window.x1) - 1.0f;
        const float y = 0.5f * static_cast<float>(window.y0 + kTitle + window.y1);
        candidates_[static_cast<std::size_t>(2 * candidateCount_)] = x;
        candidates_[static_cast<std::size_t>(2 * candidateCount_ + 1)] = y;
        ++candidateCount_;
        const int reach = around + kReach + 1;
        const auto ix = static_cast<int>(std::lround(x));
        const auto iy = static_cast<int>(std::lround(y));
        out[count++] = {ix - reach, iy - reach, ix + reach + 1, iy + reach + 1};
    }
    return count;
}

bool MinimapCone::read(const GreyLevel& full) {
    seen_ = false;
    if (locked_) {
        if (readAt(full, tipX_, tipY_)) {
            missSinceMs_ = -1.0;
            return true;
        }
        if (missSinceMs_ < 0.0) {
            missSinceMs_ = timeMs_;
        } else if (timeMs_ - missSinceMs_ >= kMissMs) {
            locked_ = false; // Looked for again on the next image.
            missSinceMs_ = -1.0;
        }
        return false;
    }
    for (int i = 0; i < candidateCount_; ++i) {
        if (findTip(full, candidates_[static_cast<std::size_t>(2 * i)],
                    candidates_[static_cast<std::size_t>(2 * i + 1)])) {
            locked_ = true;
            missSinceMs_ = -1.0;
            return readAt(full, tipX_, tipY_);
        }
    }
    if (candidateCount_ > 0) {
        retryMs_ = timeMs_ + kRetryMs;
    }
    candidateCount_ = 0;
    return false;
}

bool MinimapCone::profile(const GreyLevel& full, float x, float y, int steps, float radiusStep) {
    profile_.assign(static_cast<std::size_t>(steps), 0.0f);
    const int radii = static_cast<int>((kOuter - kInner) / radiusStep) + 1;
    for (int i = 0; i < steps; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(steps);
        const float s = std::sin(angle);
        const float c = std::cos(angle);
        float sum = 0.0f;
        for (int r = 0; r < radii; ++r) {
            const float radius = kInner + radiusStep * static_cast<float>(r);
            float grey = 0.0f;
            if (!greyAt(full, x + radius * s, y - radius * c, grey)) {
                return false;
            }
            sum += grey;
        }
        profile_[static_cast<std::size_t>(i)] = sum / static_cast<float>(radii);
    }
    return true;
}

MinimapCone::Edges MinimapCone::edges() {
    const int n = static_cast<int>(profile_.size());
    const auto at = [&](int i) { return profile_[static_cast<std::size_t>(((i % n) + n) % n)]; };
    // How fast the grey rises going clockwise at each step (in greys, for a sharp edge: its height).
    std::vector<float>& rises = rises_;
    rises.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        rises[static_cast<std::size_t>(i)] = (2.0f * (at(i + 1) - at(i - 1)) + (at(i + 2) - at(i - 2))) / 3.0f;
    }
    const auto rise = [&](int i) { return rises[static_cast<std::size_t>(((i % n) + n) % n)]; };
    // The rise at i and the fall kWidth on (give or take kWidthSlack) that differ most.
    const int least = stepsOf(kWidth - kWidthSlack, n);
    const int most = stepsOf(kWidth + kWidthSlack, n);
    int bestRise = 0;
    int bestFall = 0;
    float best = -1e30f;
    for (int i = 0; i < n; ++i) {
        const float up = rise(i);
        if (up <= 0.0f) {
            continue;
        }
        for (int gap = least; gap <= most; ++gap) {
            if (const float sharp = up - rise(i + gap); sharp > best) {
                best = sharp;
                bestRise = i;
                bestFall = i + gap;
            }
        }
    }
    Edges found;
    if (best <= 0.0f) {
        return found;
    }
    // A fraction of a step from the rises either side (a parabola through the three).
    const auto between = [&](int i, float sign) {
        const float left = sign * rise(i - 1);
        const float middle = sign * rise(i);
        const float right = sign * rise(i + 1);
        const float curve = left - 2.0f * middle + right;
        return static_cast<float>(i) + (curve < -1e-6f ? std::clamp(0.5f * (left - right) / curve, -0.5f, 0.5f) : 0.0f);
    };
    found.rise = between(bestRise, 1.0f);
    found.fall = between(bestFall, -1.0f);
    found.strength = best;
    // Lighter inside than beside it (a fifth of the cone's width each side).
    const int beside = std::max(1, (bestFall - bestRise) / 5);
    float inside = 0.0f;
    for (int i = bestRise + 1; i < bestFall; ++i) {
        inside += at(i);
    }
    inside /= static_cast<float>(std::max(1, bestFall - bestRise - 1));
    float outside = 0.0f;
    for (int i = 1; i <= beside; ++i) {
        outside += at(bestRise - i) + at(bestFall + i);
    }
    outside /= static_cast<float>(2 * beside);
    found.seen = rise(bestRise) >= kMinEdge && -rise(bestFall) >= kMinEdge && inside - outside >= kMinContrast;
    return found;
}

bool MinimapCone::findTip(const GreyLevel& full, float x, float y) {
    // Coarsely first (every 2 degrees and 2 pixels out), on every whole pixel round where the window puts it; then
    // finely, every half pixel round the sharpest.
    float bestX = x;
    float bestY = y;
    float best = -1.0f;
    for (int dy = -kReach; dy <= kReach; ++dy) {
        for (int dx = -kReach; dx <= kReach; ++dx) {
            const float atX = std::round(x) + static_cast<float>(dx);
            const float atY = std::round(y) + static_cast<float>(dy);
            if (!profile(full, atX, atY, 180, 2.0f)) {
                continue;
            }
            if (const Edges found = edges(); found.seen && found.strength > best) {
                best = found.strength;
                bestX = atX;
                bestY = atY;
            }
        }
    }
    if (best < 0.0f) {
        return false;
    }
    const float coarseX = bestX;
    const float coarseY = bestY;
    best = -1.0f;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const float atX = coarseX + 0.5f * static_cast<float>(dx);
            const float atY = coarseY + 0.5f * static_cast<float>(dy);
            if (!profile(full, atX, atY, 720, 1.0f)) {
                continue;
            }
            if (const Edges found = edges(); found.seen && found.strength > best) {
                best = found.strength;
                bestX = atX;
                bestY = atY;
            }
        }
    }
    if (best < 0.0f) {
        return false;
    }
    tipX_ = bestX;
    tipY_ = bestY;
    return true;
}

bool MinimapCone::readAt(const GreyLevel& full, float x, float y) {
    constexpr int kSteps = 720; // Half degrees.
    if (!profile(full, x, y, kSteps, 1.0f)) {
        return false;
    }
    const Edges found = edges();
    if (!found.seen) {
        return false;
    }
    const float width = found.fall - found.rise;
    const float heading = (found.rise + 0.5f * width) * kTwoPi / static_cast<float>(kSteps);
    heading_ = std::fmod(std::fmod(heading, kTwoPi) + kTwoPi, kTwoPi);
    seen_ = true;
    return true;
}

} // namespace observex
