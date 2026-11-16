#include "heuristics/DepthTerrain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace observex {

namespace {

constexpr int kCells = GroundHeights::kCells;
constexpr std::size_t kCellCount = static_cast<std::size_t>(kCells) * kCells;
constexpr float kCell = GroundHeights::kCell;

// The pyramid's levels for push-pull: kCells, half, ... 1 cells each way, one after the other in one array.
constexpr int kLevels = 8; // 128 .. 1.
static_assert(kCells == 1 << (kLevels - 1));
constexpr std::size_t levelStart(int level) {
    std::size_t start = 0;
    for (int i = 0; i < level; ++i) {
        const auto size = static_cast<std::size_t>(kCells >> i);
        start += size * size;
    }
    return start;
}
constexpr std::size_t kPyramid = levelStart(kLevels);

} // namespace

DepthTerrain::DepthTerrain()
    : heights_(std::make_unique<GroundHeights>()), level_(kCellCount, 0.0f), weight_(kCellCount, 0.0f),
      counts_(kCellCount + 1, 0), measured_(kCellCount, 0.0f), measuredWeight_(kCellCount, 0.0f),
      fillValue_(kPyramid, 0.0f), fillWeight_(kPyramid, 0.0f), sure_(kCellCount, 0.0f) {}

void DepthTerrain::clear() {
    const std::uint64_t version = version_;
    heights_ = std::make_unique<GroundHeights>(); // Not a temporary: some 140 kB, too much for a thread's stack.
    std::fill(level_.begin(), level_.end(), 0.0f);
    std::fill(weight_.begin(), weight_.end(), 0.0f);
    version_ = version + 1;
    heights_->version = version_;
}

void DepthTerrain::recentre(const GroundCamera& camera) {
    constexpr float kHalf = 0.5f * static_cast<float>(kCells - 1) * kCell;
    GroundHeights& out = *heights_;
    if (!out.known) {
        out.firstX = camera.feetX - kHalf;
        out.firstY = camera.feetY - kHalf;
        std::fill(level_.begin(), level_.end(), 0.0f);
        std::fill(weight_.begin(), weight_.end(), 0.0f);
        return;
    }
    const float offX = camera.feetX - (out.firstX + kHalf);
    const float offY = camera.feetY - (out.firstY + kHalf);
    if (std::hypot(offX, offY) <= kRecentre) {
        return;
    }
    const int shiftX = static_cast<int>(std::lround(offX / kCell));
    const int shiftY = static_cast<int>(std::lround(offY / kCell));
    // Cell (x, y) of the new square is (x + shiftX, y + shiftY) of the old one.
    std::vector<float>& level = measured_; // Free between maps: used as scratch here.
    std::vector<float>& weight = measuredWeight_;
    for (int y = 0; y < kCells; ++y) {
        for (int x = 0; x < kCells; ++x) {
            const int ox = x + shiftX;
            const int oy = y + shiftY;
            const auto to = static_cast<std::size_t>(y * kCells + x);
            if (ox < 0 || oy < 0 || ox >= kCells || oy >= kCells) {
                level[to] = 0.0f;
                weight[to] = 0.0f;
            } else {
                const auto from = static_cast<std::size_t>(oy * kCells + ox);
                level[to] = level_[from];
                weight[to] = weight_[from];
            }
        }
    }
    level_.swap(level);
    weight_.swap(weight);
    out.firstX += static_cast<float>(shiftX) * kCell;
    out.firstY += static_cast<float>(shiftY) * kCell;
}

