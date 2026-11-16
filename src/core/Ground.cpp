#include "core/Ground.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
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

bool ownFeetHidden(float top, float bottom, float middleX, std::span<const GreyArea> windows) {
    for (const GreyArea& window : windows) {
        const auto x0 = static_cast<float>(window.x0);
        const auto x1 = static_cast<float>(window.x1);
        const auto y0 = static_cast<float>(window.y0);
        const auto y1 = static_cast<float>(window.y1);
        if (middleX >= x0 && middleX < x1 && top < y0 && bottom >= y0 - kFeetHiddenSlack && bottom <= y1) {
            return true;
        }
    }
    return false;
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

void groundToMap(const GroundCamera& camera, float right, float away, float& mapX, float& mapY) {
    const float c = std::cos(camera.yaw);
    const float s = std::sin(camera.yaw);
    mapX = camera.feetX + c * right + s * away;
    mapY = camera.feetY - s * right + c * away;
}

float heightOnMap(const GroundHeights& heights, float mapX, float mapY) {
    constexpr int kLast = GroundHeights::kCells - 1;
    const float x = std::clamp((mapX - heights.firstX) / GroundHeights::kCell, 0.0f, static_cast<float>(kLast));
    const float y = std::clamp((mapY - heights.firstY) / GroundHeights::kCell, 0.0f, static_cast<float>(kLast));
    const int x0 = std::min(static_cast<int>(x), kLast - 1);
    const int y0 = std::min(static_cast<int>(y), kLast - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto at = [&](int cx, int cy) {
        return heights.height[static_cast<std::size_t>(cy * GroundHeights::kCells + cx)];
    };
    const float top = at(x0, y0) + fx * (at(x0 + 1, y0) - at(x0, y0));
    const float bottom = at(x0, y0 + 1) + fx * (at(x0 + 1, y0 + 1) - at(x0, y0 + 1));
    return top + fy * (bottom - top);
}

float groundHeight(const GroundHeights& heights, const GroundCamera& camera, float mapX, float mapY) {
    if (!heights.known) {
        return 0.0f;
    }
    return heightOnMap(heights, mapX, mapY) - heightOnMap(heights, camera.feetX, camera.feetY);
}

namespace {

// A camera's sines worked out once, for many points of the ground's map.
struct MapView {
    explicit MapView(const GroundCamera& seen)
        : camera(seen), c(std::cos(seen.pitch)), s(std::sin(seen.pitch)), cosYaw(std::cos(seen.yaw)),
          sinYaw(std::sin(seen.yaw)) {}
    // Map point (mapX, mapY) `up` own heights over the camera's feet: where on its screen, and how far ahead of its
    // eye; false behind it (as worldToScreen and aheadOfCamera).
    bool project(float mapX, float mapY, float up, float& x, float& y, float& ahead) const {
        const float dx = mapX - camera.feetX;
        const float dy = mapY - camera.feetY;
        const float right = cosYaw * dx - sinYaw * dy;
        const float away = sinYaw * dx + cosYaw * dy;
        ahead = away * c + camera.distance - (up - kGroundTarget) * s;
        if (ahead <= kNear) {
            return false;
        }
        x = camera.middleX + camera.focal * right / ahead;
        y = camera.middleY + camera.focal * (-away * s - (up - kGroundTarget) * c) / ahead;
        return true;
    }
    GroundCamera camera;
    float c = 0.0f;
    float s = 0.0f;
    float cosYaw = 0.0f;
    float sinYaw = 0.0f;
};

// log2 of a positive normal `x`, to about 0.005 (frontCode's, for every cell of a map).
float quickLog2(float x) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &x, sizeof(bits));
    const auto exponent = static_cast<float>(static_cast<int>((bits >> 23) & 0xFFu) - 128);
    bits = (bits & 0x807FFFFFu) | 0x3F800000u; // Its mantissa, in [1, 2).
    float mantissa = 0.0f;
    std::memcpy(&mantissa, &bits, sizeof(mantissa));
    return exponent + (-0.34484843f * mantissa + 2.02466578f) * mantissa - 0.67487759f;
}

const float kFrontNearestLog = std::log2(kFrontNearest);
const float kFrontLogStep = std::log2(kFrontFarthest / kFrontNearest) / 254.0f;
const std::array<float, 256> kFrontAheads = [] {
    std::array<float, 256> aheads{};
    aheads[0] = 1e30f; // Nothing in front.
    for (int code = 1; code < 256; ++code) {
        aheads[static_cast<std::size_t>(code)] =
            kFrontNearest * std::exp2(static_cast<float>(code - 1) * std::log2(kFrontFarthest / kFrontNearest) / 254.0f);
    }
    return aheads;
}();

bool hasFront(const GroundHeights* heights) {
    return heights != nullptr && heights->known && heights->frontCamera.known && heights->frontColumns > 0 &&
           heights->frontRows > 0;
}

// Whether `heights`' newest map saw something in front of a point at (x, y) of its camera's screen, `ahead` of its eye:
// it is farther than a point there can be and be seen.
bool frontHides(const GroundHeights& heights, float x, float y, float ahead) {
    const float column = x * heights.frontPerPixelX;
    const float row = y * heights.frontPerPixelY;
    if (!(column >= 0.0f && row >= 0.0f && column < static_cast<float>(heights.frontColumns) &&
          row < static_cast<float>(heights.frontRows))) {
        return false;
    }
    const std::uint8_t code =
        heights.front[static_cast<std::size_t>(static_cast<int>(row) * heights.frontColumns + static_cast<int>(column))];
    return code != 0 && ahead > kFrontAheads[code];
}

} // namespace

