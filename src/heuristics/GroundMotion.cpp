#include "heuristics/GroundMotion.h"

#include <algorithm>
#include <cmath>

namespace observex {

namespace {

// The full-size pixel place `full` on `level` (pixel middles at whole numbers there).
float onLevel(float full, int level) {
    return full / static_cast<float>(1 << level) - 0.5f;
}

// The grey of `level` at (`x`, `y`) in its own pixels, between the four pixels around; false off it.
bool greyAt(const GreyLevel& level, float x, float y, float& grey) {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int ix = static_cast<int>(fx);
    const int iy = static_cast<int>(fy);
    if (ix < 0 || iy < 0 || ix + 1 >= level.width || iy + 1 >= level.height) {
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

// `values` less their mean, over the root of their summed squares; false if they are flat (a spread under 2 greys).
bool normalise(float* values, int count) {
    float mean = 0.0f;
    for (int i = 0; i < count; ++i) {
        mean += values[i];
    }
    mean /= static_cast<float>(count);
    float squares = 0.0f;
    for (int i = 0; i < count; ++i) {
        values[i] -= mean;
        squares += values[i] * values[i];
    }
    if (squares < 4.0f * static_cast<float>(count)) {
        return false;
    }
    const float scale = 1.0f / std::sqrt(squares);
    for (int i = 0; i < count; ++i) {
        values[i] *= scale;
    }
    return true;
}

// The correlation of normalised `learned` with `seen` (normalised here), -1 to 1; -1 if `seen` is flat.
float correlate(const float* learned, float* seen, int count) {
    if (!normalise(seen, count)) {
        return -1.0f;
    }
    float sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        sum += learned[i] * seen[i];
    }
    return sum;
}

// core/Ground's groundToScreen and screenToGround for `camera`'s lens at `pitch` and `distance`, its sines worked out
// once (the search carries half a million points through them an image).
struct Projector {
    Projector(const GroundCamera& camera, float pitch, float distance)
        : focal(camera.focal), middleX(camera.middleX), middleY(camera.middleY), sine(std::sin(pitch)),
          cosine(std::cos(pitch)), reach(distance + kGroundTarget * std::sin(pitch)) {}

    // Where ground point (`right`, `away`) is on the screen; false behind the camera.
    bool screen(float right, float away, float& x, float& y) const {
        const float ahead = away * cosine + reach;
        if (ahead <= 0.05f) {
            return false;
        }
        x = middleX + focal * right / ahead;
        y = middleY + focal * (kGroundTarget * cosine - away * sine) / ahead;
        return true;
    }
    // The ground point at screen pixel (`x`, `y`); false at or above the horizon.
    bool ground(float x, float y, float& right, float& away) const {
        const float v = (y - middleY) / focal;
        const float falls = v * cosine + sine;
        if (falls <= 1e-3f) {
            return false;
        }
        away = (kGroundTarget * cosine - v * reach) / falls;
        const float ahead = away * cosine + reach;
        if (ahead <= 0.05f) {
            return false;
        }
        right = (x - middleX) / focal * ahead;
        return true;
    }

    float focal;
    float middleX;
    float middleY;
    float sine;
    float cosine;
    float reach; // The distance ahead of the camera of the feet's ground, less away * cosine.
};

// A patch's `index`-th sample along a side, on the screen: kSpacing apart round its `middle`.
float sampleAt(float middle, int index) {
    constexpr float kFirst = -GroundMotion::kSpacing * static_cast<float>(GroundMotion::kSamples - 1) * 0.5f;
    return middle + kFirst + GroundMotion::kSpacing * static_cast<float>(index);
}

// Where something far away (as if infinitely) seen at (`x`, `y`) by `camera`'s lens at `pitch` is once the camera
// turned `turn` (as the ground turns: GroundMotion's score); false if then behind it.
bool farTurned(const GroundCamera& camera, float pitch, float turn, float x, float y, float& outX, float& outY) {
    const float u = (x - camera.middleX) / camera.focal;
    const float v = (y - camera.middleY) / camera.focal;
    const float sine = std::sin(pitch);
    const float cosine = std::cos(pitch);
    const float up = -sine - v * cosine; // Its way from the eye: right u, up, away.
    const float away = cosine - v * sine;
    const float c = std::cos(turn);
    const float s = std::sin(turn);
    const float right = c * u - s * away;
    const float turnedAway = s * u + c * away;
    const float ahead = -up * sine + turnedAway * cosine;
    if (ahead <= 0.05f) {
        return false;
    }
    outX = camera.middleX + camera.focal * right / ahead;
    outY = camera.middleY + camera.focal * (-up * cosine - turnedAway * sine) / ahead;
    return true;
}

} // namespace

void GroundMotion::place(const GroundCamera& camera, int width, int height, std::span<const GreyArea> avoid) {
    if (width != width_ || height != height_) {
        clear(); // Another screen: nothing learned holds.
        width_ = width;
        height_ = height;
    }
    const float half = kSpacing * static_cast<float>(kSamples) * 0.5f;
    const float lowest = camera.known ? horizonRow(camera) + kBelowHorizon * static_cast<float>(height) : 0.0f;
    for (int row = 0; row < kRows; ++row) {
        const float middleY = static_cast<float>(height) *
                              (kTop + (kBottom - kTop) * (static_cast<float>(row) + 0.5f) / static_cast<float>(kRows));
        for (int column = 0; column < kColumns; ++column) {
            Patch& patch = patches_[static_cast<std::size_t>(row * kColumns + column)];
            patch.x = static_cast<float>(width) * (static_cast<float>(column) + 0.5f) / static_cast<float>(kColumns);
            patch.y = middleY;
            const auto x0 = static_cast<int>(patch.x - half);
            const auto y0 = static_cast<int>(patch.y - half);
            const auto x1 = static_cast<int>(patch.x + half);
            const auto y1 = static_cast<int>(patch.y + half);
            patch.distant = camera.known && static_cast<float>(y0) < lowest;
            patch.placed = y0 >= 0 && y1 <= height &&
                           std::none_of(avoid.begin(), avoid.end(), [&](const GreyArea& area) {
                               return x0 < area.x1 && x1 > area.x0 && y0 < area.y1 && y1 > area.y0;
                           });
        }
    }
}

void GroundMotion::learn(const GreyView& image, const GroundCamera& camera, double timeMs) {
    learnedCount_ = 0;
    farCount_ = 0;
    learnedMs_ = timeMs;
    learnedCamera_ = camera;
    const GreyLevel& fine = image.levels[kFineLevel];
    const GreyLevel& coarse = image.levels[kCoarseLevel];
    for (Patch& patch : patches_) {
        patch.learned = false;
        patch.learnedFar = patch.distant;
        if (!patch.placed || !camera.known) {
            continue;
        }
        bool whole = true;
        int coarseCount = 0;
        for (int j = 0; j < kSamples && whole; ++j) {
            for (int i = 0; i < kSamples && whole; ++i) {
                const float x = sampleAt(patch.x, i);
                const float y = sampleAt(patch.y, j);
                whole = greyAt(fine, onLevel(x, kFineLevel), onLevel(y, kFineLevel),
                               patch.fine[static_cast<std::size_t>(j * kSamples + i)]);
                if (whole && i % kCoarseEvery == 0 && j % kCoarseEvery == 0) {
                    whole = greyAt(coarse, onLevel(x, kCoarseLevel), onLevel(y, kCoarseLevel),
                                   patch.coarse[static_cast<std::size_t>(coarseCount++)]);
                }
            }
        }
        patch.learned = whole && normalise(patch.fine.data(), kPerPatch) && normalise(patch.coarse.data(), kPerCoarse);
        (patch.learnedFar ? farCount_ : learnedCount_) += patch.learned ? 1 : 0;
    }
}

float GroundMotion::score(const GreyView& image, const GroundCamera& camera, const Motion& motion) const {
    const Projector from(learnedCamera_, motion.firstPitch, learnedCamera_.distance);
    const Projector to(camera, motion.secondPitch, learnedCamera_.distance * std::exp(motion.zoom));
    const GreyLevel& coarse = image.levels[kCoarseLevel];
    const float c = std::cos(motion.turn);
    const float s = std::sin(motion.turn);
    std::array<float, kPerCoarse> seen{};
    float total = 0.0f;
    int counted = 0;
    for (const Patch& patch : patches_) {
        if (!patch.learned || patch.learnedFar) {
            continue;
        }
        bool whole = true;
        int n = 0;
        for (int j = 0; j < kSamples && whole; j += kCoarseEvery) {
            for (int i = 0; i < kSamples && whole; i += kCoarseEvery) {
                float right = 0.0f;
                float away = 0.0f;
                float x = 0.0f;
                float y = 0.0f;
                whole = from.ground(sampleAt(patch.x, i), sampleAt(patch.y, j), right, away);
                const float r = right - motion.slideRight;
                const float a = away - motion.slideAway;
                whole = whole && to.screen(c * r - s * a, s * r + c * a, x, y) &&
                        greyAt(coarse, onLevel(x, kCoarseLevel), onLevel(y, kCoarseLevel),
                               seen[static_cast<std::size_t>(n++)]);
            }
        }
        if (whole) {
            total += correlate(patch.coarse.data(), seen.data(), kPerCoarse);
            ++counted;
        }
    }
    return counted >= std::max(5, learnedCount_ / 3) ? total / static_cast<float>(counted) : -2.0f;
}

std::optional<float> GroundMotion::findFarPitch(const GreyView& image, const GroundCamera& camera, float turn) const {
    if (farCount_ < kFarLeast) {
        return std::nullopt;
    }
    const GreyLevel& coarse = image.levels[kCoarseLevel];
    const GreyLevel& fine = image.levels[kFineLevel];
    const float pitch = learnedCamera_.pitch;
    std::array<float, kPerCoarse> seenCoarse{};
    std::array<float, kPerPatch> seenFine{};
    std::array<float, kPatches> tilts{};
    int found = 0;
    for (const Patch& patch : patches_) {
        if (!patch.learned || !patch.learnedFar) {
            continue;
        }
        float turnedX = 0.0f;
        float turnedY = 0.0f;
        if (!farTurned(camera, pitch, turn, patch.x, patch.y, turnedX, turnedY)) {
            continue;
        }
        const float byX = turnedX - patch.x; // All its samples moved so (full size), then searched round there.
        const float byY = turnedY - patch.y;
        const auto matchAt = [&](int level, float offsetX, float offsetY) {
            const bool fineLevel = level == kFineLevel;
            const int every = fineLevel ? 1 : kCoarseEvery;
            float* seen = fineLevel ? seenFine.data() : seenCoarse.data();
            int n = 0;
            for (int j = 0; j < kSamples; j += every) {
                for (int i = 0; i < kSamples; i += every) {
                    if (!greyAt(fineLevel ? fine : coarse, onLevel(sampleAt(patch.x, i) + byX, level) + offsetX,
                                onLevel(sampleAt(patch.y, j) + byY, level) + offsetY, seen[n++])) {
                        return -2.0f;
                    }
                }
            }
            return correlate(fineLevel ? patch.fine.data() : patch.coarse.data(), seen, n);
        };
        float best = -2.0f;
        int bestX = 0;
        int bestY = 0;
        for (int dy = -kFarReach; dy <= kFarReach; ++dy) {
            for (int dx = -kFarSideReach; dx <= kFarSideReach; ++dx) {
                if (const float match = matchAt(kCoarseLevel, static_cast<float>(dx), static_cast<float>(dy));
                    match > best) {
                    best = match;
                    bestX = dx;
                    bestY = dy;
                }
            }
        }
        // To a pixel on the quarter size (2 of them an eighth-size pixel).
        float fineBest = -2.0f;
        int fineY = 0;
        for (int dy = 2 * bestY - 2; dy <= 2 * bestY + 2; ++dy) {
            for (int dx = 2 * bestX - 2; dx <= 2 * bestX + 2; ++dx) {
                if (const float match = matchAt(kFineLevel, static_cast<float>(dx), static_cast<float>(dy));
                    match > fineBest) {
                    fineBest = match;
                    fineY = dy;
                }
            }
        }
        if (fineBest < kFarMatch) {
            continue;
        }
        // Risen (or fallen) so on the screen: the camera tilted as much (its rays turned about its sideways axis).
        const float row = turnedY + static_cast<float>(fineY << kFineLevel);
        tilts[static_cast<std::size_t>(found++)] = std::atan((turnedY - camera.middleY) / camera.focal) -
                                                   std::atan((row - camera.middleY) / camera.focal);
    }
    if (found < kFarLeast) {
        return std::nullopt;
    }
    std::nth_element(tilts.begin(), tilts.begin() + found / 2, tilts.begin() + found);
    // The camera circles a point its distance ahead, so as it tilts it also moves: something kFarRange away seems to
    // turn only (1 - distance / kFarRange) of the tilt.
    const float seen = 1.0f - std::min(learnedCamera_.distance / kFarRange, 0.5f);
    return pitch + tilts[static_cast<std::size_t>(found / 2)] / seen;
}

int GroundMotion::find(const GreyView& image, const GroundCamera& camera, double timeMs, std::optional<float> turn,
                       std::vector<GroundMatch>& out) {
    out.clear();
    searched_ = {};
    searchMatch_ = 0.0f;
    farPitch_.reset();
    if (learnedCount_ == 0 || !camera.known) {
        return 0;
    }

    // Foretold: going on as the camera was going (its turn, unless the minimap says it, and tilt, and its feet's slide
    // turned from the map to the view).
    const auto gapMs = static_cast<float>(timeMs - learnedMs_);
    const float mapX = camera.feetSpeedX * gapMs;
    const float mapY = camera.feetSpeedY * gapMs;
    const float yawC = std::cos(camera.yaw);
    const float yawS = std::sin(camera.yaw);
    Motion best{turn ? *turn : camera.yawSpeed * gapMs,
                yawC * mapX - yawS * mapY,
                yawS * mapX + yawC * mapY,
                learnedCamera_.pitch,
                std::clamp(learnedCamera_.pitch + camera.pitchSpeed * gapMs, kLowestTilt, kHighestPitch),
                std::log(camera.distance / learnedCamera_.distance)};
    // Tilted as the far patches say, if they do (with the turn known: else they are not looked for where it went).
    float foretoldPitch = best.firstPitch; // What a tilt costs from.
    if (const std::optional<float> far = turn ? findFarPitch(image, camera, *turn) : std::nullopt) {
        best.secondPitch = std::clamp(*far, kLowestTilt, kHighestPitch);
        foretoldPitch = best.secondPitch;
        farPitch_ = best.secondPitch;
    }
    const float foretoldRight = best.slideRight;
    const float foretoldAway = best.slideAway;
    const float foretoldZoom = best.zoom;
    const auto cost = [&](const Motion& motion) {
        return kTiltCost * std::fabs(motion.secondPitch - foretoldPitch) +
               kZoomCost * std::fabs(motion.zoom - foretoldZoom) +
               kSlideCost * std::hypot(motion.slideRight - foretoldRight, motion.slideAway - foretoldAway);
    };
    float bestScore = score(image, camera, best);
    float bestValue = bestScore - cost(best);

    // The search, coarse to fine, one part of the motion at a time (the others as found so far).
    const auto consider = [&](const Motion& motion) {
        const float scored = score(image, camera, motion);
        if (const float value = scored - cost(motion); value > bestValue) {
            bestValue = value;
            bestScore = scored;
            best = motion;
        }
    };
    const auto steps = [](float from, float to, float step, auto&& each) {
        for (float value = from; value <= to + 1e-6f; value += step) {
            each(value);
        }
    };
    const auto tryTurns = [&](float reach, float step) {
        const Motion around = best;
        steps(around.turn - reach, around.turn + reach, step, [&](float turn) {
            Motion motion = around;
            motion.turn = turn;
            consider(motion);
        });
    };
    const auto trySlides = [&](float reach, float step) {
        const Motion around = best;
        steps(around.slideRight - reach, around.slideRight + reach, step, [&](float right) {
            steps(around.slideAway - reach, around.slideAway + reach, step, [&](float away) {
                Motion motion = around;
                motion.slideRight = right;
                motion.slideAway = away;
                consider(motion);
            });
        });
    };
    const auto tryPitches = [&](float from, float to, float step) { // The same at both images.
        const Motion around = best;
        steps(std::max(from, kLowestPitch), std::min(to, kHighestPitch), step, [&](float pitch) {
            Motion motion = around;
            motion.firstPitch = pitch;
            motion.secondPitch = pitch + (around.secondPitch - around.firstPitch);
            consider(motion);
        });
    };
    const auto tryTilts = [&](float reach, float step) {
        const Motion around = best;
        steps(std::max(around.secondPitch - reach, kLowestTilt), std::min(around.secondPitch + reach, kHighestPitch),
              step, [&](float pitch) {
                  Motion motion = around;
                  motion.secondPitch = pitch;
                  consider(motion);
              });
    };
    const auto tryZooms = [&](float reach, float step) {
        const Motion around = best;
        steps(around.zoom - reach, around.zoom + reach, step, [&](float zoom) {
            Motion motion = around;
            motion.zoom = zoom;
            consider(motion);
        });
    };
    // Or standing still (turned only as the minimap says): after images not followed the foretold slide can be far
    // off, beyond the search's reach (clip 2026-11-12_17-43-14, 8.2 s: a still camera not found).
    Motion still = best;
    still.turn = turn.value_or(0.0f);
    still.slideRight = 0.0f;
    still.slideAway = 0.0f;
    still.secondPitch = still.firstPitch;
    still.zoom = 0.0f;
    consider(still);
    // The turn, the tilt, then the turn again: a tilt taken for a turn is not found again from there unless the turn
    // is searched again after it (clip 2026-11-12_15-50-36, tilted from level to straight down: held at 6 degrees,
    // turned 58), and the tilt searched first takes a fast turn for a tilt.
    const float turnReach = turn ? kKnownTurnReach : kTurnReach;
    const float turnStep = turn ? 0.005f : 0.015f;
    tryTurns(turnReach, turnStep);
    tryTilts(kTiltReach, 0.015f);
    tryTurns(turnReach, turnStep);
    trySlides(kSlideReach, 0.1f);
    tryZooms(kZoomReach, 0.03f);
    if (!camera.pitchMeasured && std::fabs(best.turn) >= kPitchTurn) {
        tryPitches(kLowestPitch, kHighestPitch, 0.04f);
        tryTurns(0.1f, 0.01f);
        tryPitches(best.firstPitch - 0.04f, best.firstPitch + 0.04f, 0.01f);
    }
    tryTilts(0.04f, 0.005f);
    tryTurns(0.03f, 0.005f);
    trySlides(0.1f, 0.025f);
    tryZooms(0.03f, 0.0075f);
    tryTilts(0.01f, 0.0025f);
    searched_ = best;
    searchMatch_ = std::max(bestScore, 0.0f);

    // Then each patch, moved so, looked for a few pixels around on the quarter size.
    const Projector from(learnedCamera_, best.firstPitch, learnedCamera_.distance);
    const Projector to(camera, best.secondPitch, learnedCamera_.distance * std::exp(best.zoom));
    const GreyLevel& fine = image.levels[kFineLevel];
    const float c = std::cos(best.turn);
    const float s = std::sin(best.turn);
    const float scale = static_cast<float>(1 << kFineLevel);
    std::array<float, kPerPatch> placeX{};
    std::array<float, kPerPatch> placeY{};
    std::array<float, kPerPatch> seen{};
    std::array<float, (2 * kWideReach + 1) * (2 * kWideReach + 1)> scores{};
    const auto findAll = [&](int reach) {
        out.clear();
        const int offsets = 2 * reach + 1;
        for (const Patch& patch : patches_) {
            if (!patch.learned || patch.learnedFar) {
                continue;
            }
            bool whole = true;
            for (int j = 0; j < kSamples && whole; ++j) {
                for (int i = 0; i < kSamples && whole; ++i) {
                    float right = 0.0f;
                    float away = 0.0f;
                    float x = 0.0f;
                    float y = 0.0f;
                    whole = from.ground(sampleAt(patch.x, i), sampleAt(patch.y, j), right, away);
                    const float r = right - best.slideRight;
                    const float a = away - best.slideAway;
                    whole = whole && to.screen(c * r - s * a, s * r + c * a, x, y);
                    const auto at = static_cast<std::size_t>(j * kSamples + i);
                    placeX[at] = onLevel(x, kFineLevel);
                    placeY[at] = onLevel(y, kFineLevel);
                }
            }
            if (!whole) {
                continue;
            }
            int bestAt = -1;
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    bool inside = true;
                    for (std::size_t at = 0; at < static_cast<std::size_t>(kPerPatch) && inside; ++at) {
                        inside = greyAt(fine, placeX[at] + static_cast<float>(dx), placeY[at] + static_cast<float>(dy),
                                        seen[at]);
                    }
                    const auto index = static_cast<std::size_t>((dy + reach) * offsets + (dx + reach));
                    scores[index] = inside ? correlate(patch.fine.data(), seen.data(), kPerPatch) : -2.0f;
                    if (bestAt < 0 || scores[index] > scores[static_cast<std::size_t>(bestAt)]) {
                        bestAt = static_cast<int>(index);
                    }
                }
            }
            const int bestX = bestAt % offsets;
            const int bestY = bestAt / offsets;
            const float match = scores[static_cast<std::size_t>(bestAt)];
            if (match < kMinMatch || bestX == 0 || bestY == 0 || bestX == offsets - 1 || bestY == offsets - 1) {
                continue; // No match, or the best is at the edge: it may be farther.
            }
            // A fraction of a pixel from the scores either side (a parabola through the three).
            const auto between = [&](int before, int after) {
                const float left = scores[static_cast<std::size_t>(before)];
                const float right = scores[static_cast<std::size_t>(after)];
                const float curve = left - 2.0f * match + right;
                return curve < -1e-6f ? std::clamp(0.5f * (left - right) / curve, -0.5f, 0.5f) : 0.0f;
            };
            const float fractionX = between(bestAt - 1, bestAt + 1);
            const float fractionY = between(bestAt - offsets, bestAt + offsets);
            // Its middle (between its four middle samples) moved so, and on by the offset found.
            const auto middle = static_cast<std::size_t>((kSamples / 2) * kSamples + kSamples / 2);
            const float middleX = 0.25f * (placeX[middle] + placeX[middle - 1] + placeX[middle - kSamples] +
                                           placeX[middle - kSamples - 1]);
            const float middleY = 0.25f * (placeY[middle] + placeY[middle - 1] + placeY[middle - kSamples] +
                                           placeY[middle - kSamples - 1]);
            const float x1 = (middleX + static_cast<float>(bestX - reach) + fractionX + 0.5f) * scale;
            const float y1 = (middleY + static_cast<float>(bestY - reach) + fractionY + 0.5f) * scale;
            out.push_back({patch.x, patch.y, x1, y1});
        }
    };
    findAll(kPatchReach);
    if (static_cast<int>(out.size()) < GroundFit::kMinMatches) {
        findAll(kWideReach);
    }
    return static_cast<int>(out.size());
}