bool DepthTerrain::take(const DepthInput& map, double timeMs, const GroundCamera& camera,
                        std::span<const GreyArea> avoid, GroundDepthNote& note) {
    if (map.map == nullptr || map.width < 2 * kStep || map.height < 2 * kStep || map.screenWidth <= 0 ||
        map.screenHeight <= 0 || !camera.known || !camera.pitchMeasured || camera.middleY <= 0.0f) {
        return false;
    }
    // The camera on the map's screen (the same as the follower's but for a test's).
    const float scale = static_cast<float>(map.screenHeight) / (2.0f * camera.middleY);
    const float focal = camera.focal * scale;
    const float middleX = camera.middleX * scale;
    const float middleY = camera.middleY * scale;
    const float s = std::sin(camera.pitch);
    const float c = std::cos(camera.pitch);
    const float eye = kGroundTarget + camera.distance * s; // The eye's height over the feet.
    if (eye < 0.2f) {
        return false; // Looking up from near the ground: no ground round the feet to fit on.
    }

    // What is left out, in samples.
    const int columns = map.width / kStep;
    const int rows = map.height / kStep;
    const float toSampleX = static_cast<float>(map.width) / static_cast<float>(map.screenWidth * kStep);
    const float toSampleY = static_cast<float>(map.height) / static_cast<float>(map.screenHeight * kStep);
    blocked_.assign(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows), std::uint8_t{0});
    const auto leaveOut = [&](const GreyArea& area) {
        const int x0 = std::max(static_cast<int>(std::floor(static_cast<float>(area.x0) * toSampleX)) - kMargin, 0);
        const int y0 = std::max(static_cast<int>(std::floor(static_cast<float>(area.y0) * toSampleY)) - kMargin, 0);
        const int x1 = std::min(static_cast<int>(std::ceil(static_cast<float>(area.x1) * toSampleX)) + kMargin, columns);
        const int y1 = std::min(static_cast<int>(std::ceil(static_cast<float>(area.y1) * toSampleY)) + kMargin, rows);
        for (int y = y0; y < y1; ++y) {
            std::fill_n(blocked_.begin() + static_cast<std::ptrdiff_t>(y * columns + x0), std::max(x1 - x0, 0),
                        std::uint8_t{1});
        }
    };
    for (int i = 0; map.ignore != nullptr && i < map.ignoreCount; ++i) {
        leaveOut(map.ignore[i]);
    }
    for (const GreyArea& area : avoid) {
        leaveOut(area);
    }

    // The samples, and which of them are on the flat ground near the feet.
    samples_.clear();
    near_.clear();
    const float pixelX = static_cast<float>(map.screenWidth) / static_cast<float>(map.width);
    const float pixelY = static_cast<float>(map.screenHeight) / static_cast<float>(map.height);
    for (int y = 0; y < rows; ++y) {
        const int mapY = y * kStep + kStep / 2;
        const float b = ((static_cast<float>(mapY) + 0.5f) * pixelY - middleY) / focal;
        const float flat = (b * c + s) / eye;
        const std::uint16_t* row = map.map + static_cast<std::size_t>(mapY) * static_cast<std::size_t>(map.width);
        for (int x = 0; x < columns; ++x) {
            if (blocked_[static_cast<std::size_t>(y * columns + x)] != 0) {
                continue;
            }
            const int mapX = x * kStep + kStep / 2;
            Sample sample;
            sample.column = x;
            sample.row = y;
            sample.network = halfToFloat(row[mapX]);
            const float left = halfToFloat(row[std::max(mapX - kStep, 0)]);
            const float rightOne = halfToFloat(row[std::min(mapX + kStep, map.width - 1)]);
            sample.edge = std::min(std::fabs(left - sample.network), std::fabs(rightOne - sample.network));
            sample.ax = ((static_cast<float>(mapX) + 0.5f) * pixelX - middleX) / focal;
            sample.b = b;
            sample.flat = flat;
            if (flat > 0.0f) {
                const float ahead = 1.0f / flat;
                const float right = ahead * sample.ax;
                const float away = -camera.distance * c + ahead * (c - b * s);
                const float from = std::hypot(right, away);
                if (from >= kNearFrom && from <= kNearReach) {
                    near_.push_back(static_cast<int>(samples_.size()));
                }
            }
            samples_.push_back(sample);
        }
    }
    if (static_cast<int>(near_.size()) < kLeastNear) {
        return false;
    }

    // network = scale x flat + offset on the near ground: least squares, then again without the outliers.
    float fitScale = 0.0f;
    float fitOffset = 0.0f;
    double spreadOfFlat = 0.0;
    double meanOfFlat = 0.0;
    int kept = 0;
    float miss = 0.0f;
    std::vector<float>& residuals = sorted_;
    for (int pass = 0; pass < 2; ++pass) {
        float limit = 1e30f;
        if (pass == 1) {
            residuals.clear();
            for (const int i : near_) {
                const Sample& sample = samples_[static_cast<std::size_t>(i)];
                residuals.push_back(std::fabs(sample.network - (fitScale * sample.flat + fitOffset)));
            }
            const auto middle = residuals.begin() + static_cast<std::ptrdiff_t>(residuals.size() / 2);
            std::nth_element(residuals.begin(), middle, residuals.end());
            limit = std::max(kOutliers * 1.4826f * *middle, 1e-6f);
        }
        double n = 0.0;
        double sx = 0.0;
        double sy = 0.0;
        double sxx = 0.0;
        double sxy = 0.0;
        for (const int i : near_) {
            const Sample& sample = samples_[static_cast<std::size_t>(i)];
            if (pass == 1 && std::fabs(sample.network - (fitScale * sample.flat + fitOffset)) > limit) {
                continue;
            }
            n += 1.0;
            sx += sample.flat;
            sy += sample.network;
            sxx += static_cast<double>(sample.flat) * sample.flat;
            sxy += static_cast<double>(sample.flat) * sample.network;
        }
        const double spread = sxx - sx * sx / std::max(n, 1.0);
        if (n < kLeastNear || spread <= 0.0) {
            return false;
        }
        const double slope = (sxy - sx * sy / n) / spread;
        fitScale = static_cast<float>(slope);
        fitOffset = static_cast<float>((sy - slope * sx) / n);
        meanOfFlat = sx / n;
        spreadOfFlat = std::sqrt(spread / n);
        kept = static_cast<int>(n);
    }
    double off = 0.0;
    for (const int i : near_) {
        const Sample& sample = samples_[static_cast<std::size_t>(i)];
        const double residual = sample.network - (fitScale * sample.flat + fitOffset);
        off += residual * residual;
    }
    miss = static_cast<float>(std::sqrt(off / static_cast<double>(near_.size())) /
                              std::max(static_cast<double>(fitScale) * meanOfFlat, 1e-9));
    note.shapeScale = fitScale;
    note.shapeOffset = fitOffset;
    note.shapeSamples = kept;
    note.shapeMiss = miss;
    if (fitScale <= 0.0f || meanOfFlat <= 0.0 || spreadOfFlat / meanOfFlat < kLeastSpread) {
        return false;
    }

    // The square round the feet, and its level under them now (the samples' heights are above the feet).
    recentre(camera);
    const float feetLevel = heights_->known ? heightOnMap(*heights_, camera.feetX, camera.feetY) : 0.0f;

    // Each sample where its depth puts it, walking up each column from the screen's bottom. On ground seen from above
    // it, what is higher up the screen is farther away and rises at most as a hillside does; so a sample is left out
    // that is nearer than the farthest ground taken below it (by kBack: a crown's or a roof's underside), or stands
    // above the last ground taken below it, or above the sample kBaselineRows-th of the rows lower, by more than a
    // hillside's rise over the way between them (kSteepest: a trunk, a wall, a crown's front; their pixels all stand
    // over the same few cells, whose heights would be theirs). Each against what is farther down, not only the sample
    // just below: from one row to the next a trunk rises and a crown comes nearer by less than the depth's noise.
    cellSamples_.clear();
    below_.assign(static_cast<std::size_t>(columns), Below{});
    points_.assign(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows), Point{});
    solid_.assign(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows), std::uint8_t{0});
    const int baseline = std::max(rows / kBaselineRows, 2);
    const float cosYaw = std::cos(camera.yaw);
    const float sinYaw = std::sin(camera.yaw);
    const float firstX = heights_->firstX - 0.5f * kCell;
    const float firstY = heights_->firstY - 0.5f * kCell;
    for (auto at = samples_.rbegin(); at != samples_.rend(); ++at) {
        const Sample& sample = *at;
        const float inverse = (sample.network - fitOffset) / fitScale;
        if (inverse <= 1.0f / (2.0f * kFarthest)) {
            continue; // The sky, or farther than anything taken.
        }
        const float ahead = 1.0f / inverse;
        const float right = ahead * sample.ax;
        const float away = -camera.distance * c + ahead * (c - sample.b * s);
        const float height = eye - ahead * (sample.b * c + s);
        const auto index = static_cast<std::size_t>(sample.row * columns + sample.column);
        points_[index] = {right, away, height, true};
        solid_[index] = 1; // Until taken.
        if (sample.edge > kEdge * fitScale * inverse) {
            continue; // Between a nearer and a farther thing, or a thin one: the map blurs them, so it floats.
        }
        const float rise = kRise * ahead + kRiseLeast; // The depth's noise grows with the distance.
        const auto steeper = [&](float overRight, float overAway, float overHeight) {
            return height > overHeight + kSteepest * std::hypot(right - overRight, away - overAway) + rise;
        };
        const float reach = std::hypot(right, away + camera.distance * c); // From the eye, along the ground.
        Below& below = below_[static_cast<std::size_t>(sample.column)];
        if (below.known && (reach < below.farthest * (1.0f - kBack) || steeper(below.right, below.away, below.height))) {
            continue;
        }
        if (sample.row + baseline < rows) {
            const Point& base = points_[static_cast<std::size_t>((sample.row + baseline) * columns + sample.column)];
            if (base.known && steeper(base.right, base.away, base.height)) {
                continue;
            }
        }
        below = {right, away, height, std::max(below.farthest, reach), true};
        solid_[index] = 0;
        if (right * right + away * away > kFarthest * kFarthest) {
            continue;
        }
        const float mapX = camera.feetX + cosYaw * right + sinYaw * away;
        const float mapY = camera.feetY - sinYaw * right + cosYaw * away;
        const int cellX = static_cast<int>(std::floor((mapX - firstX) / kCell));
        const int cellY = static_cast<int>(std::floor((mapY - firstY) / kCell));
        if (cellX < 0 || cellY < 0 || cellX >= kCells || cellY >= kCells) {
            continue;
        }
        const float sure = kSureAhead / std::max(ahead, kSureAhead);
        cellSamples_.push_back({cellY * kCells + cellX, height, sure * sure});
    }

    keepFront(map, camera, scale, fitScale, fitOffset, feetLevel, columns, rows);

    // Each cell's low share of its heights (counting sort by cell).
    std::fill(counts_.begin(), counts_.end(), 0);
    for (const CellSample& sample : cellSamples_) {
        ++counts_[static_cast<std::size_t>(sample.cell) + 1];
    }
    for (std::size_t i = 1; i < counts_.size(); ++i) {
        counts_[i] += counts_[i - 1];
    }
    sorted_.resize(cellSamples_.size());
    sortedWeights_.resize(cellSamples_.size());
    {
        std::vector<int>& next = near_; // Done with: the place each cell's next sample goes.
        next.assign(counts_.begin(), counts_.end() - 1);
        for (const CellSample& sample : cellSamples_) {
            const auto at = static_cast<std::size_t>(next[static_cast<std::size_t>(sample.cell)]++);
            sorted_[at] = sample.height;
            sortedWeights_[at] = sample.weight;
        }
    }
    for (std::size_t cell = 0; cell < kCellCount; ++cell) {
        const int from = counts_[cell];
        const int to = counts_[cell + 1];
        measuredWeight_[cell] = 0.0f;
        if (to - from < 2) {
            continue;
        }
        float weight = 0.0f;
        for (int i = from; i < to; ++i) {
            weight += sortedWeights_[static_cast<std::size_t>(i)];
        }
        const auto first = sorted_.begin() + from;
        const auto low = first + static_cast<std::ptrdiff_t>(kLowShare * static_cast<float>(to - from - 1));
        std::nth_element(first, low, sorted_.begin() + to);
        measured_[cell] = *low;
        measuredWeight_[cell] = std::min(weight / kFullSamples, 1.0f);
    }

    // Left out where it stands off the cells round it; the rest into the square.
    int said = 0;
    std::array<float, 24> round{};
    for (int y = 0; y < kCells; ++y) {
        for (int x = 0; x < kCells; ++x) {
            const auto cell = static_cast<std::size_t>(y * kCells + x);
            const float weight = measuredWeight_[cell];
            if (weight <= 0.0f) {
                continue;
            }
            int count = 0;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= kCells || ny >= kCells) {
                        continue;
                    }
                    const auto other = static_cast<std::size_t>(ny * kCells + nx);
                    if (measuredWeight_[other] > 0.0f) {
                        round[static_cast<std::size_t>(count++)] = measured_[other];
                    }
                }
            }
            const float height = measured_[cell];
            if (count >= 5) {
                const auto middle = round.begin() + count / 2;
                std::nth_element(round.begin(), middle, round.begin() + count);
                if (height > *middle + kJump || height < *middle - 2.0f * kJump) {
                    continue;
                }
            }
            const float level = height + feetLevel;
            const float before = weight_[cell];
            level_[cell] = (level_[cell] * before + level * weight) / (before + weight);
            weight_[cell] = std::min(before + weight, kMostWeight);
            ++said;
        }
    }
    note.shapeCells = said;
    if (said == 0) {
        return false;
    }
    hand(feetLevel, timeMs);
    note.shaped = true;
    return true;
}

