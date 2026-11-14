#pragma once

#include "core/Depth.h"

#include <cstdint>
#include <vector>

namespace observex {

// How observex shows the depth network's map today (core/Depth's Depthing; the user asked, 2026-11-12: colours vivid
// enough that even a distant monster shows, the background behind it darker).
//
// 1. The interface (DepthInput::ignore, kMargin map pixels wider: the network smears a window's edge into the world
//    beside it) is no depth of the world. Its pixels are filled from the world round them the way a texture's
//    pixels past its edges are (the user's idea, 2026-11-12; push-pull: the known pixels averaged into ever smaller
//    halvings until no hole is left, then each hole filled from the halving above it, bilinearly, from the smallest
//    up, and smoothed there towards the membrane its edge spans, so a slope goes on evenly through it; with the trend
//    down the screen, each row's mean, taken out first, so the ground's slope goes on to the screen's edge too):
//    smooth, without seams, and left out of the ranks below.
// 2. Near and far by rank, not by value (contrast-limited histogram equalisation over the world's pixels): inverse
//    depth falls off as 1 / distance, so by value everything past a few metres is squeezed into the darkest few
//    levels. By rank the far half of the screen gets its half of the levels. Each histogram bin counts at most kClip
//    times the mean (the rest is shared out evenly), so a large flat stretch does not spend all the levels on its own
//    noise; kStraight of a plain straight mapping is mixed in.
// 3. Near against what is round it: rank alone left a wolf 30 metres off 1 to 3 levels over the ground at its feet
//    (clip 2026-11-12_17-43-14, frame 79). So kLocal of the level is how much nearer a pixel is than the pixels round
//    it (its log depth against their mean over kLocalRadius pixels each way, softly limited at kLocalSpread): a thing
//    standing on the ground is brighter, the ground just behind it darker, the ground itself (whose inverse depth
//    changes evenly down the screen) unchanged. In logs, so a far thing counts as much as a near one.
// 4. Shown in inferno's colours (matplotlib's palette: black, purple, red, orange, yellow, nearer brighter).
//
// Keeps buffers between maps, nothing else: the same map always shades the same. Use one from one thread.
class DepthRules final : public Depthing {
public:
    static constexpr int kMargin = 2;            // Map pixels round each interface piece left out too.
    static constexpr int kTrendShare = 16;       // A row's mean counts with at least 1 / this of its pixels the world's;
    static constexpr int kTrendRows = 16;        // rows with fewer go on along the line through this many nearest,
    static constexpr int kTrendRadius = 32;      // and the means are smoothed over this many rows each way.
    static constexpr int kRelax = 12;            // Sweeps towards the membrane over the holes, on each halving
                                                 // but the map itself,
    static constexpr float kOverRelax = 1.8f;    // each step this much farther than to its neighbours' mean.
    static constexpr int kBins = 2048;           // The histogram's.
    static constexpr float kClip = 3.0f;         // A bin counts at most this many times the mean.
    static constexpr float kStraight = 0.1f;     // Of the rank, this share from a straight mapping.
    static constexpr float kLocal = 0.3f;        // Of the level, this share from the near-against-round term,
    static constexpr int kLocalRadius = 12;      // over this many map pixels each way,
    static constexpr float kLocalSpread = 0.03f; // limited softly at this much nearer (in log depth: 3%),
    static constexpr float kLogFloor = 0.02f;    // the log taken of the depth above the world's least plus this much
                                                 // of its range (the sky's is about 0).

    bool shade(const DepthInput& input, std::uint8_t* levels, std::uint32_t* palette) override;

    // The palette shade() writes.
    static void inferno(std::uint32_t* palette);

private:
    // One halving of the push-pull: its values, and 1 where known (0: a hole).
    struct Level {
        int width = 0;
        int height = 0;
        std::vector<float> values;
        std::vector<std::uint8_t> known;
    };
    std::vector<float> halves_; // Every half float's value (65 536).
    std::vector<Level> halvings_;
    struct Hole {
        std::uint16_t x = 0;
        std::uint16_t y = 0;
    };
    std::vector<std::uint32_t> inner_; // The holes of the halving being filled: inside it (by index),
    std::vector<Hole> edges_;          // and on its edges.
    std::vector<float> rowMeans_;          // The trend down the screen: each row's mean over the world's pixels,
    std::vector<std::uint8_t> rowKnown_;   // 1 where enough of them made it,
    std::vector<float> trend_;             // and smoothed.
    std::vector<float> histogram_; // Then the rank at each bin's start,
    std::vector<float> logs_;      // and the log depth there.
    std::vector<float> ranks_;     // Each pixel's,
    std::vector<float> local_;     // its log depth,
    std::vector<float> around_;    // blurred once
    std::vector<float> blurred_;   // and twice.
    std::vector<float> rows_;
    std::vector<float> sums_;
};

} // namespace observex
