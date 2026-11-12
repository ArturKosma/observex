#include "heuristics/Snapper.h"

#include <algorithm>
#include <cmath>

#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define OBSERVEX_SSE2 1
#endif

namespace observex {

void GreyImage::fromBgra(const std::uint8_t* pixels, int width, int height, std::ptrdiff_t stride) {
    Level& full = levels_[0];
    full.width = width;
    full.height = height;
    full.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* in = pixels + stride * y;
        std::uint8_t* out = full.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 0; x < width; ++x, in += 4) {
            out[x] = static_cast<std::uint8_t>((in[0] * 29u + in[1] * 150u + in[2] * 77u) >> 8); // B, G, R.
        }
    }
    halve();
}

void GreyImage::fromGrey(const std::uint8_t* pixels, int width, int height, std::ptrdiff_t stride) {
    Level& full = levels_[0];
    full.width = width;
    full.height = height;
    full.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
        std::copy_n(pixels + stride * y, width,
                    full.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width));
    }
    halve();
}

GreyView GreyImage::view(std::uint64_t image) const {
    GreyView view;
    view.image = image;
    for (std::size_t l = 0; l < levels_.size(); ++l) {
        view.levels[l] = {levels_[l].pixels.data(), levels_[l].width, levels_[l].height, levels_[l].width};
    }
    return view;
}

void GreyImage::halve() {
    for (std::size_t l = 1; l < levels_.size(); ++l) {
        const Level& above = levels_[l - 1];
        Level& level = levels_[l];
        level.width = above.width / 2;
        level.height = above.height / 2;
        level.pixels.resize(static_cast<std::size_t>(level.width) * static_cast<std::size_t>(level.height));
        for (int y = 0; y < level.height; ++y) {
            const std::uint8_t* a = above.pixels.data() + static_cast<std::size_t>(2 * y) * static_cast<std::size_t>(above.width);
            const std::uint8_t* b = a + above.width;
            std::uint8_t* out = level.pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(level.width);
            for (int x = 0; x < level.width; ++x) {
                out[x] = static_cast<std::uint8_t>((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
            }
        }
    }
}

namespace {

constexpr float kNone = -2.0f; // No match there (the look does not fit on the image).

struct Best {
    int x = 0; // Where the patch starts, at its level.
    int y = 0;
    float match = kNone;
};

} // namespace

#ifdef OBSERVEX_SSE2
// 16 of 0xFF, then 16 of 0: from `16 - n` on, the mask that keeps a row's first n pixels.
constexpr std::uint8_t kKeep[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
#endif

// How well `patch` matches the image at `left`, `top` of `level`: zero-mean normalised cross-correlation. 16 pixels
// at a time (SSE2, which every x64 processor has; a row's last 16 cut to the patch's width), except at the image's
// right edge: one at a time took 2.3 ms a frame for the 7 boxes on a Lineage 2 clip's frames, most of it on far
// characters under 50 pixels, whose looks are too narrow for 16 at a time without the cut.
template <typename Patch>
static float matchAt(const GreyView& image, int level, int left, int top, const Patch& patch) {
    if (left < 0 || top < 0 || left + patch.width > image.width(level) || top + patch.height > image.height(level)) {
        return kNone;
    }
    std::uint64_t sum = 0;
    std::uint64_t squares = 0;
    std::int64_t cross = 0;
#ifdef OBSERVEX_SSE2
    if (left + patch.stride <= image.width(level)) { // The last 16 of each row are on the image.
        const __m128i zero = _mm_setzero_si128();
        const __m128i keep = _mm_loadu_si128(reinterpret_cast<const __m128i*>(kKeep + patch.stride - patch.width));
        __m128i sums = zero;    // Two 64-bit sums.
        __m128i squared = zero; // Four 32-bit ones: at most 255 * 255 * 4 for each 16 pixels, and a patch is at most
        __m128i crossed = zero; // 320 x 180 (the eighth of a 2560 x 1440 screen).
        const auto add = [&](__m128i pixels, const std::int16_t* values) {
            const __m128i low = _mm_unpacklo_epi8(pixels, zero);
            const __m128i high = _mm_unpackhi_epi8(pixels, zero);
            sums = _mm_add_epi64(sums, _mm_sad_epu8(pixels, zero));
            squared = _mm_add_epi32(squared, _mm_add_epi32(_mm_madd_epi16(low, low), _mm_madd_epi16(high, high)));
            const __m128i lowValues = _mm_loadu_si128(reinterpret_cast<const __m128i*>(values));
            const __m128i highValues = _mm_loadu_si128(reinterpret_cast<const __m128i*>(values + 8));
            crossed = _mm_add_epi32(crossed, _mm_add_epi32(_mm_madd_epi16(low, lowValues),
                                                           _mm_madd_epi16(high, highValues)));
        };
        const int last = patch.stride - 16;
        const std::int16_t* values = patch.values.data();
        for (int y = 0; y < patch.height; ++y, values += patch.stride) {
            const std::uint8_t* row = image.row(level, top + y) + left;
            for (int x = 0; x < last; x += 16) {
                add(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row + x)), values + x);
            }
            add(_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(row + last)), keep), values + last);
        }
        alignas(16) std::uint64_t sumLanes[2];
        alignas(16) std::uint32_t squareLanes[4];
        alignas(16) std::int32_t crossLanes[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(sumLanes), sums);
        _mm_store_si128(reinterpret_cast<__m128i*>(squareLanes), squared);
        _mm_store_si128(reinterpret_cast<__m128i*>(crossLanes), crossed);
        sum = sumLanes[0] + sumLanes[1];
        squares = static_cast<std::uint64_t>(squareLanes[0]) + squareLanes[1] + squareLanes[2] + squareLanes[3];
        cross = static_cast<std::int64_t>(crossLanes[0]) + crossLanes[1] + crossLanes[2] + crossLanes[3];
    } else