void DepthTerrain::keepFront(const DepthInput& map, const GroundCamera& camera, float screenScale, float fitScale,
                             float fitOffset, float feetLevel, int sampleColumns, int sampleRows) {
    GroundHeights& out = *heights_;
    const int step = std::max({kStep, (map.width + GroundHeights::kFrontColumns - 1) / GroundHeights::kFrontColumns,
                               (map.height + GroundHeights::kFrontRows - 1) / GroundHeights::kFrontRows});
    const int columns = map.width / step;
    const int rows = map.height / step;
    out.frontCamera = camera;
    out.frontFeetLevel = feetLevel;
    out.frontPerPixelX = screenScale * static_cast<float>(map.width) / static_cast<float>(map.screenWidth * step);
    out.frontPerPixelY = screenScale * static_cast<float>(map.height) / static_cast<float>(map.screenHeight * step);
    out.frontColumns = columns;
    out.frontRows = rows;
    for (int y = 0; y < rows; ++y) {
        const int mapY = y * step + step / 2;
        const std::uint16_t* row = map.map + static_cast<std::size_t>(mapY) * static_cast<std::size_t>(map.width);
        const std::uint8_t* solid =
            solid_.data() + static_cast<std::ptrdiff_t>(std::min(mapY / kStep, sampleRows - 1) * sampleColumns);
        std::uint8_t* codes = out.front.data() + static_cast<std::ptrdiff_t>(y * columns);
        for (int x = 0; x < columns; ++x) {
            const int mapX = x * step + step / 2;
            const float lifted = halfToFloat(row[mapX]) - fitOffset; // Scale x inverse depth.
            if (!(lifted > 0.0f)) {
                codes[x] = 0;
                continue;
            }
            const float seen = fitScale / lifted;
            codes[x] = frontCode(solid[std::min(mapX / kStep, sampleColumns - 1)] != 0
                                     ? seen * (1.0f + kSolidShare) + kSolidLeast
                                     : (seen + kFrontLeast) / (1.0f - kFrontShare));
        }
    }
    // The map's own interface says nothing (the characters do: the grid goes behind them too).
    const float toFrontX = static_cast<float>(map.width) / static_cast<float>(map.screenWidth * step);
    const float toFrontY = static_cast<float>(map.height) / static_cast<float>(map.screenHeight * step);
    for (int i = 0; map.ignore != nullptr && i < map.ignoreCount; ++i) {
        const GreyArea& area = map.ignore[i];
        const int x0 = std::max(static_cast<int>(std::floor(static_cast<float>(area.x0) * toFrontX)), 0);
        const int y0 = std::max(static_cast<int>(std::floor(static_cast<float>(area.y0) * toFrontY)), 0);
        const int x1 = std::min(static_cast<int>(std::ceil(static_cast<float>(area.x1) * toFrontX)), columns);
        const int y1 = std::min(static_cast<int>(std::ceil(static_cast<float>(area.y1) * toFrontY)), rows);
        for (int y = y0; y < y1 && x1 > x0; ++y) {
            std::fill_n(out.front.begin() + static_cast<std::ptrdiff_t>(y * columns + x0), x1 - x0, std::uint8_t{0});
        }
    }
}

