#include "core/Geodata.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

namespace observex {

namespace {

constexpr int kBlocks = 256; // Each way in a region.
constexpr int kBlockCells = 8;

std::uint16_t word(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

// A complex or multilayer cell's floor: its height in the upper 12 bits (signed, halved), its ways out in the lower 4.
GeoFloor floorOf(std::uint16_t value) {
    return {static_cast<std::int16_t>(static_cast<std::int16_t>(value & 0xFFF0) >> 1),
            static_cast<std::uint8_t>(value & 0x0F)};
}

} // namespace

bool GeoRegion::load(const std::filesystem::path& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error != nullptr) {
            *error = "cannot open " + path.filename().string();
        }
        return false;
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    return parse(std::move(bytes), error);
}

bool GeoRegion::parse(std::vector<std::uint8_t> bytes, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error != nullptr) {
            *error = why;
        }
        bytes_.clear();
        blocks_.clear();
        return false;
    };
    std::vector<std::uint32_t> blocks(static_cast<std::size_t>(kBlocks) * kBlocks);
    std::size_t at = 0;
    for (std::uint32_t& block : blocks) {
        if (at >= bytes.size()) {
            return fail("its blocks end too soon");
        }
        block = static_cast<std::uint32_t>(at);
        switch (bytes[at++]) {
        case 0: // Flat: one height.
            at += 2;
            break;
        case 1: // Complex: a floor a cell.
            at += 2 * kBlockCells * kBlockCells;
            break;
        case 2: // Multilayer: each cell its floors.
            for (int cell = 0; cell < kBlockCells * kBlockCells; ++cell) {
                if (at >= bytes.size()) {
                    return fail("its blocks end too soon");
                }
                at += 1 + 2 * static_cast<std::size_t>(bytes[at]);
            }
            break;
        default:
            return fail("a block of a kind it does not know");
        }
        if (at > bytes.size()) {
            return fail("its blocks end too soon");
        }
    }
    bytes_ = std::move(bytes);
    blocks_ = std::move(blocks);
    return true;
}

int GeoRegion::floors(int x, int y, GeoFloor* out, int max) const {
    if (blocks_.empty() || max <= 0 || x < 0 || y < 0 || x >= kGeoRegionCells || y >= kGeoRegionCells) {
        return 0;
    }
    std::size_t at = blocks_[static_cast<std::size_t>((x / kBlockCells) * kBlocks + y / kBlockCells)];
    const int cell = (x % kBlockCells) * kBlockCells + y % kBlockCells;
    switch (bytes_[at++]) {
    case 0:
        out[0] = {static_cast<std::int16_t>(word(bytes_, at)), 15};
        return 1;
    case 1:
        out[0] = floorOf(word(bytes_, at + 2 * static_cast<std::size_t>(cell)));
        return 1;
    default: {
        for (int skip = 0; skip < cell; ++skip) {
            at += 1 + 2 * static_cast<std::size_t>(bytes_[at]);
        }
        const int count = bytes_[at++];
        const int kept = std::min(count, max);
        for (int i = 0; i < kept; ++i) {
            out[i] = floorOf(word(bytes_, at + 2 * static_cast<std::size_t>(i)));
        }
        return kept;
    }
    }
}

std::shared_ptr<const GeoRegion> Geodata::region(int rx, int ry) {
    const std::scoped_lock lock(mutex_);
    const int key = rx * 1000 + ry;
    if (const auto found = regions_.find(key); found != regions_.end()) {
        return found->second;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "%d_%d.l2j", rx, ry);
    auto region = std::make_shared<GeoRegion>();
    std::shared_ptr<const GeoRegion> kept;
    if (std::error_code error; std::filesystem::exists(folder_ / name, error) && region->load(folder_ / name, nullptr)) {
        kept = std::move(region);
    }
    regions_[key] = kept;
    if (kept != nullptr) { // Too many loaded: the ones farthest from this one are let go (read again if needed).
        int loaded = 0;
        for (const auto& [where, read] : regions_) {
            loaded += read != nullptr ? 1 : 0;
        }
        for (; loaded > kKept; --loaded) {
            auto farthest = regions_.end();
            int farthestBy = -1;
            for (auto it = regions_.begin(); it != regions_.end(); ++it) {
                const int by = std::max(std::abs(it->first / 1000 - rx), std::abs(it->first % 1000 - ry));
                if (it->second != nullptr && by > farthestBy) {
                    farthest = it;
                    farthestBy = by;
                }
            }
            regions_.erase(farthest);
        }
    }
    return kept;
}

