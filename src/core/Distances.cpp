#include "core/Distances.h"

#include <cmath>

namespace observex {

namespace {

constexpr float kNearest = 0.05f;   // Along the ray from the eye (own heights): nearer is the camera itself.
constexpr float kStepAlong = 0.25f; // How far a step of the search goes along the ground (own heights).
constexpr int kHalvings = 8;        // Then halving the step round where it went under: 0.001 own heights.

// The ray from `camera`'s eye through screen pixel (x, y): the eye and how far it goes right, away and up for each
// own height it goes ahead (along where the camera looks).
struct Ray {
    GroundSpot eye;
    GroundSpot step;
    GroundSpot at(float ahead) const {
        return {eye.right + ahead * step.right, eye.away + ahead * step.away, eye.up + ahead * step.up};
    }
};

Ray rayThrough(const GroundCamera& camera, float x, float y) {
    const float s = std::sin(camera.pitch);
    const float c = std::cos(camera.pitch);
    const float a = (x - camera.middleX) / camera.focal; // Right and down for each own height ahead.
    const float b = (y - camera.middleY) / camera.focal;
    // The eye circles kGroundTarget over the feet at the camera's distance, looking down at its pitch (as
    // worldToScreen: there ahead is 0 and down is 0).
    return {{0.0f, -camera.distance * c, kGroundTarget + camera.distance * s}, {a, c - s * b, -s - c * b}};
}

// The ground's height under `spot` above the feet (own heights).
float heightUnder(const GroundCamera& camera, const GroundHeights& heights, const GroundSpot& spot) {
    float mapX = 0.0f;
    float mapY = 0.0f;
    groundToMap(camera, spot.right, spot.away, mapX, mapY);
    return groundHeight(heights, camera, mapX, mapY);
}

} // namespace

std::optional<GroundSpot> groundUnder(const GroundCamera& camera, const GroundHeights* heights, float x, float y) {
    if (!camera.known || camera.focal <= 0.0f) {
        return std::nullopt;
    }
    const Ray ray = rayThrough(camera, x, y);
    // The flat ground: where the ray's height reaches 0.
    std::optional<GroundSpot> flat;
    if (ray.step.up < -1e-6f) {
        const float ahead = -ray.eye.up / ray.step.up;
        if (ahead > kNearest) {
            flat = ray.at(ahead);
            flat->up = 0.0f;
        }
    }
    if (heights == nullptr || !heights->known) {
        return flat;
    }
    // The terrain: steps along the ray until it goes from over the ground to under it, then halves the last step round
    // there. Where it starts under the ground (the terrain under and behind the camera is never seen, only filled from
    // round it: on clip 2026-11-15_14-24-16 it stood over the eye, and every ray "landed" there, 4 to 6 own heights
    // from the feet, for monsters 13 to 47 away), it goes on until it is over it: the eye is never in the ground.
    const float across = std::hypot(ray.step.right, ray.step.away); // Along the ground for each own height ahead.
    if (across < 1e-6f) {                                           // Straight down: under the eye.
        return flat;
    }
    const float step = kStepAlong / across;
    const float farthest = (kFarthestTerrain + camera.distance) / across;
    float before = kNearest;
    bool over = ray.at(kNearest).up > heightUnder(camera, *heights, ray.at(kNearest));
    for (float ahead = kNearest + step; ahead <= farthest; ahead += step) {
        const GroundSpot spot = ray.at(ahead);
        const bool overNow = spot.up > heightUnder(camera, *heights, spot);
        if (overNow || !over) {
            over = overNow;
            before = ahead;
            continue;
        }
        float above = before;
        float under = ahead;
        for (int i = 0; i < kHalvings; ++i) {
            const float middle = 0.5f * (above + under);
            const GroundSpot there = ray.at(middle);
            (there.up > heightUnder(camera, *heights, there) ? above : under) = middle;
        }
        GroundSpot landed = ray.at(0.5f * (above + under));
        landed.up = heightUnder(camera, *heights, landed);
        return landed;
    }
    return flat; // Over every hill in reach: the flat ground farther out.
}

float alongGround(const GroundSpot& spot) {
    return std::hypot(spot.right, spot.away);
}

namespace {

std::optional<float> distanceAt(const GroundCamera& camera, const GroundHeights* heights, float x, float y) {
    const std::optional<GroundSpot> spot = groundUnder(camera, heights, x, y);
    if (!spot || alongGround(*spot) > kFarthestDistance) {
        return std::nullopt;
    }
    return alongGround(*spot);
}

} // namespace

std::optional<float> characterDistance(const GroundCamera& camera, const GroundHeights* heights, const Detection& box) {
    float x = 0.0f;
    float y = 0.0f;
    characterFoot(box, x, y);
    return distanceAt(camera, heights, x, y);
}

std::optional<float> dropDistance(const GroundCamera& camera, const GroundHeights* heights, const Detection& box) {
    float x = 0.0f;
    float y = 0.0f;
    dropFoot(box, x, y);
    return distanceAt(camera, heights, x, y);
}

} // namespace observex
