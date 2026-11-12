#include "core/Ground.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace observex {

namespace {

constexpr float kNear = 0.05f;           // Nearer the camera's eye than this (own heights) is behind it.
constexpr double kMinPitch = -0.6;       // The pitches a fit may end at (radians): from looking up a little
constexpr double kMaxPitch = 1.5650;     // to straight down (less a hair, for the tangent).
constexpr double kMinDistance = 0.6;     // Nearer than this the camera would be inside the character.
constexpr double kMaxDistance = 200.0;

// The camera of a motion fit, in doubles (the fit takes differences of tiny steps).
struct Pose {
    double pitch = 0.0;
    double distance = 0.0;
};

struct Screen {
    double focal = 0.0;
    double middleX = 0.0;
    double middleY = 0.0;
};

// Where the ground point (`right`, `away`) is on the screen of `screen` seen from `pose`; false behind the camera.
bool project(const Screen& screen, const Pose& pose, double right, double away, double& x, double& y) {
    const double s = std::sin(pose.pitch);
    const double c = std::cos(pose.pitch);
    const double down = -away * s + kGroundTarget * c;
    const double ahead = away * c + pose.distance + kGroundTarget * s;
    if (ahead <= kNear) {
        return false;
    }
    x = screen.middleX + screen.focal * right / ahead;
    y = screen.middleY + screen.focal * down / ahead;
    return true;
}

// The ground point under screen pixel (x, y) seen from `pose`; false at or above the horizon.
bool unproject(const Screen& screen, const Pose& pose, double x, double y, double& right, double& away) {
    const double s = std::sin(pose.pitch);
    const double c = std::cos(pose.pitch);
    const double a = (x - screen.middleX) / screen.focal;
    const double b = (y - screen.middleY) / screen.focal;
    const double falls = c * b + s; // How fast the ray from the eye through the pixel goes down.
    if (falls <= 1e-6) {
        return false;
    }
    const double along = (kGroundTarget + pose.distance * s) / falls; // The eye's height over how fast it falls.
    right = along * a;
    away = -pose.distance * c + along * (c - s * b);
    return true;
}

// The own character's box's rows seen from `pose`.
void ownRowsOf(const Screen& screen, const Pose& pose, double& top, double& bottom) {
    const double s = std::sin(pose.pitch);
    const double c = std::cos(pose.pitch);
    const double t = kGroundTarget;
    const double feetDown = kOwnFeetReach * s + t * c;
    const double feetAhead = -kOwnFeetReach * c + pose.distance + t * s;
    const double headDown = -kOwnHeadReach * s - (1.0 - t) * c;
    const double headAhead = kOwnHeadReach * c + pose.distance - (1.0 - t) * s;
    bottom = screen.middleY + screen.focal * feetDown / std::max(feetAhead, 1e-3);
    top = screen.middleY + screen.focal * headDown / std::max(headAhead, 1e-3);
}

// Solves the `n` x `n` system `a` x = `b` in place (Gaussian elimination with partial pivoting); false if singular.
template <std::size_t N> bool solve(std::array<std::array<double, N>, N>& a, std::array<double, N>& b) {
    for (std::size_t column = 0; column < N; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1; row < N; ++row) {
            if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) {
                pivot = row;
            }
        }
        if (std::fabs(a[pivot][column]) < 1e-12) {
            return false;
        }
        std::swap(a[column], a[pivot]);
        std::swap(b[column], b[pivot]);
        for (std::size_t row = column + 1; row < N; ++row) {
            const double factor = a[row][column] / a[column][column];
            for (std::size_t k = column; k < N; ++k) {
                a[row][k] -= factor * a[column][k];
            }
            b[row] -= factor * b[column];
        }
    }
    for (std::size_t column = N; column-- > 0;) {
        for (std::size_t k = column + 1; k < N; ++k) {
            b[column] -= a[column][k] * b[k];
        }
        b[column] /= a[column][column];
    }
    return true;
}

} // namespace

GroundCamera groundCamera(float pitch, float distance, float width, float height) {
    GroundCamera camera;
    camera.known = true;
    camera.pitch = pitch;
    camera.distance = distance;
    camera.focal = kGroundFocal * width / 2560.0f; // The game keeps its lens's width (an assumption: one screen).
    camera.middleX = width * 0.5f;
    camera.middleY = height * 0.5f;
    return camera;
}

