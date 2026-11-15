#pragma once

#include "core/Ground.h"
#include "core/UiLayout.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace observex {

// Lineage 2's geodata: the game server's own map of every floor a character can stand on (the user asked,
// 2026-11-15: exact distances to monsters, instead of guessing depth; the public L2J geodata matches the user's
// Chronicle 1 server's /loc heights exactly, checked at two places). World units: x east, y south, z up; the own
// character is about kUnitsPerOwnHeight tall (measured against a tree trunk on clip 2026-11-15_14-24-16).
//
// The files are L2J's ".l2j" regions (models/geodata/RX_RY.l2j, e.g. 19_18.l2j), each 32768 x 32768 units: 256 x 256
// blocks of 8 x 8 cells of 16 units. A block is flat (one height), complex (each cell one floor) or multilayer (each
// cell its floors, e.g. the ground under a bridge and the bridge). A floor is its height and which ways a character
// may leave the cell (NSWE: a wall or a trunk's edge leaves some out). Region 20_18 begins at world x 0, y 0.
inline constexpr int kGeoCell = 16;
inline constexpr int kGeoRegionCells = 2048;
inline constexpr int kGeoRegionX = 20;
inline constexpr int kGeoRegionY = 18;
inline constexpr float kUnitsPerOwnHeight = 47.0f;

struct GeoFloor {
    std::int16_t z = 0;
    std::uint8_t nswe = 15; // Bits of the ways out (15: all four).
};

// The global cell (counted from region 0_0's corner) world x or y is in, and the region and its cell in it.
inline int geoCellX(double x) {
    return static_cast<int>(std::floor(x / kGeoCell)) + kGeoRegionX * kGeoRegionCells;
}
inline int geoCellY(double y) {
    return static_cast<int>(std::floor(y / kGeoCell)) + kGeoRegionY * kGeoRegionCells;
}

// One region file, as read.
class GeoRegion {
public:
    // Reads `path`; false (with *error, if given) if it cannot be read or is not a region (a block it does not know,
    // blocks past its end).
    bool load(const std::filesystem::path& path, std::string* error);
    bool parse(std::vector<std::uint8_t> bytes, std::string* error);
    // The floors of cell (x, y) inside the region (0 to kGeoRegionCells - 1 each): up to `max` into `out`; how many.
    int floors(int x, int y, GeoFloor* out, int max) const;

private:
    std::vector<std::uint8_t> bytes_;
    std::vector<std::uint32_t> blocks_; // Where each block (bx * 256 + by) begins in bytes_: its type.
};

// The regions of a folder, each read the first time it is needed. Any thread. Only the kKept regions nearest the
// one read last stay loaded (each is up to 25 MB, and the whole set is 203 of them, 1 GB: a teleport across the
// world reads new ones); one still held where it is used stays alive until let go.
class Geodata {
public:
    static constexpr int kKept = 9; // A 3 x 3 round where the character is (a field spans at most 2 x 2).

    explicit Geodata(std::filesystem::path folder) : folder_(std::move(folder)) {}

    // The region RX_RY, or null if its file is not there or cannot be read (tried once).
    std::shared_ptr<const GeoRegion> region(int rx, int ry);
    // How many regions are loaded now (at most kKept).
    int loadedRegions();
    // The floors of global cell (gx, gy) (geoCellX/Y): up to `max`; how many (0: no geodata there).
    int floors(int gx, int gy, GeoFloor* out, int max);
    // The floor at world (x, y) nearest height `z` (a floor at most kStep above it first: where the character
    // stands); none without geodata there.
    static constexpr int kStep = 48;
    std::optional<GeoFloor> floorNear(double x, double y, double z);
    // The region file's name for world (x, y), e.g. "19_18.l2j".
    static std::string regionName(double x, double y);
    const std::filesystem::path& folder() const { return folder_; }

private:
    std::filesystem::path folder_;
    std::mutex mutex_;
    std::map<int, std::shared_ptr<const GeoRegion>> regions_; // By rx * 1000 + ry; null: no file (or a wrong one).
};