namespace {

std::span<const GreyArea> spanOf(const GreyArea* areas, int count) {
    return {areas, areas != nullptr && count > 0 ? static_cast<std::size_t>(count) : 0};
}

} // namespace

void GroundFollower::begin(double timeMs, int width, int height, const GroundHints& hints) {
    timeMs_ = timeMs;
    if (hints.hasOwn) {
        fit_.own(hints.ownTop, hints.ownBottom, static_cast<float>(width), static_cast<float>(height), timeMs);
    } else {
        fit_.unseen(timeMs);
    }
    motion_.place(fit_.camera(), width, height, spanOf(hints.avoid, hints.avoidCount));
    coneAreaCount_ = static_cast<std::size_t>(cone_.areas(timeMs, spanOf(hints.windows, hints.windowCount),
                                                          coneAreas_.data(), static_cast<int>(coneAreas_.size())));
}

int GroundFollower::fullAreas(GreyArea* out, int max) const {
    const int count = std::min(static_cast<int>(coneAreaCount_), std::max(max, 0));
    std::copy_n(coneAreas_.begin(), count, out);
    return count;
}

GroundNote GroundFollower::note() const {
    GroundNote note;
    note.followed = followed_;
    note.fitted = fitted_;
    note.motion = fit_.lastMotion();
    note.learned = learnedBefore_;
    note.found = static_cast<int>(matches_.size());
    std::vector<float> moved;
    moved.reserve(matches_.size());
    for (const GroundMatch& match : matches_) {
        moved.push_back(std::hypot(match.x1 - match.x0, match.y1 - match.y0));
    }
    if (!moved.empty()) {
        std::nth_element(moved.begin(), moved.begin() + static_cast<std::ptrdiff_t>(moved.size() / 2), moved.end());
        note.moved = moved[moved.size() / 2];
    }
    note.agreeing = fit_.lastAgreeing();
    note.miss = fit_.lastMiss();
    note.searchTurn = motion_.searchTurn();
    note.searchMatch = motion_.searchMatch();
    note.headed = cone_.seen();
    note.heading = cone_.heading();
    return note;
}

