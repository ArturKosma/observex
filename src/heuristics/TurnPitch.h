#pragma once

#include "core/Ground.h"
#include "core/Heuristics.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace observex {

// The camera's pitch from how the screen moves while the camera turns (the user asked, 2026-11-15: "the most precise
// way to figure out the pitch given just the screen"). Lineage 2's camera never rolls, and it turns round the world's
// up. Seen from the camera that up leans by the pitch: looking level a turn slides the picture sideways, looking
// straight down it spins the picture round its middle, in between it does some of each, and how much of each says the
// pitch. So points followed from one image to the next say it, whatever the ground's shape: no plane (GroundFit's
// flat ground reads the pitch off by the ground's slope, -4 to +3 degrees on clip 2026-11-15_14-24-16), no depth. A
// still camera says nothing of it, a tilt only how far it tilted.
//
// Each follow: up to kCellsX x kCellsY points (the strongest corner in each cell, the interface and the characters
// left out) learned on the quarter size (what the ground finder reads whole, GroundMotion::kFirstLevel), followed onto
// the next image (pyramidal Lucas-Kanade on kPyramid halvings, started where the camera's motion as the follower has it
// takes them, checked by following back), then the two cameras fitted to them: the first image's pitch, the turn, the
// tilt and which way the camera went (any way: the camera circles the character, and walks with it), by how far each
// point is off the line the other image's camera says it must be on (Sampson's distance, robust). How sure the pitch
// is comes with it (its spread). Measured on the user's clips (2026-11-15, Python, the same way: scratchpad pitch/ of
// that session): about 0.08 degrees a follow on the quarter size at 15 a second, 0.13 from the half size's (2026-11-12_
// 18-58-01); against the geodata's tree bases it was right where the skyline's fit was 1.4 to 3 degrees off (2026-11-
// 15_14-24-16).
class TurnPitch {
public:
    static constexpr int kLevel = 2;   // The quarter size of a GreyView,
    static constexpr int kPyramid = 4; // and its halvings followed on: down to the 32nd of the full size.
    static constexpr int kCellsX = 32; // One point at most a cell (20 x 20 quarter-size pixels on 2560 x 1440).
    static constexpr int kCellsY = 18;
    static constexpr int kHalfWindow = 4; // A point is followed by the 9 x 9 pixels round it,
    static constexpr int kIterations = 12; // in at most this many steps a level,
    static constexpr float kLeastCorner = 40.0f; // where its corner is at least this strong (the structure tensor's
                                                 // smaller eigenvalue over 5 x 5 pixels, grey levels squared).
    static constexpr float kBackMiss = 0.3f;     // Followed back, it must land this near (quarter-size pixels).
    static constexpr int kLeastPoints = 40;      // Fewer followed: no fit.
    static constexpr float kMissScale = 1.0f;    // The fit's robust scale (full-size pixels off the line).

    // Learns the points on `image` (its kLevel whole), leaving out `avoid` (full-size areas: the interface, the
    // characters).
    void learn(const GreyView& image, std::span<const GreyArea> avoid);
    bool learned() const { return !points_.empty(); }
    int learnedCount() const { return static_cast<int>(points_.size()); }

    // How the camera went from the image learned to `image` (later; its kLevel read whole). `camera` is the camera as
    // the follower has it (its lens), `pitch` its pitch on the image learned, `turn` and `tilt` how far it turned and
    // tilted since as the follower says (radians, GroundCamera's yaw and pitch): the points are looked for from there.
    struct Pair {
        bool fitted = false; // The cameras fitted (at least kLeastPoints followed):
        int points = 0;      // points followed, of them agreeing with the fit,
        int agreeing = 0;
        float pitch = 0.0f;  // the first image's pitch (radians), how far it turned and tilted to the second,
        float turn = 0.0f;
        float tilt = 0.0f;
        float spread = 0.0f; // how sure the pitch is (its standard deviation, radians; large while not turning),
        float miss = 0.0f;   // and how far the agreeing points were off (full-size pixels, root mean square).
    };
    Pair follow(const GreyView& image, const GroundCamera& camera, float pitch, float turn, float tilt);

    void clear() { points_.clear(); }

