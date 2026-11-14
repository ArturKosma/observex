#include "heuristics/DepthRules.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace observex {

namespace {

// matplotlib's inferno at every tenth, red, green, blue (its colours: black, purple, red, orange, pale yellow).
constexpr std::array<std::array<float, 3>, 11> kInferno{{{0, 0, 4},
                                                         {22, 11, 57},
                                                         {66, 10, 104},
                                                         {106, 23, 110},
                                                         {147, 38, 103},
                                                         {188, 55, 84},
                                                         {221, 81, 58},
                                                         {243, 118, 27},
                                                         {252, 165, 10},
                                                         {246, 215, 70},
                                                         {252, 255, 164}}};

std::size_t at(int x, int y, int width) {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
}

// tanh, within 2% (a rational fit, then cut at 1): the local term's soft limit, without 478 000 calls to std::tanh.
float softLimit(float x) {
    const float y = x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
    return std::clamp(y, -1.0f, 1.0f);
}

// `values` (width x height) through a box `radius` pixels each way, into `out`, its edges repeated outwards: along the
// rows into `rows`, then down the columns. `sums` is room for a row of column sums.
void boxBlur(const std::vector<float>& values, int width, int height, int radius, std::vector<float>& rows,
             std::vector<float>& out, std::vector<float>& sums) {
    const float scale = 1.0f / static_cast<float>(2 * radius + 1);
    rows.resize(values.size());
    out.resize(values.size());
    for (int y = 0; y < height; ++y) {
        const float* in = values.data() + at(0, y, width);
        float* to = rows.data() + at(0, y, width);
        float sum = 0.0f;
        for (int k = -radius; k <= radius; ++k) {
            sum += in[std::clamp(k, 0, width - 1)];
        }
        for (int x = 0; x < width; ++x) {
            to[x] = sum * scale;
            sum += in[std::min(x + radius + 1, width - 1)] - in[std::max(x - radius, 0)];
        }
    }
    // Down the columns, all at once (row by row through the memory).
    sums.assign(static_cast<std::size_t>(width), 0.0f);
    for (int k = -radius; k <= radius; ++k) {
        const float* in = rows.data() + at(0, std::clamp(k, 0, height - 1), width);
        for (int x = 0; x < width; ++x) {
            sums[static_cast<std::size_t>(x)] += in[x];
        }
    }
    for (int y = 0; y < height; ++y) {
        float* to = out.data() + at(0, y, width);
        const float* add = rows.data() + at(0, std::min(y + radius + 1, height - 1), width);
        const float* take = rows.data() + at(0, std::max(y - radius, 0), width);
        for (int x = 0; x < width; ++x) {
            to[x] = sums[static_cast<std::size_t>(x)] * scale;
            sums[static_cast<std::size_t>(x)] += add[x] - take[x];
        }
    }
}

} // namespace

void DepthRules::inferno(std::uint32_t* palette) {
    for (int i = 0; i < kPaletteSize; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kPaletteSize - 1) * 10.0f;
        const int low = std::min(static_cast<int>(t), 9);
        const float share = t - static_cast<float>(low);
        std::uint32_t colour = 0xFF000000u;
        for (int c = 0; c < 3; ++c) {
            const float value = kInferno[static_cast<std::size_t>(low)][static_cast<std::size_t>(c)] * (1.0f - share) +
                                kInferno[static_cast<std::size_t>(low + 1)][static_cast<std::size_t>(c)] * share;
            const auto byte = static_cast<std::uint32_t>(std::clamp(std::lround(value), 0L, 255L));
            colour |= byte << (16 - 8 * c); // Red in bits 16-23, green 8-15, blue 0-7.
        }
        palette[i] = colour;
    }
}