bool GroundFollower::end(const GreyView& image) {
    bool fitted = false;
    learnedBefore_ = motion_.learnedCount();
    followed_ = motion_.learned();
    // How far the camera turned since the image the patches were learned on, if the minimap showed it on both.
    const bool headed = cone_.read(image.levels[0]); // Reads only the parts fullAreas() gave.
    std::optional<float> turn;
    if (headed && learnedHeaded_) {
        constexpr float kPi = 3.14159265f;
        turn = std::remainder(cone_.heading() - learnedHeading_, 2.0f * kPi);
    }
    if (followed_) {
        motion_.find(image, fit_.camera(), timeMs_, turn, matches_);
        const GroundFit::Start found{motion_.searchFirstPitch(),    motion_.searchPitch(),
                                     motion_.searchFirstDistance(), motion_.searchDistance(),
                                     turn.has_value(),              turn.value_or(0.0f),
                                     motion_.farPitch().has_value()};
        fitted = fit_.motion(matches_, motion_.learnedMs(), timeMs_, found);
        const GroundFit::Motion how = fit_.lastMotion();
        if ((how == GroundFit::Motion::FewMatches || how == GroundFit::Motion::FewAgreeing) &&
            motion_.searchMatch() >= GroundMotion::kMinSearch) {
            GroundFit::Start searched = found;
            if (motion_.searchMatch() < GroundMotion::kSureTilt) { // Blurred: tilted only as far things say.
                searched.secondPitch = motion_.farPitch().value_or(searched.firstPitch);
            }
            fit_.searched(motion_.searchTurn(), motion_.searchSlideRight(), motion_.searchSlideAway(), searched,
                          motion_.learnedMs(), timeMs_);
        }
        if (fitted || (fit_.lastMotion() == GroundFit::Motion::Searched &&
                       motion_.searchMatch() >= GroundMotion::kSureSearch)) {
            lostSinceMs_ = -1.0;
        } else if (lostSinceMs_ < 0.0) {
            lostSinceMs_ = timeMs_;
        } else if (timeMs_ - lostSinceMs_ > kLostMs) {
            fit_.reset(); // Known again from the next image's own character.
            lostSinceMs_ = -1.0;
        }
    } else {
        matches_.clear();
    }
    if (headed) {
        fit_.head(cone_.heading(), fitted || (followed_ && fit_.lastMotion() == GroundFit::Motion::Searched));
    }
    motion_.learn(image, fit_.camera(), timeMs_);
    learnedHeaded_ = headed;
    learnedHeading_ = cone_.heading();
    between_ = false;
    fitted_ = fitted;
    return fitted;
}