#endif
    {
        const std::int16_t* values = patch.values.data();
        for (int y = 0; y < patch.height; ++y, values += patch.stride) {
            const std::uint8_t* row = image.row(level, top + y) + left;
            for (int x = 0; x < patch.width; ++x) {
                const std::int32_t v = row[x];
                sum += static_cast<std::uint64_t>(v);
                squares += static_cast<std::uint64_t>(v * v);
                cross += v * values[x];
            }
        }
    }
    const double count = static_cast<double>(patch.width) * static_cast<double>(patch.height);
    const double mean = static_cast<double>(sum) / count;
    const double spread = static_cast<double>(squares) - static_cast<double>(sum) * mean;
    if (spread <= count) { // Flat there (under a grey level apart): nothing to match.
        return -1.0f;
    }
    // The patch's values were rounded, so they do not quite add up to 0.
    const double centred = static_cast<double>(cross) - mean * static_cast<double>(patch.total);
    return static_cast<float>(centred / (std::sqrt(spread) * static_cast<double>(patch.norm)));
}

template <typename Patch>
static Best search(const GreyView& image, int level, const Patch& patch, int centreX, int centreY, int radius) {
    Best best;
    for (int y = centreY - radius; y <= centreY + radius; ++y) {
        for (int x = centreX - radius; x <= centreX + radius; ++x) {
            if (const float match = matchAt(image, level, x, y, patch); match > best.match) {
                best = {x, y, match};
            }
        }
    }
    return best;
}

void Snapper::levelsOf(float width, float height, int& finest, int& coarsest) {
    const float side = std::max(width, height);
    const float shorter = std::min(width, height);
    coarsest = 0;
    while (coarsest + 1 < GreyView::kLevels && side / static_cast<float>(1 << coarsest) > kCoarseSide &&
           shorter / static_cast<float>(2 << coarsest) >= 4.0f) {
        ++coarsest;
    }
    finest = 0;
    while (finest < coarsest && side / static_cast<float>(1 << finest) > kFineSide) {
        ++finest;
    }
}

GreyArea Snapper::learnArea(const Detection& box) {
    // learn() reads the box rounded on each level: within a pixel of that level, so two of the coarsest.
    int finest = 0;
    int coarsest = 0;
    levelsOf(box.width, box.height, finest, coarsest);
    const float margin = static_cast<float>(2 << coarsest);
    return {static_cast<int>(std::floor(box.x - margin)), static_cast<int>(std::floor(box.y - margin)),
            static_cast<int>(std::ceil(box.x + box.width + margin)),
            static_cast<int>(std::ceil(box.y + box.height + margin))};
}

GreyArea Snapper::findArea(std::uint32_t id, float width, float height, float guessX, float guessY,
                           float reach) const {
    // find() reads the look at every place within `reach` (on the coarsest level, rounded up to its pixels), each
    // finer level up to 2 of its pixels further, plus roundings: within 6 pixels of the coarsest. The look is the
    // one learned, or one of a box this size about to be.
    int finest = 0;
    int coarsest = 0;
    levelsOf(width, height, finest, coarsest);
    if (const auto known = looks_.find(id); known != looks_.end()) {
        const Look& look = known->second;
        coarsest = std::max(coarsest, look.coarsest);
        for (int level = look.finest; level <= look.coarsest; ++level) {
            const Patch& patch = look.patches[static_cast<std::size_t>(level)];
            width = std::max(width, static_cast<float>((patch.width + 1) << level));
            height = std::max(height, static_cast<float>((patch.height + 1) << level));
        }
    }
    const float margin = reach + static_cast<float>(6 << coarsest);
    return {static_cast<int>(std::floor(guessX - margin)), static_cast<int>(std::floor(guessY - margin)),
            static_cast<int>(std::ceil(guessX + width + margin)), static_cast<int>(std::ceil(guessY + height + margin))};
}