float aheadOfCamera(const GroundCamera& camera, float right, float away, float up) {
    (void)right;
    return away * std::cos(camera.pitch) + camera.distance - (up - kGroundTarget) * std::sin(camera.pitch);
}

GroundPoint worldToScreen(const GroundCamera& camera, float right, float away, float up) {
    const float s = std::sin(camera.pitch);
    const float c = std::cos(camera.pitch);
    const float down = -away * s - (up - kGroundTarget) * c; // In the camera's view: right, down and ahead.
    const float ahead = away * c + camera.distance - (up - kGroundTarget) * s;
    if (ahead <= kNear) {
        return {};
    }
    return {camera.middleX + camera.focal * right / ahead, camera.middleY + camera.focal * down / ahead, true};
}

std::optional<GroundPoint> screenToGround(const GroundCamera& camera, float x, float y) {
    const Screen screen{camera.focal, camera.middleX, camera.middleY};
    double right = 0.0;
    double away = 0.0;
    if (!unproject(screen, {camera.pitch, camera.distance}, x, y, right, away)) {
        return std::nullopt;
    }
    return GroundPoint{static_cast<float>(right), static_cast<float>(away), true};
}

float horizonRow(const GroundCamera& camera) {
    return camera.middleY - camera.focal * std::tan(camera.pitch);
}

OwnRows ownRows(const GroundCamera& camera) {
    double top = 0.0;
    double bottom = 0.0;
    ownRowsOf({camera.focal, camera.middleX, camera.middleY}, {camera.pitch, camera.distance}, top, bottom);
    return {static_cast<float>(top), static_cast<float>(bottom)};
}

std::optional<float> distanceForOwnBottom(const GroundCamera& camera, float bottom) {
    const float s = std::sin(camera.pitch);
    const float c = std::cos(camera.pitch);
    const float down = kOwnFeetReach * s + kGroundTarget * c;
    const float below = (bottom - camera.middleY) / camera.focal;
    if (below <= 1e-4f || down <= 0.0f) {
        return std::nullopt;
    }
    const float distance = down / below + kOwnFeetReach * c - kGroundTarget * s;
    if (distance < kMinDistance || distance > kMaxDistance) {
        return std::nullopt;
    }
    return distance;
}

const Tracked* ownCharacter(std::span<const Tracked> things, int ownKind, float width) {
    const Tracked* own = nullptr;
    for (const Tracked& thing : things) {
        const float middle = thing.x + thing.width * 0.5f;
        if (ownKind >= 0 && static_cast<int>(thing.kind) == ownKind && thing.sure >= kGroundOwnSure &&
            std::fabs(middle - width * 0.5f) < width * 0.1f && (own == nullptr || thing.sure > own->sure)) {
            own = &thing;
        }
    }
    return own;
}

GroundCamera groundAt(const GroundCamera& camera, double timeMs) {
    GroundCamera moved = camera;
    const auto ahead = static_cast<float>(std::clamp(timeMs - camera.timeMs, -kGroundAheadMs, kGroundAheadMs));
    moved.timeMs = timeMs;
    moved.yaw += camera.yawSpeed * ahead;
    moved.pitch = std::clamp(camera.pitch + camera.pitchSpeed * ahead, static_cast<float>(kMinPitch),
                             static_cast<float>(kMaxPitch));
    moved.feetX += camera.feetSpeedX * ahead;
    moved.feetY += camera.feetSpeedY * ahead;
    return moved;
}

GroundCamera groundBetween(const GroundCamera& before, const GroundCamera& after, double timeMs) {
    if (!before.known || !after.known || after.timeMs <= before.timeMs) {
        return before.known && (!after.known || std::fabs(timeMs - before.timeMs) <= std::fabs(timeMs - after.timeMs))
                   ? before
                   : after;
    }
    if (timeMs <= before.timeMs) {
        return before;
    }
    if (timeMs >= after.timeMs) {
        return after;
    }
    const auto share = static_cast<float>((timeMs - before.timeMs) / (after.timeMs - before.timeMs));
    const auto mix = [share](float a, float b) { return a + share * (b - a); };
    GroundCamera camera = after;
    camera.timeMs = timeMs;
    camera.pitch = mix(before.pitch, after.pitch);
    camera.distance = mix(before.distance, after.distance);
    camera.yaw = mix(before.yaw, after.yaw);
    camera.feetX = mix(before.feetX, after.feetX);
    camera.feetY = mix(before.feetY, after.feetY);
    return camera;
}