int Geodata::loadedRegions() {
    const std::scoped_lock lock(mutex_);
    return static_cast<int>(std::count_if(regions_.begin(), regions_.end(),
                                          [](const auto& entry) { return entry.second != nullptr; }));
}

int Geodata::floors(int gx, int gy, GeoFloor* out, int max) {
    if (gx < 0 || gy < 0) {
        return 0;
    }
    const std::shared_ptr<const GeoRegion> found = region(gx / kGeoRegionCells, gy / kGeoRegionCells);
    return found != nullptr ? found->floors(gx % kGeoRegionCells, gy % kGeoRegionCells, out, max) : 0;
}

std::optional<GeoFloor> Geodata::floorNear(double x, double y, double z) {
    GeoFloor here[16];
    const int count = floors(geoCellX(x), geoCellY(y), here, 16);
    if (count == 0) {
        return std::nullopt;
    }
    // The highest at most kStep above `z` (what it stands on), else the lowest above it.
    const GeoFloor* best = nullptr;
    for (int i = 0; i < count; ++i) {
        if (here[i].z <= z + kStep && (best == nullptr || here[i].z > best->z)) {
            best = &here[i];
        }
    }
    if (best == nullptr) {
        best = &here[0];
        for (int i = 1; i < count; ++i) {
            best = here[i].z < best->z ? &here[i] : best;
        }
    }
    return *best;
}

std::string Geodata::regionName(double x, double y) {
    char name[32];
    std::snprintf(name, sizeof(name), "%d_%d.l2j", geoCellX(x) / kGeoRegionCells, geoCellY(y) / kGeoRegionCells);
    return name;
}

