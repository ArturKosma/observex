#pragma once

#include "core/Detection.h"
#include "core/Heuristics.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace observex {

// A grey copy of a screen image and its halvings that keeps its own pixels (core/Heuristics' GreyView points at
// them), made on the CPU: for the tests and the bench (live, engine/GreyMaker makes them on the GPU).
class GreyImage {
public:
    static constexpr int kLevels = GreyView::kLevels;

    // From BGRA pixels (ScreenCapture::kImageFormat), `stride` bytes a row.
    void fromBgra(const std::uint8_t* pixels, int width, int height, std::ptrdiff_t stride);
    // From grey pixels, `stride` bytes a row.
    void fromGrey(const std::uint8_t* pixels, int width, int height, std::ptrdiff_t stride);

    // As screen image `image` (GreyView::image). Valid until it is made again or goes.
    GreyView view(std::uint64_t image = 0) const;

private:
    void halve();

    struct Level {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels;
    };
    std::array<Level, kLevels> levels_;
};

// Puts boxes onto their things on a newer screen image than the one the model looked at (the user asked, 2026-11-11:
// boxes snapped to the characters in a fast camera turn, where foretelling from earlier looks leaves them 20-50
// pixels off). It learns how each thing looks from its box on an image where the model found it, then finds that
// look on the newer image near where the thing is expected: zero-mean normalised cross-correlation, from the
// coarsest halving with the look at most kCoarseSide pixels down to the finest with it at most kFineSide, to a
// fraction of a pixel there. Things are told apart by id. Used by heuristics/SnapRules.
class Snapper {
public:
    static constexpr float kMinMatch = 0.6f; // Matches below this (of -1 to 1) are no match (0.7: 9.0% of the
                                             // fast boxes outside their things, not 4.3%: blur lowers real ones).
    static constexpr int kCoarseSide = 24;   // The coarsest search: the look at most this many pixels on a side.
    // The finest: the look at most this many pixels on a side (or full size), which bounds the work a box (a close
    // character is 200 pixels high); a fraction of a pixel there is a little more at full size.
    static constexpr int kFineSide = 48;
    // Which way of learning and finding this is (core/Heuristics' SnapFind::finder): a clip's replay takes the searches
    // recorded live only from the same one. It follows the numbers above by itself; change what learn() or find() do
    // otherwise and add one to kFinderRevision, or replays take results the new way would not find.
    static constexpr std::uint32_t kFinderRevision = 1;
    static constexpr std::uint32_t kFinder = (kFinderRevision * 1'000'003u) ^
                                             (static_cast<std::uint32_t>(kMinMatch * 10'000.0f) << 8) ^
                                             (static_cast<std::uint32_t>(kCoarseSide) << 20) ^
                                             (static_cast<std::uint32_t>(kFineSide) << 26);

    // How the thing `id` looks: its box on `image`, where the model found it. Replaces what was learned of it; keeps
    // it if too little of the box is on the image or it is flat there.
    void learn(const GreyView& image, std::uint32_t id, const Detection& box);
    // Forgets the things not in `ids`.
    void keepOnly(std::span<const std::uint32_t> ids);
    void forget(std::uint32_t id) {
        looks_.erase(id);
        learned_.erase(id);
    }
    void clear() {
        looks_.clear();
        learned_.clear();
    }
    bool knows(std::uint32_t id) const { return looks_.contains(id); }

    // The image and box the thing `id` was last learned from (learn()'s, also one that kept what it had); null if
    // none. A replay's searches are the same as live's only for a look learned the same (core/Heuristics' SnapFind).
    struct Learned {
        std::uint64_t image = 0; // GreyView::image.
        Detection box;
    };
    const Learned* learnedOf(std::uint32_t id) const {
        const auto known = learned_.find(id);
        return known != learned_.end() ? &known->second : nullptr;
    }

    struct Found {
        float x = 0.0f; // Its box's top left on the image,
        float y = 0.0f;
        float match = -2.0f; // and how well the best place matched (also when false; -2: nothing learned of it).
    };
    // Where the thing `id` is on `image`: its box's top left, looked for within `reach` pixels of (`guessX`,
    // `guessY`) both ways. False if nothing was learned of it, or nothing there matches kMinMatch.
    bool find(const GreyView& image, std::uint32_t id, float guessX, float guessY, float reach, Found& found) const;

    // The part of an image learn() reads for `box` (Snapping::learnAreas).
    static GreyArea learnArea(const Detection& box);
    // The part of an image find() reads for `id` (Snapping::snapAreas): looked for as a box `width` x `height` (as
    // it is now, or as it is about to be learned), within `reach` of (`guessX`, `guessY`).
    GreyArea findArea(std::uint32_t id, float width, float height, float guessX, float guessY, float reach) const;

private:
    // The levels a box this size is matched on.
    static void levelsOf(float width, float height, int& finest, int& coarsest);

    struct Patch {
        int width = 0;
        int height = 0;
        float offsetX = 0.0f; // Where its pixels start, less where the box starts, at its level (-0.5 to 0.5).
        float offsetY = 0.0f;
        float norm = 0.0f;               // Root of the summed squares of
        std::int32_t total = 0;          // (and the sum of)
        std::vector<std::int16_t> values; // the pixels less their mean, rounded: rows `stride` apart, 0 after
        int stride = 0;                   // `width` (a multiple of 16, for matching 16 pixels at a time).
    };
    struct Look {
        int finest = 0;
        int coarsest = 0;
        std::array<Patch, GreyView::kLevels> patches; // Per level, from the finest to the coarsest.
    };

    std::unordered_map<std::uint32_t, Look> looks_;
    std::unordered_map<std::uint32_t, Learned> learned_;
};

} // namespace observex