GroundCamera groundAmong(std::span<const GroundCamera> cameras, double timeMs) {
    if (cameras.empty()) {
        return {};
    }
    const auto after = std::lower_bound(cameras.begin(), cameras.end(), timeMs,
                                        [](const GroundCamera& camera, double ms) { return camera.timeMs < ms; });
    if (after == cameras.begin()) {
        return *after;
    }
    if (after == cameras.end()) {
        return cameras.back();
    }
    return groundBetween(*(after - 1), *after, timeMs);
}

void mapToGround(const GroundCamera& camera, float mapX, float mapY, float& right, float& away) {
    const float c = std::cos(camera.yaw);
    const float s = std::sin(camera.yaw);
    const float x = mapX - camera.feetX;
    const float y = mapY - camera.feetY;
    right = c * x - s * y;
    away = s * x + c * y;
}

void GroundFit::own(float top, float bottom, float width, float height, double timeMs) {
    if (seenCount_ > 0) {
        const OwnRows& last = seen_[static_cast<std::size_t>((seenNext_ + seen_.size() - 1) % seen_.size())].rows;
        const float taller = (bottom - top) / std::max(last.bottom - last.top, 1.0f);
        if (taller > kOwnMostJump || taller * kOwnMostJump < 1.0f) {
            unseen(timeMs);
            return;
        }
    }
    if (!camera_.known) {
        const GroundCamera start = groundCamera(kGroundStartPitch, 8.0f, width, height);
        const std::optional<float> distance = distanceForOwnBottom(start, bottom);
        if (!distance) {
            return;
        }
        camera_ = groundCamera(kGroundStartPitch, *distance, width, height);
        camera_.timeMs = timeMs;
    } else if (const std::optional<float> distance = camera_.pitch > kOwnSteepPitch
                                                         ? std::nullopt
                                                         : distanceForOwnBottom(camera_, bottom)) {
        // Only from a box as tall as the character would be there: looking down steeply the box is a blob round the
        // screen's middle, its bottom no longer the feet (clip 2026-11-12_11-34-03 put the camera 22 away, then 3).
        GroundCamera there = camera_;
        there.distance = *distance;
        const OwnRows foretold = ownRows(there);
        const float tall = (bottom - top) / std::max(foretold.bottom - foretold.top, 1.0f);
        if (tall > kOwnLeastTall && tall < kOwnMostTall) {
            const double share = 1.0 - std::exp(-std::max(0.0, timeMs - ownMs_) / kDistanceSmoothMs);
            camera_.distance = static_cast<float>(
                std::exp(std::log(camera_.distance) + share * (std::log(*distance) - std::log(camera_.distance))));
        }
    }
    seen_[static_cast<std::size_t>(seenNext_)] = {{top, bottom}, timeMs};
    seenNext_ = (seenNext_ + 1) % static_cast<int>(seen_.size());
    seenCount_ = std::min(seenCount_ + 1, static_cast<int>(seen_.size()));
    ownMs_ = timeMs;
}

std::optional<OwnRows> GroundFit::ownAt(double timeMs) const {
    const OwnSeen* nearest = nullptr;
    for (int i = 0; i < seenCount_; ++i) {
        const OwnSeen& seen = seen_[static_cast<std::size_t>(i)];
        if (nearest == nullptr || std::fabs(seen.timeMs - timeMs) < std::fabs(nearest->timeMs - timeMs)) {
            nearest = &seen;
        }
    }
    if (nearest == nullptr || std::fabs(nearest->timeMs - timeMs) > kOwnNearMs) {
        return std::nullopt;
    }
    return nearest->rows;
}

void GroundFit::things(std::span<const Tracked> things, int ownKind, float width, float height, double timeMs) {
    if (const Tracked* own = ownCharacter(things, ownKind, width)) {
        this->own(own->y, own->y + own->height, width, height, timeMs);
    } else {
        unseen(timeMs);
    }
}