std::shared_ptr<const GeoField> GeoField::build(Geodata& geodata, double x, double y, double z) {
    auto field = std::make_shared<GeoField>();
    const int side = 2 * kReach + 1;
    field->side_ = side;
    field->cellX_ = geoCellX(x) - kReach;
    field->cellY_ = geoCellY(y) - kReach;
    field->middleX_ = x;
    field->middleY_ = y;
    const std::size_t cells = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    constexpr float kNone = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> raw(cells, kNone);
    std::vector<std::uint8_t> nswe(cells, 15);
    std::vector<std::uint8_t> single(cells, 0);
    // Cells with several floors: their index and their floors (up to 8), chosen once the ground round them is known.
    struct Many {
        std::size_t index = 0;
        int count = 0;
        GeoFloor floors[8];
    };
    std::vector<Many> many;
    for (int i = 0; i < side; ++i) {
        for (int j = 0; j < side; ++j) {
            const std::size_t index = static_cast<std::size_t>(i) * static_cast<std::size_t>(side) +
                                      static_cast<std::size_t>(j);
            Many m;
            m.index = index;
            m.count = geodata.floors(field->cellX_ + i, field->cellY_ + j, m.floors, 8);
            if (m.count == 0) {
                if (field->missing_++ == 0) {
                    field->missingRegion_ =
                        Geodata::regionName(static_cast<double>(field->cellX_ + i - kGeoRegionX * kGeoRegionCells) *
                                                kGeoCell,
                                            static_cast<double>(field->cellY_ + j - kGeoRegionY * kGeoRegionCells) *
                                                kGeoCell);
                }
            } else if (m.count == 1) {
                raw[index] = m.floors[0].z;
                nswe[index] = m.floors[0].nswe;
                single[index] = 1;
            } else {
                many.push_back(m);
            }
        }
    }
    // The ground round each cell of several floors: the nearest single-floor cell's (spreading out from them).
    if (!many.empty()) {
        std::vector<float> near = raw;
        std::deque<std::size_t> open;
        for (std::size_t index = 0; index < cells; ++index) {
            if (single[index] != 0) {
                open.push_back(index);
            }
        }
        std::vector<std::uint8_t> isMany(cells, 0);
        for (const Many& m : many) {
            isMany[m.index] = 1;
        }
        while (!open.empty()) {
            const std::size_t at = open.front();
            open.pop_front();
            const int i = static_cast<int>(at / static_cast<std::size_t>(side));
            const int j = static_cast<int>(at % static_cast<std::size_t>(side));
            const int next[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
            for (const auto& n : next) {
                if (n[0] < 0 || n[1] < 0 || n[0] >= side || n[1] >= side) {
                    continue;
                }
                const std::size_t to = static_cast<std::size_t>(n[0]) * static_cast<std::size_t>(side) +
                                       static_cast<std::size_t>(n[1]);
                if (isMany[to] != 0 && std::isnan(near[to])) {
                    near[to] = near[at];
                    open.push_back(to);
                }
            }
        }
        for (const Many& m : many) {
            const float ground = std::isnan(near[m.index]) ? static_cast<float>(z) : near[m.index];
            int best = 0;
            for (int k = 1; k < m.count; ++k) {
                if (std::fabs(m.floors[k].z - ground) < std::fabs(m.floors[best].z - ground)) {
                    best = k;
                }
            }
            raw[m.index] = m.floors[best].z;
            nswe[m.index] = m.floors[best].nswe;
        }
    }
    // What sticks up from the ground round it: a grey opening (the most of the least over kOpening x kOpening),
    // separable; cells without geodata count as very high, so they never cut what is next to them.
    constexpr float kHigh = 1e9f;
    std::vector<float> opened(cells);
    std::vector<float> pass(cells);
    for (std::size_t index = 0; index < cells; ++index) {
        opened[index] = std::isnan(raw[index]) ? kHigh : raw[index];
    }
    const int half = kOpening / 2;
    const auto filter = [&](std::vector<float>& from, std::vector<float>& to, bool alongI, bool least) {
        for (int i = 0; i < side; ++i) {
            for (int j = 0; j < side; ++j) {
                float best = least ? std::numeric_limits<float>::max() : std::numeric_limits<float>::lowest();
                for (int k = -half; k <= half; ++k) {
                    const int a = alongI ? std::clamp(i + k, 0, side - 1) : i;
                    const int b = alongI ? j : std::clamp(j + k, 0, side - 1);
                    const float v = from[static_cast<std::size_t>(a) * static_cast<std::size_t>(side) +
                                         static_cast<std::size_t>(b)];
                    best = least ? std::min(best, v) : std::max(best, v);
                }
                to[static_cast<std::size_t>(i) * static_cast<std::size_t>(side) + static_cast<std::size_t>(j)] = best;
            }
        }
    };
    filter(opened, pass, true, true);
    filter(pass, opened, false, true);
    filter(opened, pass, true, false);
    filter(pass, opened, false, false);
    std::vector<std::uint8_t> bump(cells, 0);
    for (std::size_t index = 0; index < cells; ++index) {
        bump[index] = !std::isnan(raw[index]) && raw[index] - opened[index] > kBump ? 1 : 0;
    }
    field->ground_ = raw;
    field->blocked_.assign(cells, 0);
    for (int i = 0; i < side; ++i) {
        for (int j = 0; j < side; ++j) {
            const std::size_t index = static_cast<std::size_t>(i) * static_cast<std::size_t>(side) +
                                      static_cast<std::size_t>(j);
            // Cut down: it and the cells next to it (the trunk's edge cells are often a little up too).
            bool cut = false;
            for (int a = std::max(i - 1, 0); a <= std::min(i + 1, side - 1) && !cut; ++a) {
                for (int b = std::max(j - 1, 0); b <= std::min(j + 1, side - 1) && !cut; ++b) {
                    cut = bump[static_cast<std::size_t>(a) * static_cast<std::size_t>(side) +
                               static_cast<std::size_t>(b)] != 0;
                }
            }
            if (cut && opened[index] < kHigh * 0.5f && !std::isnan(raw[index])) {
                field->ground_[index] = opened[index];
            }
            field->blocked_[index] = (cut || nswe[index] != 15) ? 1 : 0;
        }
    }
    return field;
}

bool GeoField::covers(double x, double y, double margin) const {
    const double cells = margin / kGeoCell;
    const double i = x / kGeoCell + kGeoRegionX * kGeoRegionCells - cellX_;
    const double j = y / kGeoCell + kGeoRegionY * kGeoRegionCells - cellY_;
    return i >= cells && j >= cells && i <= side_ - cells && j <= side_ - cells;
}

float GeoField::ground(double x, double y) const {
    // Cell middles: cell i's at i + 0.5.
    const double fi = x / kGeoCell + kGeoRegionX * kGeoRegionCells - cellX_ - 0.5;
    const double fj = y / kGeoCell + kGeoRegionY * kGeoRegionCells - cellY_ - 0.5;
    const int i = static_cast<int>(std::floor(fi));
    const int j = static_cast<int>(std::floor(fj));
    if (i < 0 || j < 0 || i + 1 >= side_ || j + 1 >= side_) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    const float a = static_cast<float>(fi - i);
    const float b = static_cast<float>(fj - j);
    const auto at = [&](int ci, int cj) {
        return ground_[static_cast<std::size_t>(ci) * static_cast<std::size_t>(side_) + static_cast<std::size_t>(cj)];
    };
    return (at(i, j) * (1.0f - a) + at(i + 1, j) * a) * (1.0f - b) + (at(i, j + 1) * (1.0f - a) + at(i + 1, j + 1) * a) * b;
}

bool GeoField::blocked(double x, double y) const {
    const int i = geoCellX(x) - cellX_;
    const int j = geoCellY(y) - cellY_;
    if (i < 0 || j < 0 || i >= side_ || j >= side_) {
        return false;
    }
    return blocked_[static_cast<std::size_t>(i) * static_cast<std::size_t>(side_) + static_cast<std::size_t>(j)] != 0;
}

bool worldOnScreen(const GroundCamera& camera, const double feet[3], double x, double y, double z, float& sx,
                   float& sy) {
    constexpr float kNearest = 0.3f; // Own heights ahead of the camera's eye: nearer is not drawn.
    const float east = static_cast<float>((x - feet[0]) / kUnitsPerOwnHeight);
    const float north = static_cast<float>(-(y - feet[1]) / kUnitsPerOwnHeight);
    const float up = static_cast<float>((z - feet[2]) / kUnitsPerOwnHeight);
    // The camera's yaw is clockwise from north (the minimap's cone): its right and away on the ground.
    const float c = std::cos(camera.yaw);
    const float s = std::sin(camera.yaw);
    const float right = c * east - s * north;
    const float away = s * east + c * north;
    if (aheadOfCamera(camera, right, away, up) < kNearest) {
        return false;
    }
    const GroundPoint point = worldToScreen(camera, right, away, up);
    if (!point.ahead) {
        return false;
    }
    sx = point.x;
    sy = point.y;
    return true;
}

GroundCamera geoCamera(const GeoField& field, const GroundCamera& camera, const double feet[3], float width,
                       float height) {
    if (!camera.known || camera.focal <= 0.0f || camera.pitchByTurns) { // The turns' pitch: no slope leans it.
        return camera;
    }
    constexpr float kFollowerBias = 1.95f * 0.017453292f;
    constexpr double kFar = 1500.0; // Units from the feet a ray goes on looking for the ground,
    constexpr double kStep = 12.0;  // in steps this long.
    const float scale = width / 2560.0f;
    const float sp = std::sin(camera.pitch);
    const float cp = std::cos(camera.pitch);
    const float sy = std::sin(camera.yaw);
    const float cy = std::cos(camera.yaw);
    // The eye: behind and above the point it circles (worldToScreen's camera), in own heights.
    const double eyeAway = -camera.distance * cp;
    const double eyeUp = kGroundTarget + camera.distance * sp;
    // Where the rays through a grid of the screen's lower middle meet the ground, and its plane through them (least
    // squares: z = a dx + b dy + c, about the feet).
    double sums[3][3] = {};
    double rhs[3] = {};
    int hits = 0;
    for (float py = 760.0f; py < 1400.0f; py += 60.0f) {
        for (float px = 500.0f; px < 2060.0f; px += 80.0f) {
            const float rx = (px * scale - camera.middleX) / camera.focal;
            const float dy = (py * (height / 1440.0f) - camera.middleY) / camera.focal;
            const double right = rx;
            const double away = cp - dy * sp;
            const double up = -sp - dy * cp;
            const double length = std::sqrt(right * right + away * away + up * up);
            const double flat = std::hypot(right, away) / length;
            if (flat <= 1e-6) {
                continue;
            }
            const double step = kStep / kUnitsPerOwnHeight / length / flat; // Ray length a kStep across the ground.
            for (double t = step; t * flat * length * kUnitsPerOwnHeight < kFar; t += step) {
                const double r = right * t;
                const double a = eyeAway + away * t;
                const double u = eyeUp + up * t;
                const double east = cy * r + sy * a;
                const double north = -sy * r + cy * a;
                const double x = feet[0] + east * kUnitsPerOwnHeight;
                const double y = feet[1] - north * kUnitsPerOwnHeight;
                const float ground = field.ground(x, y);
                if (std::isnan(ground)) {
                    break;
                }
                if (feet[2] + u * kUnitsPerOwnHeight <= ground) {
                    const double p[3] = {x - feet[0], y - feet[1], 1.0};
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) {
                            sums[i][j] += p[i] * p[j];
                        }
                        rhs[i] += p[i] * (static_cast<double>(ground) - feet[2]);
                    }
                    ++hits;
                    break;
                }
            }
        }
    }
    if (hits < 10) {
        return camera;
    }
    // Solve the 3 x 3 normal equations (Cramer's rule).
    const auto det = [](const double m[3][3]) {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    const double whole = det(sums);
    if (std::fabs(whole) < 1e-9) {
        return camera;
    }
    double solved[2] = {};
    for (int k = 0; k < 2; ++k) {
        double m[3][3];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                m[i][j] = j == k ? rhs[i] : sums[i][j];
            }
        }
        solved[k] = det(m) / whole;
    }
    // Down along the view (world y is south): how much the ground falls away from the camera.
    const double along = solved[0] * sy - solved[1] * cy;
    GroundCamera fixed = camera;
    fixed.pitch = camera.pitch + static_cast<float>(-std::atan(along)) - kFollowerBias;
    return fixed;
}

