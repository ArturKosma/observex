#pragma once

#include "core/Depth.h"
#include "core/Ground.h"
#include "core/Heuristics.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace observex {

// The terrain from the depth network's maps (core/Ground's GroundHeights; the user asked, 2026-11-16: "the grid points
// should wrap around terrain which we can get the shape of from the depth"), so the grid lies on hills and hollows
// instead of one flat plane through the feet.
//
// The network's map is inverse depth up to a scale and an offset of each map's own (core/Depth). On flat ground through
// the own character's feet a pixel's inverse depth (1 / how far ahead of the camera's eye, in own heights) follows from
// the camera alone, so the scale and offset are fitted to the samples whose flat-ground point is kNearFrom to
// kNearReach from the feet (least squares, then again without those more than kOutliers spreads off): the ground round
// the feet is taken as flat, as the feet are on it. With them each sample's depth says where it is: how high above the
// feet (the eye's height times 1 - its flat inverse depth / its inverse depth) and where on the ground's map (by the
// camera on the map's image). Walking up each column of samples, one nearer than the farthest ground below it (kBack)
// or rising above the ground below more steeply than kSteepest is a crown, a trunk or a wall, and left out (take()
// says how); so is one in an outline's blur (its inverse depth
// kEdge off both its neighbours' along the row, where flat ground's is the same: the map blurs a thin trunk into what
// is behind it, so it would float between them; either side of a sharp outline is like one of its neighbours). Each cell takes the kLowShare-th of its samples' heights (a trunk's or a wall's samples
// stand above the ground at its foot), one standing more than kJump above the middle of the cells round it (or 2 kJump
// below) is left out (a crown, a rock), and the rest go into the square's cells on its level, weighed by how many
// samples said it and how near they were (a sample's height error grows with its distance), at most kMostWeight
// maps deep, so newer maps keep changing it. The heights handed out: the cells seen, filled from round them where none
// was (push-pull), and drawn towards the flat ground through the feet the farther a cell is from any seen. With them
// goes what the newest map saw in front (GroundHeights::front), so the grid is hidden behind trunks, walls, crests and
// characters (groundHidden): every kStep-th pixel each way, how far a ground point there can be and be seen, its
// depth's distance and some doubt: kSolidShare of it and kSolidLeast more where the walk up its column left it out (a
// trunk, a wall, a crown: the grid's ground right behind a trunk's foot is only a little farther), else kFrontShare and
// kFrontLeast (the ground: its depth and the terrain's heights disagree by a few percent).
class DepthTerrain {
public:
    static constexpr int kStep = 3;               // Every kStep-th map pixel each way.
    static constexpr int kMargin = 1;             // Samples round what is left out (the interface, the characters).
    static constexpr float kNearFrom = 0.8f;      // The flat ground the scale and offset are fitted on (own heights
    static constexpr float kNearReach = 8.0f;     // from the feet),
    static constexpr int kLeastNear = 300;        // with at least this many samples
    static constexpr float kLeastSpread = 0.08f;  // and their flat inverse depths this spread (of their mean: looking
                                                  // straight down all are as near, so scale and offset are one).
    static constexpr float kOutliers = 3.0f;
    static constexpr float kFarthest = 40.0f;     // Samples farther from the feet (own heights) say nothing,
    static constexpr float kSureAhead = 10.0f;    // nearer the eye than this count whole, farther as distance squared.
    static constexpr float kSteepest = 1.2f;      // A hillside's steepest rise over its way (about 50 degrees),
    static constexpr float kRise = 0.01f;         // and a little more (the depth's noise): this share of the distance
    static constexpr float kRiseLeast = 0.02f;    // ahead and this many own heights.
    static constexpr float kBack = 0.05f;         // Nearer than the farthest ground below it by this share: an overhang.
    static constexpr int kBaselineRows = 32;      // The rows lower a sample is also measured against: this share.
    static constexpr float kEdge = 0.12f;         // A sample's inverse depth this share off both its left and its
                                                  // right neighbour's: in an outline's blur, left out.
    static constexpr float kLowShare = 0.3f;
    static constexpr float kJump = 0.5f;
    static constexpr float kFullSamples = 8.0f;   // A cell's samples' weights making a whole map's say.
    static constexpr float kMostWeight = 8.0f;
    static constexpr float kRecentre = 8.0f;      // The feet this far from the square's middle: it moves along.
    static constexpr float kFillFade = 0.85f;     // How a cell's sureness carries to the next cell out.
    static constexpr float kFrontShare = 0.1f;
    static constexpr float kFrontLeast = 0.2f;
    static constexpr float kSolidShare = 0.03f;
    static constexpr float kSolidLeast = 0.1f;