// The ground round a point, as the Geodata flag draws it (app/GeodataView): kReach cells each way. Where a cell has
// several floors (tree crowns, roofs, a cave under the ground), the one nearest the ground round it (the single-floor
// cells nearest it; the character's height where there are none). The geodata keeps the tops of what stands on the
// ground (tree trunks, rocks: a cell a character cannot walk into, as high as its top), so what sticks up more than
// kBump above the ground round it, narrower than kOpening cells (a grey opening keeps slopes and hills whatever their
// size), is cut down to the ground round it and marked blocked (scratch tools of 2026-11-15: a trunk's top held the
// ground 2,000 units up under it otherwise). Read-only once built: any thread.
class GeoField {
public:
    static constexpr int kReach = 320; // Cells each way from the middle: 5120 units.
    static constexpr int kOpening = 11;
    static constexpr float kBump = 40.0f;

    // Built round world (x, y), floors chosen near height `z` where nothing else says. Takes some 10-40 ms.
    static std::shared_ptr<const GeoField> build(Geodata& geodata, double x, double y, double z);

    // Whether world (x, y) is at least `margin` units inside it.
    bool covers(double x, double y, double margin) const;
    // The ground's height at world (x, y), between the cells' middles; NaN outside or without geodata there.
    float ground(double x, double y) const;
    // Whether the cell at world (x, y) is cut down (a trunk, a rock) or not walkable every way.
    bool blocked(double x, double y) const;
    // Cells without geodata (their region file missing), and the first missing region's file name.
    int missing() const { return missing_; }
    const std::string& missingRegion() const { return missingRegion_; }
    double middleX() const { return middleX_; }
    double middleY() const { return middleY_; }

private:
    int side_ = 0;          // 2 * kReach + 1.
    int cellX_ = 0;         // The global cell of [0, 0].
    int cellY_ = 0;
    double middleX_ = 0.0;
    double middleY_ = 0.0;
    std::vector<float> ground_;       // By x * side_ + y; NaN without geodata.
    std::vector<std::uint8_t> blocked_;
    int missing_ = 0;
    std::string missingRegion_;
};

// The grid the Geodata flag draws, worked out for one camera (the bench draws the same).
struct GeoGrid {
    struct Line {
        float x0 = 0.0f; // On the screen, in pixels.
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
        float rise = 0.0f;     // How high it is against the feet (units): colours it.
        float strength = 0.0f; // 0 to 1: fainter farther out.
        bool fine = false;     // Of the fine cells near the feet.
    };
    struct Cell { // A blocked fine cell: its corners on the screen.
        float x[4] = {};
        float y[4] = {};
        float strength = 0.0f;
    };
    std::vector<Line> lines;
    std::vector<Cell> blocked;
    std::vector<float> feet; // A ring round the feet: x, y pairs (empty if not all of it ahead of the camera).
};

// The point (x, y, z) of the world on the screen, as `camera` circles the own character standing at `feet` (x, y, z):
// false (nothing set) if it is not well ahead of the camera.
bool worldOnScreen(const GroundCamera& camera, const double feet[3], double x, double y, double z, float& sx,
                   float& sy);

// `camera` (the ground finder's) with its pitch set right on the geodata's ground round `feet`: the ground finder takes
// the ground it sees for level, so its pitch is off by the ground's slope away from the camera (2026-11-15, clip
// 2026-11-15_14-24-16: -4 degrees facing a hill, +3 facing the plain below). The slope is the geodata's where the
// screen's lower middle meets the ground (a grid of rays as `camera` sees them, a plane through where they meet it,
// within 1,500 units); then the follower's own lean (kFollowerBias, 1.95 degrees there) is taken off: with both, the
// pitch was within 0.8 degrees of the one the clip's landmarks need. `camera` as it is with too little ground in view,
// or when its pitch is the camera's turns' (GroundCamera::pitchByTurns: no slope leans that).
GroundCamera geoCamera(const GeoField& field, const GroundCamera& camera, const double feet[3], float width,
                       float height);

// The grid of `field` round `feet` (world x, y, z) as `camera` sees it on a `width` x `height` screen: lines every
// kFine units out to kFineReach of the feet, every kCoarse units out to kCoarseReach (fading out), fixed to the
// world (they never slide as the character walks); the fine cells that are blocked; a ring round the feet. The game's
// interface (`clear`: its windows) is left clear: lines are cut where they go under it, cells touching it left out.
struct GeoGridSize {
    static constexpr int kFine = 32;
    static constexpr int kFineReach = 1216;
    static constexpr int kCoarse = 96;
    static constexpr int kCoarseReach = 4032;
};
void geoGrid(const GeoField& field, const GroundCamera& camera, const double feet[3], float width, float height,
             std::span<const UiPiece> clear, GeoGrid& out);

} // namespace observex