namespace {

// The parts of the line from (x0, y0) to (x1, y1) outside every piece of `clear`, as fractions of it (from, to) into
// `parts` (cleared first).
void outsideOf(float x0, float y0, float x1, float y1, std::span<const UiPiece> clear,
               std::vector<std::pair<float, float>>& parts) {
    parts.clear();
    std::pair<float, float> under[UiPieces::kMax];
    int count = 0;
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    for (const UiPiece& piece : clear) {
        // Liang-Barsky: where the line is inside the piece's rectangle.
        float from = 0.0f;
        float to = 1.0f;
        const float p[4] = {-dx, dx, -dy, dy};
        const float q[4] = {x0 - static_cast<float>(piece.x0), static_cast<float>(piece.x1) - x0,
                            y0 - static_cast<float>(piece.y0), static_cast<float>(piece.y1) - y0};
        bool inside = true;
        for (int k = 0; k < 4 && inside; ++k) {
            if (p[k] == 0.0f) {
                inside = q[k] >= 0.0f;
            } else if (const float t = q[k] / p[k]; p[k] < 0.0f) {
                from = std::max(from, t);
            } else {
                to = std::min(to, t);
            }
        }
        if (inside && from < to && count < UiPieces::kMax) {
            under[count++] = {from, to};
        }
    }
    std::sort(under, under + count);
    float at = 0.0f;
    for (int k = 0; k < count; ++k) {
        if (under[k].first > at) {
            parts.emplace_back(at, under[k].first);
        }
        at = std::max(at, under[k].second);
    }
    if (at < 1.0f) {
        parts.emplace_back(at, 1.0f);
    }
}

bool touches(const GeoGrid::Cell& cell, std::span<const UiPiece> clear) {
    const float minX = std::min({cell.x[0], cell.x[1], cell.x[2], cell.x[3]});
    const float maxX = std::max({cell.x[0], cell.x[1], cell.x[2], cell.x[3]});
    const float minY = std::min({cell.y[0], cell.y[1], cell.y[2], cell.y[3]});
    const float maxY = std::max({cell.y[0], cell.y[1], cell.y[2], cell.y[3]});
    return std::any_of(clear.begin(), clear.end(), [&](const UiPiece& piece) {
        return maxX > static_cast<float>(piece.x0) && minX < static_cast<float>(piece.x1) &&
               maxY > static_cast<float>(piece.y0) && minY < static_cast<float>(piece.y1);
    });
}

} // namespace