    DepthTerrain();

    // Takes `map` of the image shown at `timeMs`, which the camera was `camera` on (known, its pitch measured: else
    // nothing), `avoid` (screen pixels: the characters, the interface) and the map's own interface left out. Writes
    // how it went into `note`'s shape fields. Returns whether it went into the terrain.
    bool take(const DepthInput& map, double timeMs, const GroundCamera& camera, std::span<const GreyArea> avoid,
              GroundDepthNote& note);
    const GroundHeights& heights() const { return *heights_; } // Until clear().
    // Forgets the terrain (the camera was lost: the ground's map begins again).
    void clear();

private:
    struct Sample {
        int column = 0;       // Which column and row of samples,
        int row = 0;
        float network = 0.0f; // the map's value, how much its left and right neighbours' differ (the less),
        float edge = 0.0f;
        float ax = 0.0f;      // the ray through it (right and down over ahead, in the camera's view)
        float b = 0.0f;
        float flat = 0.0f;    // and its inverse depth if it were the flat ground (0 or less: above the horizon).
    };
    struct Below { // The last ground taken in a column, walking up it,
        float right = 0.0f;
        float away = 0.0f;
        float height = 0.0f;
        float farthest = 0.0f; // and the farthest from the eye (along the ground) of all taken in it.
        bool known = false;
    };
    struct Point { // Where a sample's depth puts it (taken or not), by row and column.
        float right = 0.0f;
        float away = 0.0f;
        float height = 0.0f;
        bool known = false;
    };
    struct CellSample {
        int cell = 0;
        float height = 0.0f;
        float weight = 0.0f;
    };
    // Moves the square along so `camera`'s feet are near its middle (all of it anew if not known yet).
    void recentre(const GroundCamera& camera);
    // Keeps what `map` saw in front in heights_ (GroundHeights::front): its value fitted (`fitScale`, `fitOffset`)
    // made a distance ahead, `camera` on its screen scaled by `screenScale`, its feet on `feetLevel`; solid_ says
    // which samples (`columns` x `rows`) the walk left out.
    void keepFront(const DepthInput& map, const GroundCamera& camera, float screenScale, float fitScale, float fitOffset,
                   float feetLevel, int columns, int rows);
    // Makes heights_ from the cells (push-pull fill, drawn to the flat level `flat` away from what was seen).
    void hand(float flat, double timeMs);

    std::unique_ptr<GroundHeights> heights_;
    std::uint64_t version_ = 0;
    std::vector<float> level_;  // The cells seen: their height on the square's level,
    std::vector<float> weight_; // and how surely (0: never seen).
    // Kept between maps so a map allocates nothing.
    std::vector<std::uint8_t> blocked_;
    std::vector<Sample> samples_;
    std::vector<int> near_;
    std::vector<Below> below_;
    std::vector<Point> points_;
    std::vector<std::uint8_t> solid_; // By row and column: left out by the walk up its column (1).
    std::vector<CellSample> cellSamples_;
    std::vector<int> counts_;
    std::vector<float> sorted_;
    std::vector<float> sortedWeights_;
    std::vector<float> measured_;
    std::vector<float> measuredWeight_;
    std::vector<float> fillValue_;
    std::vector<float> fillWeight_;
    std::vector<float> sure_;
};

} // namespace observex