int GroundFollower::headAreas(double timeMs, const GroundHints& hints, GreyArea* out, int max) {
    headMs_ = timeMs;
    return cone_.areas(timeMs, spanOf(hints.windows, hints.windowCount), out, max);
}

bool GroundFollower::head(const GreyView& image) {
    constexpr float kPi = 3.14159265f;
    const GroundCamera& followed = fit_.camera();
    between_ = true;
    betweenCamera_ = groundAt(followed, headMs_);
    const bool seen = cone_.read(image.levels[0]); // Reads only the parts headAreas() gave (none: nothing).
    if (seen && learnedHeaded_ && followed.known) {
        // Turned as far as the cone, and held there: not on along the follow's speed, which goes on after a turn stops.
        betweenCamera_.yaw = followed.yaw + std::remainder(cone_.heading() - learnedHeading_, 2.0f * kPi);
        betweenCamera_.yawSpeed = 0.0f;
    }
    return seen;
}

void GroundFollower::clear() {
    motion_.clear();
    fit_.reset();
    cone_.clear();
    coneAreaCount_ = 0;
    learnedHeaded_ = false;
    between_ = false;
    matches_.clear();
    lostSinceMs_ = -1.0;
    followed_ = false;
    fitted_ = false;
    learnedBefore_ = 0;
}

} // namespace observex