    // A point seen on the first image (x1, y1) and the second (x2, y2), as rays: (x - middle) / focal.
    struct Ray2 {
        double x1 = 0.0;
        double y1 = 0.0;
        double x2 = 0.0;
        double y2 = 0.0;
    };
    // The fit alone (for the tests): the two cameras that best explain `rays` (seen with lens `focal` pixels), started
    // at `pitch`, `turn` and `tilt`.
    static Pair fit(std::span<const Ray2> rays, float focal, float pitch, float turn, float tilt);

private:
    // An image's kLevel and its halvings, in float grey.
    struct Level {
        int width = 0;
        int height = 0;
        std::vector<float> pixels;
        bool inside(float x, float y, float margin) const {
            return x >= margin && y >= margin && x <= static_cast<float>(width - 1) - margin &&
                   y <= static_cast<float>(height - 1) - margin;
        }
    };
    using Pyramid = std::array<Level, kPyramid>;
    static bool build(const GreyView& image, Pyramid& out);
    // Follows the point at (x, y) (kLevel pixels) from `from` to `to`, starting `guessX`, `guessY` away: where it is,
    // or false if lost.
    static bool track(const Pyramid& from, const Pyramid& to, float x, float y, float guessX, float guessY, float& outX,
                      float& outY);

    struct Point {
        float x = 0.0f; // On kLevel, pixel middles at whole numbers.
        float y = 0.0f;
    };
    Pyramid learned_;
    Pyramid seen_; // The image last followed onto (its number seenImage_: learnt on next without building it again).
    std::uint64_t seenImage_ = 0;
    std::vector<Point> points_;
    std::vector<Ray2> rays_;
};

// The pitch as the camera's turns said it, carried on by every tilt since: one number and how sure it is (a Kalman
// filter of one). Until turns have said it surely enough it does not hold the pitch (holds() false): the follower's
// pitch (the ground's motion and the depth network's maps) stands, and each pair starts from it. A pair that turned at
// least kLeastTurn and is sure to kSaySpread says the pitch; while its own spread is under kHoldSpread (surer than the
// follower's pitch, 1 to 3 degrees off on the user's clips) it holds it.
class TurnHold {
public:
    static constexpr float kLeastTurn = 0.01f;    // Radians (0.6 degrees) between two follows.
    static constexpr float kSaySpread = 0.009f;   // 0.5 degrees.
    static constexpr float kHoldSpread = 0.017f;  // 1 degree.
    static constexpr float kOtherSpread = 0.035f; // The follower's pitch, as a start: 2 degrees.
    static constexpr float kLeastSpread = 0.0005f; // A pair is never taken as surer than 0.03 degrees.
    // Each tilt adds kTiltSpread and kTiltShare of itself to the spread (they add up over a long tilt). A pair not
    // fitted, or with fewer than kLeastAgreeing points agreeing (blurred: its tilt is not to be trusted), is lost: it
    // adds kLostSpread (more than kHoldSpread: it no longer holds) and the tilt is the follower's. The user flicks
    // the camera tens of degrees between two follows: a 40-degree tilt in 70 ms, the points lost and the follower
    // saying it did not tilt, held the pitch at 46 where the camera looked level (clip 2026-11-15_21-19-16, 8.0 s) for
    // good, the depth maps not let to pull it.
    static constexpr float kTiltSpread = 0.0003f;
    static constexpr float kTiltShare = 0.02f;
    static constexpr int kLeastAgreeing = 100;
    static constexpr float kLostSpread = 0.02f;
    // A pair saying a pitch more than kOutlier of their spreads off the one held is not taken; kOutliers in a row
    // that agree with each other (as far), the last is. Not holding, one more than kFirstReach (20 degrees) off the
    // follower's: the fit found a wrong camera (85.7 degrees for 17, clip 2026-11-12_18-58-01, the first follow;
    // 84.6 and -85.2 in a fast tilt, 2026-11-12_20-16-13, 0.3 and 0.4 s); else at once (the follower started 11
    // degrees off on clip 2026-11-15_14-24-16, 2.0 s).
    static constexpr float kOutlier = 5.0f;
    static constexpr int kOutliers = 2;
    static constexpr float kFirstReach = 0.35f;

    // One follow's pair; `before` the follower's pitch on the image learned, `followerTilt` how far it says it tilted
    // since. Returns whether the pair said the pitch (taken).
    bool pair(const TurnPitch::Pair& pair, float before, float followerTilt);
    bool holds() const { return known_ && variance_ < kHoldSpread * kHoldSpread; }
    float pitch() const { return pitch_; }
    float spread() const;
    void clear() { *this = TurnHold(); }

private:
    bool known_ = false;
    float pitch_ = 0.0f;
    float variance_ = 0.0f;
    int outliers_ = 0;          // Outliers in a row, the last saying this pitch (carried on by the tilts since),
    float outlierPitch_ = 0.0f; // this surely.
    float outlierVariance_ = 0.0f;
};

} // namespace observex