std::uint8_t frontCode(float ahead) {
    if (!(ahead < kFrontFarthest)) {
        return 0;
    }
    if (ahead <= kFrontNearest) {
        return 1;
    }
    const int code = 1 + static_cast<int>((quickLog2(ahead) - kFrontNearestLog) / kFrontLogStep + 0.5f);
    return static_cast<std::uint8_t>(std::clamp(code, 1, 255));
}

float frontAhead(std::uint8_t code) {
    return kFrontAheads[code];
}

bool groundHidden(const GroundHeights& heights, float mapX, float mapY, float level) {
    if (!hasFront(&heights)) {
        return false;
    }
    const MapView view(heights.frontCamera);
    float x = 0.0f;
    float y = 0.0f;
    float ahead = 0.0f;
    return view.project(mapX, mapY, level - heights.frontFeetLevel, x, y, ahead) && frontHides(heights, x, y, ahead);
}

void groundGrid(const GroundCamera& camera, const GroundHeights* heights, std::span<GridPoint> out) {
    constexpr float kReach = kGridCell * static_cast<float>(kGridCells);
    constexpr float kNearest = 0.3f; // Own heights ahead of the camera's eye: nearer is not drawn.
    constexpr float kStep = kGridCell / static_cast<float>(kGridSteps);
    if (out.size() < kGridPoints) {
        return;
    }
    const float strength = camera.pitchMeasured ? 1.0f : 0.5f;
    const bool shaped = heights != nullptr && heights->known;
    const bool hiding = hasFront(heights);
    const float feetLevel = shaped ? heightOnMap(*heights, camera.feetX, camera.feetY) : 0.0f;
    const float firstX = (std::floor(camera.feetX / kGridCell) - static_cast<float>(kGridCells)) * kGridCell;
    const float firstY = (std::floor(camera.feetY / kGridCell) - static_cast<float>(kGridCells)) * kGridCell;
    const MapView view(camera);
    const MapView frontView(hiding ? heights->frontCamera : camera);
    for (int j = 0; j < kGridSide; ++j) {
        for (int i = 0; i < kGridSide; ++i) {
            GridPoint& point = out[static_cast<std::size_t>(j * kGridSide + i)];
            const float mapX = firstX + static_cast<float>(i) * kStep;
            const float mapY = firstY + static_cast<float>(j) * kStep;
            const float closeness = 1.0f - std::hypot(mapX - camera.feetX, mapY - camera.feetY) / kReach;
            point.shown = closeness > 0.0f ? closeness * closeness * strength : 0.0f;
            const float level = shaped ? heightOnMap(*heights, mapX, mapY) : 0.0f;
            point.ok = view.project(mapX, mapY, level - feetLevel, point.x, point.y, point.ahead) &&
                       point.ahead >= kNearest;
            point.front = false;
            point.hidden = false;
            if (hiding && point.ok) {
                point.front = frontView.project(mapX, mapY, level - heights->frontFeetLevel, point.frontX,
                                                point.frontY, point.frontAhead);
                point.hidden = point.front && frontHides(*heights, point.frontX, point.frontY, point.frontAhead);
            }
        }
    }
}

