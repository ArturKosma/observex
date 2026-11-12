#pragma once

#include "core/Heuristics.h"

#include <array>
#include <span>
#include <vector>

namespace observex {

// Where the game's camera looks, from Lineage 2's minimap (the user asked, 2026-11-12: the ground's yaw drifted, and
// a camera tilt was taken for turns). The minimap is a map with north up, round the own character's arrow; a light
// cone 92 degrees wide from the arrow shows where the camera looks and turns with it, whatever its pitch: on clip
// 2026-11-12_15-50-36 the same at frames 641 and 833 of a tilt with no turn; on clip 2026-11-11_22-10-08 its turns
// were the ground's (heuristics/GroundMotion), 260 degrees each way, to 4 degrees, with no lag at 15 images a second
// (scratchpad ground/cone.py of that session). Its two edges are read off a profile round its tip: the grey between
// kInner and kOuter pixels from the tip at each half degree, the steepest rise and fall kWidth apart; the heading is
// half way between them. Steady to a fraction of a degree while the camera is still.
//
// The tip is found once (and again when the cone has not been seen for a while): the minimap is a framed window
// (core/UiLayout's pieces), its tip in the middle of the map below the window's top strip, give or take kReach
// pixels; where the cone's edges are sharpest is the tip. Then only a small part round the tip is read: areas().
// Full-size pixels throughout (a GreyView's level 0), their middles at whole numbers. Use one from one thread.
class MinimapCone {
public:
    static constexpr float kInner = 10.0f; // From the tip, in pixels: past the own character's arrow,
    static constexpr float kOuter = 28.0f; // short of where the cone fades out (about 40).
    static constexpr float kWidth = 1.606f;     // The cone, in radians (92 degrees: 91 to 94 measured),
    static constexpr float kWidthSlack = 0.09f; // give or take this.
    // Seen where both edges rise at least this many greys and the cone is at least this much lighter than the map
    // beside it (the weakest of 1690 frames of two clips: 33 and 72).
    static constexpr float kMinEdge = 12.0f;
    static constexpr float kMinContrast = 25.0f;
    // A window that may be the minimap: at least this many pixels each way, about square.
    static constexpr int kLeastSide = 150;
    static constexpr float kMostLong = 1.33f;
    static constexpr int kTitle = 18; // Its top strip, in pixels (the window's frame to the map).
    static constexpr int kReach = 5;  // How far from where the window puts it the tip is looked for, in pixels.
    static constexpr int kCandidates = 4; // Windows looked at, at most.
    // The tip is looked for again after the cone was not seen for this long (the minimap moved or closed; a time, as
    // it is read on every image between the ground's follows too, engine/GroundFinder);
    static constexpr double kMissMs = 450.0;
    // a look that found none is tried again this long after.
    static constexpr double kRetryMs = 1000.0;

    // The full-size parts read() reads on the image at `timeMs`: round the tip, or round where each window among
    // `windows` that may be the minimap would put it (none until kRetryMs after a look found none). Writes up to `max`
    // into `out`; returns how many.
    int areas(double timeMs, std::span<const GreyArea> windows, GreyArea* out, int max);
    // Reads the cone on `full` (the image's full-size level, the areas() given read). Returns whether it was seen.
    bool read(const GreyLevel& full);

    bool seen() const { return seen_; }
    // Where the camera looks on the last read() that saw the cone: radians clockwise from north (the minimap's up),
    // 0 to 2 pi.
    float heading() const { return heading_; }
    bool locked() const { return locked_; }
    float tipX() const { return tipX_; }
    float tipY() const { return tipY_; }
    void clear() { *this = MinimapCone(); }

private:
    // The cone on a profile: where its edges are (in the profile's steps, a fraction between), how sharp, and how much
    // lighter it is than beside it.
    struct Edges {
        float rise = 0.0f;
        float fall = 0.0f;
        float strength = 0.0f; // The two edges' rises added.
        bool seen = false;
    };
    // The profile round (`x`, `y`) at `steps` angles, the radii `radiusStep` apart, into profile_; false if it reaches
    // off `full`.
    bool profile(const GreyLevel& full, float x, float y, int steps, float radiusStep);
    Edges edges();
    // Looks for the tip round (`x`, `y`); true (tipX_, tipY_ set) if the cone is seen there.
    bool findTip(const GreyLevel& full, float x, float y);
    bool readAt(const GreyLevel& full, float x, float y);

    std::vector<float> profile_;
    std::vector<float> rises_;
    std::array<float, kCandidates * 2> candidates_{}; // Where the windows put the tip (x, y each), for read().
    int candidateCount_ = 0;
    bool locked_ = false;
    float tipX_ = 0.0f;
    float tipY_ = 0.0f;
    double missSinceMs_ = -1.0; // The first read since the last one that saw the cone; -1: none.
    double timeMs_ = 0.0;    // The image areas() was last for.
    double retryMs_ = -1e30; // No look for the tip before this.
    bool seen_ = false;
    float heading_ = 0.0f;
};

} // namespace observex
