#include "heuristics/DepthTilt.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace observex {

namespace {

// The measured table (degrees): the zero row's pitch as the network sees it, and the pitch that is (each the
// median of the grid's pitch over the maps seeing about that, smoothed into one rising line; header).
struct TablePoint {
    float seen = 0.0f;
    float pitch = 0.0f;
};
constexpr std::array<TablePoint, 16> kTable{{
    {0.0f, 0.0f},
    {5.0f, 4.5f},
    {10.0f, 10.5f},
    {14.0f, 14.5f},
    {16.0f, 17.5f},
    {18.0f, 20.5f},
    {20.0f, 22.4f},
    {22.0f, 25.4f},
    {24.0f, 33.0f},
    {26.0f, 43.5f},
    {28.0f, 50.3f},
    {30.0f, 55.7f},
    {32.0f, 61.0f},
    {34.0f, 67.0f},
    {36.0f, 75.0f},
    {38.0f, 81.5f},
}};

constexpr float kDegrees = 57.2957795f;

} // namespace

DepthLine measureDepthLine(const DepthInput& map, std::span<const GreyArea> avoid) {
    DepthLine line;
    if (map.map == nullptr || map.width <= 0 || map.height <= 0 || map.screenWidth <= 0 || map.screenHeight <= 0) {
        return line;
    }
    const float sx = static_cast<float>(map.width) / static_cast<float>(map.screenWidth);
    const float sy = static_cast<float>(map.height) / static_cast<float>(map.screenHeight);
    const int top = static_cast<int>(kDepthBandTop * static_cast<float>(map.height));
    const int bottom = std::min(static_cast<int>(kDepthBandBottom * static_cast<float>(map.height)), map.height);
    const int left = static_cast<int>(kDepthBandLeft * static_cast<float>(map.width));
    const int right = std::min(static_cast<int>(kDepthBandRight * static_cast<float>(map.width)), map.width);
    if (bottom <= top || right <= left) {
        return line;
    }
    // What is left out, in the map's pixels.
    std::vector<GreyArea> out;
    out.reserve(static_cast<std::size_t>(std::max(map.ignoreCount, 0)) + avoid.size());
    const auto leaveOut = [&](const GreyArea& area) {
        out.push_back({static_cast<int>(std::floor(static_cast<float>(area.x0) * sx)) - kDepthMargin,
                       static_cast<int>(std::floor(static_cast<float>(area.y0) * sy)) - kDepthMargin,
                       static_cast<int>(std::ceil(static_cast<float>(area.x1) * sx)) + kDepthMargin,
                       static_cast<int>(std::ceil(static_cast<float>(area.y1) * sy)) + kDepthMargin});
    };
    for (int i = 0; map.ignore != nullptr && i < map.ignoreCount; ++i) {
        leaveOut(map.ignore[i]);
    }
    for (const GreyArea& area : avoid) {
        leaveOut(area);
    }

    const int columns = right - left;
    std::vector<std::uint8_t> blocked(static_cast<std::size_t>(columns));
    std::vector<float> values;
    values.reserve(static_cast<std::size_t>(columns));
    std::vector<double> rows;
    std::vector<double> medians;
    for (int y = top; y < bottom; ++y) {
        std::fill(blocked.begin(), blocked.end(), std::uint8_t{0});
        for (const GreyArea& area : out) {
            if (y < area.y0 || y >= area.y1) {
                continue;
            }
            for (int x = std::max(area.x0, left); x < std::min(area.x1, right); ++x) {
                blocked[static_cast<std::size_t>(x - left)] = 1;
            }
        }
        values.clear();
        const std::uint16_t* row = map.map + static_cast<std::size_t>(y) * static_cast<std::size_t>(map.width);
        for (int x = left; x < right; ++x) {
            if (blocked[static_cast<std::size_t>(x - left)] == 0) {
                values.push_back(halfToFloat(row[x]));
            }
        }
        if (values.empty() || static_cast<float>(values.size()) < kDepthRowShare * static_cast<float>(columns)) {
            continue;
        }
        // The median (of an even count, the two middle ones' mean).
        const std::size_t half = values.size() / 2;
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(half), values.end());
        double median = values[half];
        if (values.size() % 2 == 0) {
            median = 0.5 * (median + *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(half)));
        }
        rows.push_back((static_cast<double>(y) + 0.5) / static_cast<double>(sy));
        medians.push_back(median);
    }
    line.rows = static_cast<int>(rows.size());
    if (rows.size() < 2 || static_cast<float>(rows.size()) < kDepthRowsShare * static_cast<float>(bottom - top)) {
        return line;
    }
    // The straight line through them (least squares), nearer (larger) down the screen.
    const auto count = static_cast<double>(rows.size());
    double meanRow = 0.0;
    double meanValue = 0.0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        meanRow += rows[i];
        meanValue += medians[i];
    }
    meanRow /= count;
    meanValue /= count;
    double across = 0.0;
    double spread = 0.0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        across += (rows[i] - meanRow) * (medians[i] - meanValue);
        spread += (rows[i] - meanRow) * (rows[i] - meanRow);
    }
    if (spread <= 0.0 || across <= 0.0) {
        return line;
    }
    const double slope = across / spread;
    const double offset = meanValue - slope * meanRow;
    double off = 0.0;
    double lowest = medians[0];
    double highest = medians[0];
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const double miss = medians[i] - (slope * rows[i] + offset);
        off += miss * miss;
        lowest = std::min(lowest, medians[i]);
        highest = std::max(highest, medians[i]);
    }
    if (highest <= lowest) {
        return line;
    }
    line.known = true;
    line.zeroRow = static_cast<float>(-offset / slope);
    line.straight = static_cast<float>(std::sqrt(off / count) / (highest - lowest));
    return line;
}

DepthPitch depthPitch(float seen) {
    const float degrees = seen * kDegrees;
    DepthPitch out;
    if (degrees <= kTable.front().seen) {
        out.pitch = seen; // Looking level or up: the band reaches past the horizon, and the line is not straight.
        out.weight = 1.0f;
        return out;
    }
    for (std::size_t i = 1; i < kTable.size(); ++i) {
        const TablePoint& from = kTable[i - 1];
        const TablePoint& to = kTable[i];
        if (degrees <= to.seen || i + 1 == kTable.size()) {
            const float slope = (to.pitch - from.pitch) / (to.seen - from.seen);
            // Past the table's end: as far as it went (the network sees no steeper).
            const float at = std::min(degrees, to.seen);
            out.pitch = (from.pitch + slope * (at - from.seen)) / kDegrees;
            out.weight = 1.0f / std::max(1.0f, slope * slope);
            return out;
        }
    }
    return out;
}

} // namespace observex