int gridStepCount(const GridPoint& a, const GridPoint& b) {
    if (!a.front || !b.front) {
        return 1;
    }
    const float steps = std::ceil(std::hypot(b.x - a.x, b.y - a.y) / kGridTestPixels);
    return steps >= 1.0f ? static_cast<int>(std::min(steps, static_cast<float>(kGridMostSteps))) : 1;
}

void gridSteps(const GroundHeights& heights, const GridPoint& a, const GridPoint& b, int steps, std::span<GridStep> out) {
    if (out.size() < 2) {
        return;
    }
    steps = std::clamp(steps, 1, static_cast<int>(out.size()) - 1);
    const bool both = a.front && b.front;
    for (int k = 0; k <= steps; ++k) {
        // Even along the ground is not even on the screen: a point's screen place times its distance ahead goes evenly.
        const float t = static_cast<float>(k) / static_cast<float>(steps);
        const float fromA = a.ahead * (1.0f - t);
        const float fromB = b.ahead * t;
        GridStep& step = out[static_cast<std::size_t>(k)];
        step.x = (a.x * fromA + b.x * fromB) / (fromA + fromB);
        step.y = (a.y * fromA + b.y * fromB) / (fromA + fromB);
        if (both) {
            const float frontA = a.frontAhead * (1.0f - t);
            const float frontB = b.frontAhead * t;
            const float ahead = frontA + frontB;
            step.hidden = frontHides(heights, (a.frontX * frontA + b.frontX * frontB) / ahead,
                                     (a.frontY * frontA + b.frontY * frontB) / ahead, ahead);
        } else {
            step.hidden = k == 0 ? a.hidden : (k == steps && b.hidden);
        }
    }
}

bool ZoomHold::wheel(bool watched, double notches, double timeMs, float current) {
    if (!watched) {
        clear();
        return false;
    }
    holds_ = true;
    const double by = counted_ ? notches - notches_ : 0.0;
    counted_ = true;
    notches_ = notches;
    if (by == 0.0) {
        return false;
    }
    if (count_ == 0) {
        if (!(current > 0.0f)) {
            return false;
        }
        push(std::log(current));
    }
    // The last kAfterStep kept round where the notches took the middle, as spread as they were round theirs.
    const int keep = std::min(count_, kAfterStep);
    std::array<float, kAfterStep> last{};
    for (int i = 0; i < keep; ++i) {
        last[static_cast<std::size_t>(i)] = logs_[static_cast<std::size_t>((next_ - keep + i + kKept) % kKept)];
    }
    std::nth_element(last.begin(), last.begin() + keep / 2, last.begin() + keep);
    keepLast(std::log(stepped(std::exp(middle_), by)) - last[static_cast<std::size_t>(keep / 2)]);
    settleMs_ = timeMs + kSettleMs;
    return true;
}

void ZoomHold::take(float distance, double timeMs) {
    if (!holds_ || !(distance > 0.0f) || timeMs < settleMs_) {
        return;
    }
    push(std::log(std::clamp(distance, kNearest, kFarthest)));
}

void ZoomHold::keepLast(float shift) {
    const int keep = std::min(count_, kAfterStep);
    std::array<float, kAfterStep> last{};
    for (int i = 0; i < keep; ++i) {
        last[static_cast<std::size_t>(i)] = logs_[static_cast<std::size_t>((next_ - keep + i + kKept) % kKept)] + shift;
    }
    std::copy_n(last.begin(), keep, logs_.begin());
    count_ = keep;
    next_ = keep % kKept;
    middle();
}

void ZoomHold::push(float log) {
    logs_[static_cast<std::size_t>(next_)] = log;
    next_ = (next_ + 1) % kKept;
    count_ = std::min(count_ + 1, kKept);
    middle();
}

void ZoomHold::middle() {
    std::array<float, kKept> sorted = logs_;
    const auto end = sorted.begin() + count_;
    const auto middle = sorted.begin() + count_ / 2;
    std::nth_element(sorted.begin(), middle, end);
    middle_ = *middle;
}

std::optional<float> ZoomHold::distance() const {
    if (count_ == 0) {
        return std::nullopt;
    }
    return std::exp(middle_);
}