void Snapper::learn(const GreyView& image, std::uint32_t id, const Detection& box) {
    learned_[id] = {image.image, box};
    Look look;
    levelsOf(box.width, box.height, look.finest, look.coarsest);
    for (int level = look.finest; level <= look.coarsest; ++level) {
        const float scale = static_cast<float>(1 << level);
        const float left = box.x / scale;
        const float top = box.y / scale;
        const int x0 = static_cast<int>(std::lround(left));
        const int y0 = static_cast<int>(std::lround(top));
        // Only what is on the image (a close character can be cut by the screen's edge).
        const int x1 = std::min(x0 + std::max(1, static_cast<int>(std::lround(box.width / scale))), image.width(level));
        const int y1 = std::min(y0 + std::max(1, static_cast<int>(std::lround(box.height / scale))), image.height(level));
        const int startX = std::max(x0, 0);
        const int startY = std::max(y0, 0);
        Patch& patch = look.patches[static_cast<std::size_t>(level)];
        patch.width = x1 - startX;
        patch.height = y1 - startY;
        if (patch.width < 3 || patch.height < 3) {
            return; // Too little of it shows: keep what was learned before.
        }
        patch.offsetX = static_cast<float>(startX) - left;
        patch.offsetY = static_cast<float>(startY) - top;
        std::uint64_t sum = 0;
        for (int y = 0; y < patch.height; ++y) {
            const std::uint8_t* row = image.row(level, startY + y) + startX;
            for (int x = 0; x < patch.width; ++x) {
                sum += row[x];
            }
        }
        const std::size_t count = static_cast<std::size_t>(patch.width) * static_cast<std::size_t>(patch.height);
        const double mean = static_cast<double>(sum) / static_cast<double>(count);
        patch.stride = (patch.width + 15) / 16 * 16;
        patch.values.assign(static_cast<std::size_t>(patch.stride) * static_cast<std::size_t>(patch.height), 0);
        double squares = 0.0;
        patch.total = 0;
        for (int y = 0; y < patch.height; ++y) {
            const std::uint8_t* row = image.row(level, startY + y) + startX;
            for (int x = 0; x < patch.width; ++x) {
                const auto value = static_cast<std::int16_t>(std::lround(static_cast<double>(row[x]) - mean));
                patch.values[static_cast<std::size_t>(y * patch.stride + x)] = value;
                patch.total += value;
                squares += static_cast<double>(value) * static_cast<double>(value);
            }
        }
        if (squares <= 4.0 * static_cast<double>(count)) {
            return; // Flat (under 2 grey levels apart on average): nothing to find it by.
        }
        patch.norm = static_cast<float>(std::sqrt(squares));
    }
    looks_[id] = std::move(look);
}

void Snapper::keepOnly(std::span<const std::uint32_t> ids) {
    const auto gone = [&](const auto& entry) { return std::find(ids.begin(), ids.end(), entry.first) == ids.end(); };
    std::erase_if(looks_, gone);
    std::erase_if(learned_, gone);
}

bool Snapper::find(const GreyView& image, std::uint32_t id, float guessX, float guessY, float reach,
                   Found& found) const {
    found.match = -2.0f;
    const auto known = looks_.find(id);
    if (known == looks_.end()) {
        return false;
    }
    const Look& look = known->second;
    int level = look.coarsest;
    const auto scale = static_cast<float>(1 << level);
    const Patch& coarse = look.patches[static_cast<std::size_t>(level)];
    Best best = search(image, level, coarse, static_cast<int>(std::lround(guessX / scale + coarse.offsetX)),
                       static_cast<int>(std::lround(guessY / scale + coarse.offsetY)),
                       std::max(1, static_cast<int>(std::ceil(reach / scale))));
    // Then each finer level, near where the coarser one put it.
    while (level > look.finest && best.match > kNone) {
        const Patch& above = look.patches[static_cast<std::size_t>(level)];
        const Patch& below = look.patches[static_cast<std::size_t>(level - 1)];
        const float boxX = (static_cast<float>(best.x) - above.offsetX) * 2.0f;
        const float boxY = (static_cast<float>(best.y) - above.offsetY) * 2.0f;
        --level;
        best = search(image, level, below, static_cast<int>(std::lround(boxX + below.offsetX)),
                      static_cast<int>(std::lround(boxY + below.offsetY)), 2);
    }
    found.match = best.match;
    if (best.match < kMinMatch) {
        return false;
    }
    // A fraction of a pixel: the top of a parabola through the matches on either side.
    const int finest = look.finest;
    const Patch& patch = look.patches[static_cast<std::size_t>(finest)];
    const auto fraction = [](float before, float at, float after) {
        const float curve = before - 2.0f * at + after;
        if (before <= kNone || after <= kNone || curve >= 0.0f) {
            return 0.0f;
        }
        return std::clamp(0.5f * (before - after) / curve, -0.5f, 0.5f);
    };
    const float dx = fraction(matchAt(image, finest, best.x - 1, best.y, patch), best.match,
                              matchAt(image, finest, best.x + 1, best.y, patch));
    const float dy = fraction(matchAt(image, finest, best.x, best.y - 1, patch), best.match,
                              matchAt(image, finest, best.x, best.y + 1, patch));
    const auto finestScale = static_cast<float>(1 << finest);
    found.x = (static_cast<float>(best.x) + dx - patch.offsetX) * finestScale;
    found.y = (static_cast<float>(best.y) + dy - patch.offsetY) * finestScale;
    return true;
}

} // namespace observex