void DepthTerrain::hand(float flat, double timeMs) {
    // Push-pull: the cells seen summed into ever coarser levels, then each level's empty cells filled from the
    // coarser one between its cells (bilinear), part filled where little was seen.
    for (std::size_t cell = 0; cell < kCellCount; ++cell) {
        const float weight = weight_[cell] > 0.0f ? 1.0f : 0.0f; // Seen at all: its own (a weak one is a far one).
        fillWeight_[cell] = weight;
        fillValue_[cell] = weight * level_[cell];
    }
    for (int level = 1; level < kLevels; ++level) {
        const int size = kCells >> level;
        const int finer = size * 2;
        float* value = fillValue_.data() + levelStart(level);
        float* weight = fillWeight_.data() + levelStart(level);
        const float* fineValue = fillValue_.data() + levelStart(level - 1);
        const float* fineWeight = fillWeight_.data() + levelStart(level - 1);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float v = 0.0f;
                float w = 0.0f;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const int at = (2 * y + dy) * finer + 2 * x + dx;
                        v += fineValue[at];
                        w += fineWeight[at];
                    }
                }
                value[y * size + x] = v;
                weight[y * size + x] = w;
            }
        }
    }
    // Down again: at each level, the value (no longer weighted) where it is known enough, the coarser one where not.
    {
        float* value = fillValue_.data() + levelStart(kLevels - 1);
        const float weight = fillWeight_[levelStart(kLevels - 1)];
        value[0] = weight > 0.0f ? value[0] / weight : flat;
    }
    for (int level = kLevels - 2; level >= 0; --level) {
        const int size = kCells >> level;
        const int coarse = size / 2;
        float* value = fillValue_.data() + levelStart(level);
        const float* weight = fillWeight_.data() + levelStart(level);
        const float* up = fillValue_.data() + levelStart(level + 1);
        for (int y = 0; y < size; ++y) {
            const float cy = std::clamp((static_cast<float>(y) + 0.5f) * 0.5f - 0.5f, 0.0f,
                                        static_cast<float>(coarse - 1));
            const int y0 = std::min(static_cast<int>(cy), std::max(coarse - 2, 0));
            const int y1 = std::min(y0 + 1, coarse - 1);
            const float fy = cy - static_cast<float>(y0);
            for (int x = 0; x < size; ++x) {
                const float cx = std::clamp((static_cast<float>(x) + 0.5f) * 0.5f - 0.5f, 0.0f,
                                            static_cast<float>(coarse - 1));
                const int x0 = std::min(static_cast<int>(cx), std::max(coarse - 2, 0));
                const int x1 = std::min(x0 + 1, coarse - 1);
                const float fx = cx - static_cast<float>(x0);
                const float top = up[y0 * coarse + x0] + fx * (up[y0 * coarse + x1] - up[y0 * coarse + x0]);
                const float bottom = up[y1 * coarse + x0] + fx * (up[y1 * coarse + x1] - up[y1 * coarse + x0]);
                const float coarser = top + fy * (bottom - top);
                const int at = y * size + x;
                const float w = weight[at];
                const float own = w > 0.0f ? value[at] / w : coarser;
                const float share = std::min(w, 1.0f);
                value[at] = share * own + (1.0f - share) * coarser;
            }
        }
    }
    // How near each cell is to what was seen: 1 on a cell seen (a far one's samples say less, but blended towards the
    // flat ground it stood lower than it is: a 2-high hill 21 away came out 1.1), else a neighbour's fading with each
    // cell out (two sweeps, eight neighbours).
    const float diagonal = std::pow(kFillFade, 1.41421356f);
    for (std::size_t cell = 0; cell < kCellCount; ++cell) {
        sure_[cell] = weight_[cell] > 0.0f ? 1.0f : 0.0f;
    }
    const auto carry = [&](int x, int y, int dx, int dy, float fade) {
        const int nx = x + dx;
        const int ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= kCells || ny >= kCells) {
            return;
        }
        float& here = sure_[static_cast<std::size_t>(y * kCells + x)];
        here = std::max(here, sure_[static_cast<std::size_t>(ny * kCells + nx)] * fade);
    };
    for (int y = 0; y < kCells; ++y) {
        for (int x = 0; x < kCells; ++x) {
            carry(x, y, -1, 0, kFillFade);
            carry(x, y, -1, -1, diagonal);
            carry(x, y, 0, -1, kFillFade);
            carry(x, y, 1, -1, diagonal);
        }
    }
    for (int y = kCells; y-- > 0;) {
        for (int x = kCells; x-- > 0;) {
            carry(x, y, 1, 0, kFillFade);
            carry(x, y, 1, 1, diagonal);
            carry(x, y, 0, 1, kFillFade);
            carry(x, y, -1, 1, diagonal);
        }
    }
    GroundHeights& out = *heights_;
    for (std::size_t cell = 0; cell < kCellCount; ++cell) {
        out.height[cell] = flat + (fillValue_[cell] - flat) * sure_[cell];
        out.seen[cell] = static_cast<std::uint8_t>(std::lround(255.0f * std::min(weight_[cell] / kMostWeight, 1.0f)));
    }
    out.known = true;
    out.timeMs = timeMs;
    out.version = ++version_;
}

} // namespace observex