bool DepthRules::shade(const DepthInput& input, std::uint8_t* levels, std::uint32_t* palette) {
    const int width = input.width;
    const int height = input.height;
    if (input.map == nullptr || width <= 0 || height <= 0 || input.screenWidth <= 0 || input.screenHeight <= 0) {
        return false;
    }
    inferno(palette);
    if (halves_.empty()) { // Every half float's value, once.
        halves_.resize(1 << 16);
        for (std::size_t i = 0; i < halves_.size(); ++i) {
            const float value = halfToFloat(static_cast<std::uint16_t>(i));
            halves_[i] = std::isfinite(value) ? value : 0.0f;
        }
    }

    // The map, and which of its pixels are the world's (not the interface's). Room for every halving at once (a side
    // halves to 1 in at most 31 steps), so `top` and the others stay where they are.
    halvings_.reserve(32);
    if (halvings_.empty()) {
        halvings_.emplace_back();
    }
    Level& top = halvings_[0];
    top.width = width;
    top.height = height;
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    top.values.resize(pixels);
    top.known.assign(pixels, 1);
    for (std::size_t i = 0; i < pixels; ++i) {
        top.values[i] = halves_[input.map[i]];
    }
    const float toMapX = static_cast<float>(width) / static_cast<float>(input.screenWidth);
    const float toMapY = static_cast<float>(height) / static_cast<float>(input.screenHeight);
    bool holes = false;
    for (int i = 0; i < input.ignoreCount; ++i) {
        const GreyArea& area = input.ignore[i];
        const int x0 = std::max(static_cast<int>(std::floor(static_cast<float>(area.x0) * toMapX)) - kMargin, 0);
        const int y0 = std::max(static_cast<int>(std::floor(static_cast<float>(area.y0) * toMapY)) - kMargin, 0);
        const int x1 = std::min(static_cast<int>(std::ceil(static_cast<float>(area.x1) * toMapX)) + kMargin, width);
        const int y1 = std::min(static_cast<int>(std::ceil(static_cast<float>(area.y1) * toMapY)) + kMargin, height);
        for (int y = y0; y < y1 && x1 > x0; ++y) {
            std::fill_n(top.known.begin() + static_cast<std::ptrdiff_t>(at(x0, y, width)), x1 - x0, std::uint8_t{0});
            holes = true;
        }
    }

    // The world's range (the interface's pixels left out).
    float low = 0.0f;
    float high = 0.0f;
    bool any = false;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (top.known[i] != 0) {
            const float value = top.values[i];
            low = any ? std::min(low, value) : value;
            high = any ? std::max(high, value) : value;
            any = true;
        }
    }
    if (!any) { // All interface: nothing of the world to show.
        std::fill_n(levels, pixels, std::uint8_t{0});
        return true;
    }

    // 1. The interface's holes filled from the world round them (push-pull).
    if (holes) {
        // The trend down the screen out first, and back in after: the fill below holds what it fills flat at the
        // screen's edge, so the ground's slope, taken out, goes on under a piece reaching the bottom. Without it a
        // hotbar's fill came out ever farther than the ground beside it, a ridge up to 24 levels along its edge; a
        // slope rule at the screen's edge instead had nothing to hold it under the game's bar across the whole bottom,
        // and ran off (2026-11-12). Each row's mean over the world's pixels there; rows with fewer than 1 / kTrendShare
        // of them straight across between the rows beside them, or past the first or the last along the line through
        // the kTrendRows nearest; then smoothed over kTrendRadius rows each way (fewer near the ends, so a straight
        // trend stays straight).
        rowMeans_.assign(static_cast<std::size_t>(height), 0.0f);
        rowKnown_.assign(static_cast<std::size_t>(height), 0);
        trend_.assign(static_cast<std::size_t>(height), 0.0f);
        const int enough = std::max(width / kTrendShare, 1);
        bool anyRow = false;
        for (int y = 0; y < height; ++y) {
            float sum = 0.0f;
            int count = 0;
            for (std::size_t i = at(0, y, width), end = i + static_cast<std::size_t>(width); i < end; ++i) {
                if (top.known[i] != 0) {
                    sum += top.values[i];
                    ++count;
                }
            }
            if (count >= enough) {
                rowMeans_[static_cast<std::size_t>(y)] = sum / static_cast<float>(count);
                rowKnown_[static_cast<std::size_t>(y)] = 1;
                anyRow = true;
            }
        }
        // The line through up to kTrendRows known rows from `from` on (by `step`), at row `y`.
        const auto line = [&](int from, int step, int y) {
            double n = 0.0;
            double sumY = 0.0;
            double sumMean = 0.0;
            double sumYY = 0.0;
            double sumYMean = 0.0;
            for (int r = from, found = 0; r >= 0 && r < height && found < kTrendRows; r += step) {
                if (rowKnown_[static_cast<std::size_t>(r)] != 0) {
                    const double mean = rowMeans_[static_cast<std::size_t>(r)];
                    n += 1.0;
                    sumY += r;
                    sumMean += mean;
                    sumYY += static_cast<double>(r) * r;
                    sumYMean += r * mean;
                    ++found;
                }
            }
            const double spread = sumYY - sumY * sumY / n;
            const double slope = spread > 1e-9 ? (sumYMean - sumY * sumMean / n) / spread : 0.0;
            return static_cast<float>(sumMean / n + slope * (y - sumY / n));
        };
        for (int y = 0; anyRow && y < height;) {
            if (rowKnown_[static_cast<std::size_t>(y)] != 0) {
                ++y;
                continue;
            }
            int end = y; // Rows y to end - 1 have too few.
            while (end < height && rowKnown_[static_cast<std::size_t>(end)] == 0) {
                ++end;
            }
            for (int r = y; r < end; ++r) {
                float& mean = rowMeans_[static_cast<std::size_t>(r)];
                if (y > 0 && end < height) {
                    const float share = static_cast<float>(r - y + 1) / static_cast<float>(end - y + 1);
                    mean = rowMeans_[static_cast<std::size_t>(y - 1)] * (1.0f - share) +
                           rowMeans_[static_cast<std::size_t>(end)] * share;
                } else {
                    mean = y == 0 ? line(end, 1, r) : line(y - 1, -1, r);
                }
            }
            y = end;
        }
        for (int y = 0; anyRow && y < height; ++y) {
            const int reach = std::min({kTrendRadius, y, height - 1 - y});
            float sum = 0.0f;
            for (int r = y - reach; r <= y + reach; ++r) {
                sum += rowMeans_[static_cast<std::size_t>(r)];
            }
            const float trend = sum / static_cast<float>(2 * reach + 1);
            trend_[static_cast<std::size_t>(y)] = trend;
            for (std::size_t i = at(0, y, width), end = i + static_cast<std::size_t>(width); i < end; ++i) {
                top.values[i] -= trend;
            }
        }

        int count = 1;
        for (;;) {
            const Level& fine = halvings_[static_cast<std::size_t>(count - 1)];
            const bool hole = std::find(fine.known.begin(), fine.known.end(), std::uint8_t{0}) != fine.known.end();
            if (!hole || (fine.width == 1 && fine.height == 1)) {
                break;
            }
            if (static_cast<int>(halvings_.size()) == count) {
                halvings_.emplace_back();
            }
            Level& coarse = halvings_[static_cast<std::size_t>(count)];
            coarse.width = (fine.width + 1) / 2;
            coarse.height = (fine.height + 1) / 2;
            coarse.values.assign(static_cast<std::size_t>(coarse.width) * static_cast<std::size_t>(coarse.height), 0.0f);
            coarse.known.assign(coarse.values.size(), 0);
            // Pull: each pixel the mean of the known of the 2 x 2 under it.
            for (int y = 0; y < coarse.height; ++y) {
                for (int x = 0; x < coarse.width; ++x) {
                    float sum = 0.0f;
                    int known = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const int fx = 2 * x + dx;
                            const int fy = 2 * y + dy;
                            if (fx < fine.width && fy < fine.height && fine.known[at(fx, fy, fine.width)] != 0) {
                                sum += fine.values[at(fx, fy, fine.width)];
                                ++known;
                            }
                        }
                    }
                    if (known > 0) {
                        coarse.values[at(x, y, coarse.width)] = sum / static_cast<float>(known);
                        coarse.known[at(x, y, coarse.width)] = 1;
                    }
                }
            }
            ++count;
        }
        // Push: from the smallest up, each hole bilinearly from the halving above it (which has none left by then).
        for (int level = count - 2; level >= 0; --level) {
            Level& fine = halvings_[static_cast<std::size_t>(level)];
            const Level& coarse = halvings_[static_cast<std::size_t>(level + 1)];
            for (int y = 0; y < fine.height; ++y) {
                const float cy = std::clamp((static_cast<float>(y) + 0.5f) * 0.5f - 0.5f, 0.0f,
                                            static_cast<float>(coarse.height - 1));
                const int y0 = static_cast<int>(cy);
                const int y1 = std::min(y0 + 1, coarse.height - 1);
                const float ty = cy - static_cast<float>(y0);
                for (int x = 0; x < fine.width; ++x) {
                    const std::size_t i = at(x, y, fine.width);
                    if (fine.known[i] != 0) {
                        continue;
                    }
                    const float cx = std::clamp((static_cast<float>(x) + 0.5f) * 0.5f - 0.5f, 0.0f,
                                                static_cast<float>(coarse.width - 1));
                    const int x0 = static_cast<int>(cx);
                    const int x1 = std::min(x0 + 1, coarse.width - 1);
                    const float tx = cx - static_cast<float>(x0);
                    const float upper = coarse.values[at(x0, y0, coarse.width)] * (1.0f - tx) +
                                        coarse.values[at(x1, y0, coarse.width)] * tx;
                    const float lower = coarse.values[at(x0, y1, coarse.width)] * (1.0f - tx) +
                                        coarse.values[at(x1, y1, coarse.width)] * tx;
                    fine.values[i] = upper * (1.0f - ty) + lower * ty;
                }
            }
            // Then towards the membrane over its holes (kRelax sweeps of each the mean of its four neighbours, each
            // step kOverRelax as far: plain steps took 200 sweeps to settle a hotbar's fill), so a slope goes on evenly
            // through a hole: pushed alone, a hole is filled with the means of the blocks beyond
            // its edge, too far at a slope's far side and too near at its near side. Not the map itself: its blocks
            // are 2 pixels, too small for that to show, and its holes the most (5 of the 9 ms the sweeps took).
            if (level == 0) {
                break;
            }
            inner_.clear();
            edges_.clear();
            for (int y = 0; y < fine.height; ++y) {
                for (int x = 0; x < fine.width; ++x) {
                    const std::size_t i = at(x, y, fine.width);
                    if (fine.known[i] != 0) {
                        continue;
                    }
                    if (x > 0 && x + 1 < fine.width && y > 0 && y + 1 < fine.height) {
                        inner_.push_back(static_cast<std::uint32_t>(i));
                    } else {
                        edges_.push_back({static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y)});
                    }
                }
            }
            float* const values = fine.values.data();
            const std::size_t row = static_cast<std::size_t>(fine.width);
            for (int sweep = 0; sweep < kRelax; ++sweep) {
                for (const std::uint32_t i : inner_) {
                    const float mean = 0.25f * (values[i - 1] + values[i + 1] + values[i - row] + values[i + row]);
                    values[i] += kOverRelax * (mean - values[i]);
                }
                // On the screen's edge: the mean of the neighbours there are (the trend taken out carries the slope
                // on to the edge; a rule going on with the slope there instead ran off with nothing round it to hold
                // it, under the game's bar across the whole bottom).
                for (const Hole& hole : edges_) {
                    const int x = hole.x;
                    const int y = hole.y;
                    const int w = fine.width;
                    float sum = 0.0f;
                    int neighbours = 0;
                    if (x > 0) {
                        sum += values[at(x - 1, y, w)];
                        ++neighbours;
                    }
                    if (x + 1 < w) {
                        sum += values[at(x + 1, y, w)];
                        ++neighbours;
                    }
                    if (y > 0) {
                        sum += values[at(x, y - 1, w)];
                        ++neighbours;
                    }
                    if (y + 1 < fine.height) {
                        sum += values[at(x, y + 1, w)];
                        ++neighbours;
                    }
                    if (neighbours > 0) {
                        float& value = values[at(x, y, w)];
                        value += kOverRelax * (sum / static_cast<float>(neighbours) - value);
                    }
                }
            }
        }
        // The trend back in: the world's pixels as they were, the holes' fill on it. A fill going on past the
        // world's range (the ground under a bar across the bottom is nearer than any of it seen) widens the range the
        // ranks and logs are taken over (the histogram stays the world's), by at most a quarter of it each way, so
        // its logs go on as well and leave no ridge.
        const float reach = 0.25f * (high - low);
        const float lowest = low - reach;
        const float highest = high + reach;
        for (int y = 0; y < height; ++y) {
            const float trend = trend_[static_cast<std::size_t>(y)];
            for (std::size_t i = at(0, y, width), end = i + static_cast<std::size_t>(width); i < end; ++i) {
                if (top.known[i] != 0) {
                    top.values[i] = halves_[input.map[i]];
                } else {
                    const float value = std::clamp(top.values[i] + trend, lowest, highest);
                    top.values[i] = value;
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
            }
        }
    }

    // 2. Rank: the world's histogram, each bin capped at kClip times the mean, the rest shared out evenly.
    const float span = std::max(high - low, 1e-6f);
    const float toBin = static_cast<float>(kBins) / span;
    histogram_.assign(kBins + 1, 0.0f);
    float world = 0.0f;
    for (std::size_t i = 0; i < pixels; ++i) {
        if (top.known[i] != 0) {
            const int bin = std::clamp(static_cast<int>((top.values[i] - low) * toBin), 0, kBins - 1);
            histogram_[static_cast<std::size_t>(bin)] += 1.0f;
            world += 1.0f;
        }
    }
    const float cap = kClip * world / static_cast<float>(kBins);
    float excess = 0.0f;
    for (int bin = 0; bin < kBins; ++bin) {
        float& count = histogram_[static_cast<std::size_t>(bin)];
        if (count > cap) {
            excess += count - cap;
            count = cap;
        }
    }
    // As a table at each bin's start: the world's share in the bins before it (the excess spread evenly), with
    // kStraight of a straight mapping; and the log of the depth there (for the local term).
    const float even = excess / static_cast<float>(kBins);
    float running = 0.0f;
    logs_.resize(kBins + 1);
    for (int bin = 0; bin <= kBins; ++bin) {
        float& count = histogram_[static_cast<std::size_t>(bin)];
        const float here = bin < kBins ? count + even : 0.0f;
        const float share = static_cast<float>(bin) / static_cast<float>(kBins);
        count = (1.0f - kStraight) * running / world + kStraight * share;
        running += here;
        logs_[static_cast<std::size_t>(bin)] = std::log(share * span + kLogFloor * span);
    }
    ranks_.resize(pixels);
    local_.resize(pixels);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float place = std::clamp((top.values[i] - low) * toBin, 0.0f, static_cast<float>(kBins));
        const int bin = std::min(static_cast<int>(place), kBins - 1);
        const float share = place - static_cast<float>(bin);
        const auto b = static_cast<std::size_t>(bin);
        ranks_[i] = histogram_[b] * (1.0f - share) + histogram_[b + 1] * share;
        local_[i] = logs_[b] * (1.0f - share) + logs_[b + 1] * share;
    }

    // 3. Near against the round about: each pixel's log depth against its mean over kLocalRadius pixels each way (twice
    // a box: a tent), softly limited, kLocal of the level. The interface's filled pixels count in: they go on with the
    // world's slope, so they leave no ridge along a piece's edge (the world's pixels alone would: beside a hole on a
    // slope their mean is one-sided).
    boxBlur(local_, width, height, kLocalRadius / 2, rows_, around_, sums_);
    boxBlur(around_, width, height, kLocalRadius / 2, rows_, blurred_, sums_);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float nearer = softLimit((local_[i] - blurred_[i]) / kLocalSpread);
        const float level = (1.0f - kLocal) * ranks_[i] + kLocal * (0.5f + 0.5f * nearer);
        levels[i] = static_cast<std::uint8_t>(std::clamp(std::lround(level * 255.0f), 0L, 255L));
    }
    return true;
}

} // namespace observex
