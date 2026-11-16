#include "core/Flee.h"

#include "core/Distances.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace observex {

namespace {

constexpr double kDegrees = 57.29577951308232;

// Game units, rounded to 10 as the live distance tags.
int unitsOf(float ownHeights) {
    return static_cast<int>(std::lround(ownHeights * kGameUnits / 10.0f)) * 10;
}

// A direction `right`, `away` (no feet: a way) on the ground's map.
void directionOnMap(const GroundCamera& camera, float right, float away, float& x, float& y) {
    const float c = std::cos(camera.yaw);
    const float s = std::sin(camera.yaw);
    x = c * right + s * away;
    y = -s * right + c * away;
    const float length = std::hypot(x, y);
    if (length > 1e-6f) {
        x /= length;
        y /= length;
    }
}

bool inAny(std::span<const FleeWalker::Box> boxes, float x, float y, float margin) {
    return std::ranges::any_of(boxes, [&](const FleeWalker::Box& box) {
        return x >= box.x0 - margin && x <= box.x1 + margin && y >= box.y0 - margin && y <= box.y1 + margin;
    });
}

// Whether screen point (x, y) can be clicked: in from the screen's sides, off every box to avoid.
bool clickable(const FleeWalker::Input& in, float x, float y) {
    const float edgeX = in.width * FleeWalker::kEdge;
    const float edgeY = in.height * FleeWalker::kEdge;
    return x >= edgeX && x <= in.width - edgeX && y >= edgeY && y <= in.height - edgeY &&
           !inAny(in.characters, x, y, FleeWalker::kMargin) && !inAny(in.avoid, x, y, FleeWalker::kMargin);
}

// The terrain the ways go over: the depth's, on the ground finder's map; null: the flat ground.
const GroundHeights* terrainOf(const FleeWalker::Input& in) {
    return in.cameraFollowed && in.camera.known && in.heights != nullptr && in.heights->known ? in.heights : nullptr;
}

std::string who(const FleeWalker::Input& in) {
    return in.targetName.empty() ? std::string("the target") : std::string(in.targetName);
}

// How a way turned from straight, for the log: "straight", "30 degrees right", "45 degrees left".
std::string turnText(float turn) {
    const int degrees = static_cast<int>(std::lround(std::abs(turn) * kDegrees));
    if (degrees == 0) {
        return "straight";
    }
    return std::to_string(degrees) + " degrees " + (turn > 0.0f ? "right" : "left");
}

void addLine(std::string& log, const std::string& line) {
    if (!log.empty()) {
        log += '\n';
    }
    log += line;
}

} // namespace

void FleeWalker::start() {
    *this = FleeWalker();
}

bool FleeWalker::blocked(const GroundCamera& camera, float right, float away, double now) const {
    float x = 0.0f;
    float y = 0.0f;
    directionOnMap(camera, right, away, x, y);
    const float spread = std::cos(kBlockedSpread);
    return std::ranges::any_of(blocked_, [&](const Blocked& b) { return b.until > now && x * b.mapX + y * b.mapY >= spread; });
}