bool GroundFit::motion(std::span<const GroundMatch> matches, double firstMs, double secondMs, const Start& start) {
    const std::optional<OwnRows> ownFirst = ownAt(firstMs);
    const std::optional<OwnRows> ownSecond = ownAt(secondMs);
    lastAgreeing_ = 0;
    lastMiss_ = 0.0f;
    lastMotion_ = !camera_.known                                       ? Motion::NoCamera
                  : !ownFirst || !ownSecond                            ? Motion::NoOwn
                  : static_cast<int>(matches.size()) < kMinMatches ? Motion::FewMatches
                                                                       : Motion::Fitted;
    if (lastMotion_ != Motion::Fitted) {
        stop();
        return false;
    }
    // Too little motion says nothing of the pitch: it is held then, and only the turn and slide fitted.
    std::vector<float> moved;
    moved.reserve(matches.size());
    for (const GroundMatch& match : matches) {
        moved.push_back(std::hypot(match.x1 - match.x0, match.y1 - match.y0));
    }
    std::nth_element(moved.begin(), moved.begin() + static_cast<std::ptrdiff_t>(moved.size() / 2), moved.end());
    const bool holdPitch = moved[moved.size() / 2] < kMinMotion;

    // The camera at both images (pitch, log distance), and how the ground turned (yaw) and slid (right, away; own
    // heights) between them under the own character's feet, fitted by Levenberg-Marquardt to where the matches went
    // (robust: matches off the ground, on the interface or on things that move by themselves count less the more they
    // miss) and to the own character's box at both (looking down steeply, to the first distance instead:
    // kOwnSteepPitch). Starting from the camera it has (its pitch measured; else the pitch given), it stays with that
    // answer: while walking, a second, far steeper one also fits the motion alone.
    const Screen screen{camera_.focal, camera_.middleX, camera_.middleY};
    constexpr std::size_t kParameters = 7;
    using Vector = std::array<double, kParameters>;
    Vector at{start.firstPitch, start.secondPitch, std::log(start.firstDistance), std::log(start.secondDistance),
              0.0, 0.0, 0.0};
    const double priorPitch = start.firstPitch;
    const bool steep = std::max(start.firstPitch, start.secondPitch) > kOwnSteepPitch;
    const double priorDistance = std::log(start.firstDistance);
    constexpr double kDistanceSpread = 0.02; // How far the first distance may wander then (as a log).
    // How far the fit may wander from it (radians), and the pitch change between the two images.
    const double pitchSpread = holdPitch            ? 0.005
                               : start.tiltKnown    ? kKnownTiltSpread
                               : camera_.pitchMeasured ? 0.15
                                                       : 0.6;
    const double pitchStep = holdPitch ? 0.005 : start.tiltKnown ? kKnownTiltSpread : 0.1;
    const double priorTilt = start.tiltKnown ? start.secondPitch - start.firstPitch : 0.0;
    constexpr double kMatchScale = 2.0;  // Pixels: a match this far off counts half.
    constexpr double kOwnScale = 3.0;    // Pixels: how far the own box's bottom may be off as far as one match,
    constexpr double kOwnTopScale = 10.0; // and its top: it says less (the hair, a helmet, the head bobbing in a fight;
    // looking down 20 to 35 degrees it is not the head's top as the box's model puts it: held as tightly as the bottom,
    // it held a camera that tilted 24 degrees to 15 of them, clip 2026-11-12_17-43-14, 0 to 0.6 s).

    const std::size_t count = matches.size();
    std::vector<double> weights(count, 1.0);
    std::vector<double> residuals;
    const auto residualsAt = [&](const Vector& x, std::vector<double>& out) {
        out.assign(2 * count + 8, 0.0);
        const Pose first{x[0], std::exp(x[2])};
        const Pose second{x[1], std::exp(x[3])};
        const double c = std::cos(x[4]);
        const double s = std::sin(x[4]);
        for (std::size_t i = 0; i < count; ++i) {
            double right = 0.0;
            double away = 0.0;
            double u = 0.0;
            double v = 0.0;
            if (!unproject(screen, first, matches[i].x0, matches[i].y0, right, away)) {
                continue; // Above the horizon: not ground.
            }
            const double movedRight = c * (right - x[5]) - s * (away - x[6]);
            const double movedAway = s * (right - x[5]) + c * (away - x[6]);
            if (!project(screen, second, movedRight, movedAway, u, v)) {
                out[2 * i] = 50.0; // Behind the camera: as far off as any miss can count.
                continue;
            }
            out[2 * i] = u - matches[i].x1;
            out[2 * i + 1] = v - matches[i].y1;
        }
        if (steep) {
            out[2 * count + 6] = (x[2] - priorDistance) / kDistanceSpread * kMatchScale;
        } else {
            double top = 0.0;
            double bottom = 0.0;
            ownRowsOf(screen, first, top, bottom);
            out[2 * count] = (bottom - ownFirst->bottom) * kMatchScale / kOwnScale;
            out[2 * count + 1] = (top - ownFirst->top) * kMatchScale / kOwnTopScale;
            ownRowsOf(screen, second, top, bottom);
            out[2 * count + 2] = (bottom - ownSecond->bottom) * kMatchScale / kOwnScale;
            out[2 * count + 3] = (top - ownSecond->top) * kMatchScale / kOwnTopScale;
        }
        out[2 * count + 4] = (x[0] - priorPitch) / pitchSpread * kMatchScale;
        out[2 * count + 5] = (x[1] - x[0] - priorTilt) / pitchStep * kMatchScale;
        if (start.turnKnown) {
            out[2 * count + 7] = (x[4] - start.turn) / kKnownTurnSpread * kMatchScale;
        }
    };
    const auto weightOf = [&](std::size_t residual) {
        return residual < 2 * count ? weights[residual / 2] : 1.0;
    };
    const auto costOf = [&](const std::vector<double>& r) {
        double cost = 0.0;
        for (std::size_t i = 0; i < r.size(); ++i) {
            cost += weightOf(i) * r[i] * r[i];
        }
        return cost;
    };
    // Cauchy weights, from a wide scale down to kMatchScale (graduated: starting narrow, from a camera far off, the
    // weights would favour whichever matches happen to miss least there; a quarter of the matches standing still,
    // as the interface does, then held the pitch 0.11 radians off).
    constexpr std::array<double, 4> kScales{32.0, 12.0, 5.0, kMatchScale};
    double scale = kScales[0];
    const auto reweight = [&](const std::vector<double>& r) {
        for (std::size_t i = 0; i < count; ++i) {
            const double miss = std::hypot(r[2 * i], r[2 * i + 1]) / scale;
            weights[i] = 1.0 / (1.0 + miss * miss);
        }
    };

    residualsAt(at, residuals);
    double damping = 1e-3;
    std::vector<double> stepped;
    std::vector<std::array<double, kParameters>> jacobian(residuals.size());
    for (int iteration = 0; iteration < 24; ++iteration) {
        if (iteration % 6 == 0) {
            scale = kScales[static_cast<std::size_t>(iteration / 6)];
            reweight(residuals);
        }
        const double cost = costOf(residuals);
        constexpr Vector kSteps{1e-5, 1e-5, 1e-5, 1e-5, 1e-5, 1e-4, 1e-4};
        jacobian.assign(residuals.size(), {});
        for (std::size_t p = 0; p < kParameters; ++p) {
            Vector nudged = at;
            nudged[p] += kSteps[p];
            residualsAt(nudged, stepped);
            for (std::size_t i = 0; i < residuals.size(); ++i) {
                jacobian[i][p] = (stepped[i] - residuals[i]) / kSteps[p];
            }
        }
        std::array<std::array<double, kParameters>, kParameters> normal{};
        Vector gradient{};
        for (std::size_t i = 0; i < residuals.size(); ++i) {
            const double w = weightOf(i);
            for (std::size_t p = 0; p < kParameters; ++p) {
                gradient[p] -= w * jacobian[i][p] * residuals[i];
                for (std::size_t q = 0; q < kParameters; ++q) {
                    normal[p][q] += w * jacobian[i][p] * jacobian[i][q];
                }
            }
        }
        bool improved = false;
        for (int tries = 0; tries < 6 && !improved; ++tries) {
            auto system = normal;
            Vector step = gradient;
            for (std::size_t p = 0; p < kParameters; ++p) {
                system[p][p] *= 1.0 + damping;
                system[p][p] += 1e-9;
            }
            if (!solve(system, step)) {
                damping *= 10.0;
                continue;
            }
            Vector next = at;
            for (std::size_t p = 0; p < kParameters; ++p) {
                next[p] += step[p];
            }
            next[0] = std::clamp(next[0], kMinPitch, kMaxPitch);
            next[1] = std::clamp(next[1], kMinPitch, kMaxPitch);
            next[2] = std::clamp(next[2], std::log(kMinDistance), std::log(kMaxDistance));
            next[3] = std::clamp(next[3], std::log(kMinDistance), std::log(kMaxDistance));
            residualsAt(next, stepped);
            if (costOf(stepped) < cost) {
                at = next;
                residuals = stepped;
                damping = std::max(damping * 0.3, 1e-6);
                improved = true;
            } else {
                damping *= 10.0;
            }
        }
        if (improved) {
            reweight(residuals);
        } else {
            iteration = (iteration / 6) * 6 + 5; // Settled at this scale: on to the next.
        }
    }

    int agreeing = 0;
    double missed = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double miss = std::hypot(residuals[2 * i], residuals[2 * i + 1]);
        if (miss < 3.0) {
            ++agreeing;
            missed += miss;
        }
    }
    lastAgreeing_ = agreeing;
    lastMiss_ = agreeing > 0 ? static_cast<float>(missed / agreeing) : 0.0f;
    // No camera turns, tilts or goes faster than the user can make it (else the matches agreed by chance, e.g. on a
    // repeating texture).
    const double gapMs = std::max(1.0, secondMs - firstMs);
    const bool tooFar = std::fabs(at[4]) > kMostTurn * gapMs ||
                        std::fabs(at[1] - at[0]) > kMostTilt * gapMs ||
                        std::hypot(at[5], at[6]) > kMostGo * gapMs;
    if (agreeing < kMinMatches || tooFar) {
        lastMotion_ = tooFar ? Motion::TooFar : Motion::FewAgreeing;
        stop();
        return false;
    }
    camera_.pitch = static_cast<float>(at[1]);
    camera_.distance = static_cast<float>(std::exp(at[3]));
    camera_.pitchMeasured = true;
    move(static_cast<float>(at[4]), static_cast<float>(at[1] - at[0]), static_cast<float>(at[5]),
         static_cast<float>(at[6]), gapMs, secondMs);
    return true;
}