void geoGrid(const GeoField& field, const GroundCamera& camera, const double feet[3], float width, float height,
             std::span<const UiPiece> clear, GeoGrid& out) {
    out.lines.clear();
    out.blocked.clear();
    out.feet.clear();
    if (!camera.known) {
        return;
    }
    struct Detail {
        int step;
        int inner; // Lines whose middle is nearer the feet than this (each way) are the finer detail's.
        int reach;
        bool fine;
    };
    const Detail details[2] = {{GeoGridSize::kFine, 0, GeoGridSize::kFineReach, true},
                               {GeoGridSize::kCoarse, GeoGridSize::kFineReach, GeoGridSize::kCoarseReach, false}};
    struct Point {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool shown = false;
    };
    std::vector<Point> points;
    std::vector<std::pair<float, float>> parts;
    const auto offScreen = [&](float ax, float ay, float bx, float by) {
        return (ax < 0.0f && bx < 0.0f) || (ax > width && bx > width) || (ay < 0.0f && by < 0.0f) ||
               (ay > height && by > height);
    };
    for (const Detail& detail : details) {
        const double step = detail.step;
        const double firstX = std::floor((feet[0] - detail.reach) / step) * step;
        const double firstY = std::floor((feet[1] - detail.reach) / step) * step;
        const int count = 2 * detail.reach / detail.step + 3;
        points.assign(static_cast<std::size_t>(count) * static_cast<std::size_t>(count), Point{});
        const auto pointAt = [&](int i, int j) -> Point& {
            return points[static_cast<std::size_t>(i) * static_cast<std::size_t>(count) + static_cast<std::size_t>(j)];
        };
        for (int i = 0; i < count; ++i) {
            for (int j = 0; j < count; ++j) {
                Point& p = pointAt(i, j);
                const double x = firstX + i * step;
                const double y = firstY + j * step;
                p.z = field.ground(x, y);
                p.shown = !std::isnan(p.z) && worldOnScreen(camera, feet, x, y, p.z, p.x, p.y);
            }
        }
        const auto add = [&](const Point& a, const Point& b, double mx, double my) {
            if (!a.shown || !b.shown || offScreen(a.x, a.y, b.x, b.y)) {
                return;
            }
            const double ring = std::max(std::fabs(mx - feet[0]), std::fabs(my - feet[1]));
            if (ring > detail.reach || ring < detail.inner) {
                return;
            }
            float strength = 1.0f;
            if (!detail.fine) {
                const double far = std::hypot(mx - feet[0], my - feet[1]);
                const float fade = static_cast<float>(
                    std::clamp(1.0 - (far - detail.inner) / (detail.reach - detail.inner), 0.0, 1.0));
                strength = fade * fade;
                if (strength < 0.02f) {
                    return;
                }
            }
            const float rise = 0.5f * (a.z + b.z) - static_cast<float>(feet[2]);
            outsideOf(a.x, a.y, b.x, b.y, clear, parts);
            for (const auto& [from, to] : parts) {
                out.lines.push_back({a.x + (b.x - a.x) * from, a.y + (b.y - a.y) * from, a.x + (b.x - a.x) * to,
                                     a.y + (b.y - a.y) * to, rise, strength, detail.fine});
            }
        };
        for (int i = 0; i < count; ++i) {
            for (int j = 0; j < count; ++j) {
                const double x = firstX + i * step;
                const double y = firstY + j * step;
                if (i + 1 < count) {
                    add(pointAt(i, j), pointAt(i + 1, j), x + 0.5 * step, y);
                }
                if (j + 1 < count) {
                    add(pointAt(i, j), pointAt(i, j + 1), x, y + 0.5 * step);
                }
                // A fine cell something stands in or a character cannot walk through every way: filled.
                if (detail.fine && i + 1 < count && j + 1 < count) {
                    const Point* corners[4] = {&pointAt(i, j), &pointAt(i + 1, j), &pointAt(i + 1, j + 1),
                                               &pointAt(i, j + 1)};
                    if (!corners[0]->shown || !corners[1]->shown || !corners[2]->shown || !corners[3]->shown) {
                        continue;
                    }
                    const double ring =
                        std::max(std::fabs(x + 0.5 * step - feet[0]), std::fabs(y + 0.5 * step - feet[1]));
                    if (ring > detail.reach) {
                        continue;
                    }
                    bool blocked = false;
                    for (int sub = 0; sub < 4 && !blocked; ++sub) {
                        blocked = field.blocked(x + kGeoCell * (0.5 + sub % 2), y + kGeoCell * (0.5 + sub / 2));
                    }
                    if (blocked) {
                        GeoGrid::Cell cell;
                        for (int k = 0; k < 4; ++k) {
                            cell.x[k] = corners[k]->x;
                            cell.y[k] = corners[k]->y;
                        }
                        cell.strength = 1.0f;
                        if (!touches(cell, clear)) {
                            out.blocked.push_back(cell);
                        }
                    }
                }
            }
        }
    }
    // A ring round the feet, half a character's height across.
    constexpr int kRing = 32;
    constexpr double kRadius = 24.0;
    for (int k = 0; k < kRing; ++k) {
        const double angle = 6.283185307179586 * k / kRing;
        float sx = 0.0f;
        float sy = 0.0f;
        if (!worldOnScreen(camera, feet, feet[0] + kRadius * std::cos(angle), feet[1] + kRadius * std::sin(angle),
                           feet[2], sx, sy)) {
            out.feet.clear();
            break;
        }
        out.feet.push_back(sx);
        out.feet.push_back(sy);
    }
}

} // namespace observex