FleeWalker::Way FleeWalker::plan(const Input& in, const GroundCamera& camera, float fromRight, float fromAway,
                                 bool fromKnown) const {
    // Straight away from the target, through the feet; not known (or on the feet): away from the camera.
    float baseRight = 0.0f;
    float baseAway = 1.0f;
    if (fromKnown) {
        if (const float length = std::hypot(fromRight, fromAway); length >= kNearFeet) {
            baseRight = -fromRight / length;
            baseAway = -fromAway / length;
        }
    }
    const GroundHeights* terrain = terrainOf(in);
    const float feetLevel = terrain != nullptr ? heightOnMap(*terrain, camera.feetX, camera.feetY) : 0.0f;
    Way best;
    for (int i = 0; i <= 2 * kTurns; ++i) { // Straight, then 1 step right, 1 left, 2 right, ...
        const int steps = (i + 1) / 2;
        const float turn = i == 0 ? 0.0f : (i % 2 == 1 ? 1.0f : -1.0f) * static_cast<float>(steps) * kTurnStep;
        // Turned clockwise seen from above (+ right): away is up, right is right.
        const float c = std::cos(turn);
        const float s = std::sin(turn);
        const float wayRight = baseRight * c + baseAway * s;
        const float wayAway = -baseRight * s + baseAway * c;
        if (blocked(camera, wayRight, wayAway, in.now)) {
            continue;
        }
        Way way;
        way.turn = turn;
        float lastUp = 0.0f;
        for (int step = 1; static_cast<float>(step) * kPathStep <= kFarthest + 1e-4f; ++step) {
            const float along = static_cast<float>(step) * kPathStep;
            const float right = wayRight * along;
            const float away = wayAway * along;
            float up = 0.0f;
            if (terrain != nullptr) {
                float mapX = 0.0f;
                float mapY = 0.0f;
                groundToMap(camera, right, away, mapX, mapY);
                const float level = heightOnMap(*terrain, mapX, mapY);
                up = level - feetLevel;
                const float rise = up - lastUp;
                if (rise > kSteepest || rise < -kDeepest) {
                    break; // A wall, a cliff.
                }
                lastUp = up;
                if (groundHidden(*terrain, mapX, mapY, level)) {
                    // Behind something the depth saw: a trunk, a wall, a crest; but not a character (they walk
                    // through each other), whose box it is on the screen.
                    const GroundPoint shown = worldToScreen(camera, right, away, up);
                    if (!shown.ahead || !inAny(in.characters, shown.x, shown.y, kMargin)) {
                        break;
                    }
                }
            }
            const GroundPoint shown = worldToScreen(camera, right, away, up);
            if (!shown.ahead) {
                break; // Behind the camera.
            }
            if (along >= kNearest - 1e-4f && clickable(in, shown.x, shown.y)) {
                way.found = true;
                way.right = right;
                way.away = away;
                way.reach = along;
                way.x = shown.x;
                way.y = shown.y;
            }
        }
        if (!way.found) {
            continue;
        }
        if (way.reach >= kGoodReach - 1e-4f) {
            return way; // The straightest that reaches well.
        }
        if (!best.found || way.reach > best.reach + 1e-4f) {
            best = way;
        }
    }
    return best;
}