bool GroundFit::searched(float turn, float slideRight, float slideAway, const Start& found, double firstMs,
                         double secondMs) {
    const double gapMs = std::max(1.0, secondMs - firstMs);
    const float tilt = found.secondPitch - found.firstPitch;
    if (!camera_.known || std::fabs(turn) > kMostTurn * gapMs || std::fabs(tilt) > kMostTilt * gapMs ||
        std::hypot(slideRight, slideAway) > kMostGo * gapMs) {
        return false;
    }
    lastMotion_ = Motion::Searched;
    camera_.pitch = found.secondPitch;
    camera_.distance =
        std::clamp(found.secondDistance, static_cast<float>(kMinDistance), static_cast<float>(kMaxDistance));
    move(turn, tilt, slideRight, slideAway, gapMs, secondMs);
    return true;
}

void GroundFit::head(float heading, bool moved) {
    if (!camera_.known) {
        return;
    }
    constexpr float kPi = 3.14159265f;
    const float off = std::remainder(heading - camera_.yaw, 2.0f * kPi);
    camera_.yaw += headed_ && moved ? kHeadShare * off : off;
    headed_ = true;
}

void GroundFit::move(float turn, float tilt, float slideRight, float slideAway, double gapMs, double secondMs) {
    // The camera at the later image, turned and gone on the map by what the ground did.
    const float c = std::cos(camera_.yaw);
    const float s = std::sin(camera_.yaw);
    const float slideX = c * slideRight + s * slideAway; // The slide (right, away at the first image) on the map.
    const float slideY = -s * slideRight + c * slideAway;
    camera_.timeMs = secondMs;
    camera_.yaw += turn;
    camera_.feetX += slideX;
    camera_.feetY += slideY;
    camera_.yawSpeed = static_cast<float>(turn / gapMs);
    camera_.pitchSpeed = static_cast<float>(tilt / gapMs);
    camera_.feetSpeedX = static_cast<float>(slideX / gapMs);
    camera_.feetSpeedY = static_cast<float>(slideY / gapMs);
}

} // namespace observex