float ZoomHold::stepped(float distance, double notches) {
    const float nearest = std::min(kNearest, distance);
    const float farthest = std::max(kFarthest, distance);
    float at = distance;
    for (double left = notches; std::fabs(left) > 1e-9;) {
        const double part = std::clamp(left, -1.0, 1.0);
        const bool fine = part < 0.0 ? at <= kFineBelow : at < kFineBelow;
        at = std::clamp(at + static_cast<float>(part) * (fine ? kFineStep : kStep), nearest, farthest);
        left -= part;
    }
    return at;
}

void GroundFit::wheel(bool watched, double notches, double timeMs) {
    if (zoom_.wheel(watched, notches, timeMs, camera_.known ? camera_.distance : 0.0f) && camera_.known) {
        camera_.distance = *zoom_.distance();
    }
}

std::optional<float> GroundFit::heldDistance() const {
    return zoom_.holds() ? zoom_.distance() : std::nullopt;
}

void GroundFit::own(float top, float bottom, float width, float height, double timeMs, bool feetSeen) {
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
        const std::optional<float> distance =
            feetSeen ? distanceForOwnBottom(start, bottom) : std::optional<float>{};
        const std::optional<float> held = heldDistance();
        if (!distance && !held) {
            return;
        }
        camera_ = groundCamera(kGroundStartPitch, held ? *held : *distance, width, height);
        camera_.timeMs = timeMs;
    } else if (const std::optional<float> distance = camera_.pitch > kOwnSteepPitch || !feetSeen
                                                         ? std::nullopt
                                                         : distanceForOwnBottom(camera_, bottom)) {
        // Only from a box as tall as the character would be there: looking down steeply the box is a blob round the
        // screen's middle, its bottom no longer the feet (clip 2026-11-12_11-34-03 put the camera 22 away, then 3).
        GroundCamera there = camera_;
        there.distance = *distance;
        const OwnRows foretold = ownRows(there);
        const float tall = (bottom - top) / std::max(foretold.bottom - foretold.top, 1.0f);
        if (tall > kOwnLeastTall && tall < kOwnMostTall) {
            // Taken only at a measured pitch: at the start pitch (a guess) the box says a wrong distance, which would
            // stay in the middle.
            if (zoom_.holds() && camera_.pitchMeasured) {
                zoom_.take(*distance, timeMs);
            }
            if (const std::optional<float> held = heldDistance()) {
                camera_.distance = *held;
            } else {
                const double share = 1.0 - std::exp(-std::max(0.0, timeMs - ownMs_) / kDistanceSmoothMs);
                camera_.distance = static_cast<float>(std::exp(
                    std::log(camera_.distance) + share * (std::log(*distance) - std::log(camera_.distance))));
            }
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
    // The wheel watched, the distance is held at both images (ZoomHold; at the first as the camera was then, a notch
    // between moved it): the matches say how the camera turned, tilted and went, the own box nothing (it bobs as the
    // character runs).
    const std::optional<float> held = heldDistance();
    const double heldLog = held ? std::log(static_cast<double>(*held)) : 0.0;
    const double heldFirstLog = std::log(static_cast<double>(start.firstDistance));
    if (held) {
        at[3] = heldLog;
    }
    const double priorPitch = start.firstPitch;
    const bool steep = std::max(start.firstPitch, start.secondPitch) > kOwnSteepPitch;
    const double priorDistance = std::log(start.firstDistance);
    constexpr double kDistanceSpread = 0.02; // How far the first distance may wander then (as a log).
    // How far the fit may wander from it (radians), and the pitch change between the two images.
    const double pitchSpread = holdPitch               ? 0.005
                               : start.pitchHeld       ? kHeldPitchSpread
                               : start.tiltKnown       ? kKnownTiltSpread
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
        out.assign(2 * count + 9, 0.0);
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
        if (held) {
            out[2 * count + 6] = (x[2] - heldFirstLog) / kHeldDistanceSpread * kMatchScale;
            out[2 * count + 8] = (x[3] - heldLog) / kHeldDistanceSpread * kMatchScale;
        } else if (steep) {
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
    camera_.distance = held ? *held : static_cast<float>(std::exp(at[3]));
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
    const std::optional<float> held = heldDistance();
    camera_.distance = held ? *held
                            : std::clamp(found.secondDistance, static_cast<float>(kMinDistance),
                                         static_cast<float>(kMaxDistance));
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