FleeWalker::Output FleeWalker::step(const Input& in) {
    Output out;
    // Nothing to flee from: done.
    if (!in.hasTarget) {
        if (noTargetSince_ < 0.0) {
            noTargetSince_ = in.now;
        }
        if (in.now - noTargetSince_ >= kNoTargetSeconds) {
            out.done = true;
            out.log = "Flee: done, no target to flee from";
            return out;
        }
    } else {
        noTargetSince_ = -1.0;
        if (in.targetHp >= 0.0f && in.targetHp <= kDeadHp) {
            out.done = true;
            out.log = "Flee: done, " + who(in) + " is dead";
            return out;
        }
    }

    // The camera: the ground finder's (on its map), else guessed (its map is then its own right and away).
    const bool onMap = in.cameraFollowed && in.camera.known;
    GroundCamera camera;
    if (onMap) {
        camera = in.camera;
    } else {
        camera = groundCamera(in.pitchKnown ? in.pitch : kGuessPitch, kGuessDistance, in.width, in.height);
        if (in.own) {
            if (const std::optional<float> distance = distanceForOwnBottom(camera, in.ownBox.y1)) {
                camera.distance = *distance;
            }
        }
    }
    if (onMap != onMap_) { // Another kind of map: what was kept on the other means nothing on it.
        onMap_ = onMap;
        known_ = false;
        saidWay_ = false;
        movedAt_ = -1.0;
        blocked_.clear();
    }
    std::erase_if(blocked_, [&](const Blocked& b) { return b.until <= in.now; });

    // Where the target is: its feet on the flat ground, kept on the map.
    bool seenNow = false;
    if (in.targetSeen) {
        const float footX = (in.targetBox.x0 + in.targetBox.x1) * 0.5f;
        if (const std::optional<GroundSpot> spot = groundUnder(camera, nullptr, footX, in.targetBox.y1)) {
            groundToMap(camera, spot->right, spot->away, targetX_, targetY_);
            seenNow = true;
            if (known_ && saidLost_) {
                addLine(out.log, "Flee: " + who(in) + " seen again");
            }
            known_ = true;
            seenAt_ = in.now;
            saidLost_ = false;
        }
    }
    if (!seenNow && known_ && !saidLost_ && in.now - seenAt_ >= kLostSeconds) {
        addLine(out.log, "Flee: lost sight of " + who(in) + ", fleeing from where it was last seen");
        saidLost_ = true;
    }
    float fromRight = 0.0f;
    float fromAway = 0.0f;
    if (known_) {
        mapToGround(camera, targetX_, targetY_, fromRight, fromAway);
    }

    // Stuck: clicked, and the feet did not move since (only on the ground finder's map: a guess has no feet).
    if (onMap && movedAt_ >= 0.0) {
        if (std::hypot(camera.feetX - feetX_, camera.feetY - feetY_) >= kMoved) {
            feetX_ = camera.feetX;
            feetY_ = camera.feetY;
            movedAt_ = in.now;
        } else if (in.now - movedAt_ >= kStuckSeconds) {
            blocked_.push_back({wayX_, wayY_, in.now + kBlockedSeconds});
            char line[200];
            std::snprintf(line, sizeof(line),
                          "Flee: not moving %.1f s after clicking; that way blocked for %.0f s, trying another",
                          in.now - movedAt_, kBlockedSeconds);
            addLine(out.log, line);
            movedAt_ = in.now;
            nextClick_ = in.now; // Another way at once.
            saidWay_ = false;
        }
    }

    const Way way = plan(in, camera, fromRight, fromAway, known_);
    if (!way.found) {
        glide_.stop();
        if (nowhereSince_ < 0.0) {
            nowhereSince_ = in.now;
        }
        if (!saidNowhere_) {
            addLine(out.log, "Flee: nowhere to click away from " + who(in) +
                                 " (the ground that way hidden, too steep, off the screen, under the interface or a "
                                 "character); waiting");
            saidNowhere_ = true;
        }
        if (in.now - nowhereSince_ >= kNowhereSeconds && !blocked_.empty()) {
            blocked_.clear();
            addLine(out.log, "Flee: forgot the blocked ways");
            nowhereSince_ = in.now;
        }
        return out;
    }
    nowhereSince_ = -1.0;
    saidNowhere_ = false;
    out.aiming = true;
    out.aimX = way.x;
    out.aimY = way.y;

    // The cursor onto it; a click once there, every kClickEvery.
    const double toX = in.left + static_cast<double>(way.x) * in.scaleX - in.cursorX;
    const double toY = in.top + static_cast<double>(way.y) * in.scaleY - in.cursorY;
    const CursorGlide::Move move = glide_.step(toX, toY, in.dt);
    out.dx = move.dx;
    out.dy = move.dy;
    const float cursorX = static_cast<float>((in.cursorX - in.left) / std::max(1e-9, in.scaleX));
    const float cursorY = static_cast<float>((in.cursorY - in.top) / std::max(1e-9, in.scaleY));
    if (in.canClick && in.now >= nextClick_ && std::hypot(toX, toY) <= kAt && clickable(in, cursorX, cursorY)) {
        out.click = true;
        nextClick_ = in.now + kClickEvery;
        directionOnMap(camera, way.right, way.away, wayX_, wayY_);
        if (onMap && movedAt_ < 0.0) {
            movedAt_ = in.now;
            feetX_ = camera.feetX;
            feetY_ = camera.feetY;
        }
        // Said on the first click and when the way turned well off the one said.
        if (!saidWay_ || wayX_ * saidX_ + wayY_ * saidY_ < std::cos(kLogTurn)) {
            saidWay_ = true;
            saidX_ = wayX_;
            saidY_ = wayY_;
            std::string line = "Flee: ";
            if (known_) {
                line += "from " + who(in) + " at " + std::to_string(unitsOf(std::hypot(fromRight, fromAway)));
                if (!seenNow) {
                    line += " (where last seen)";
                }
            } else {
                line += who(in) + " not seen yet, away from the camera";
            }
            line += ": clicked the ground " + std::to_string(unitsOf(way.reach)) + " away, " + turnText(way.turn);
            if (terrainOf(in) == nullptr) {
                line += " (no terrain yet: flat ground)";
            }
            addLine(out.log, line);
        }
    }
    return out;
}

} // namespace observex
