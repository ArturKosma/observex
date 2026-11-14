#pragma once

#include "core/Heuristics.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace observex {

// How near each pixel of the screen is, for the Show flag Depth (the user asked, 2026-11-12: step 2 of the depth plan,
// after the ground plane). A network (engine/DepthNet: Depth Anything V2 Small) guesses it from one screen image as a
// map of relative inverse depth: larger nearer, in no unit, with a scale and offset of each image's own. What observex
// makes of that map to show it is this: a level 0 to 255 per pixel and a palette of 256 colours (Depthing::shade).
//
// This is the fixed side: what goes in (DepthInput) and what comes out. The rules themselves are in src/heuristics
// (heuristics/DepthRules), built into observex_heuristics.dll beside the tracker's, so a change to how depth is shown
// needs only that DLL rebuilt (engine/HeuristicsLibrary loads it again in the running app). Shading keeps nothing from
// one map to the next, so a clip's recorded map shaded today shows exactly what was shown live as long as the DLL is
// the same (engine/ClipRecorder records the map, and what was shown).

// One map, as the network gave it.
struct DepthInput {
    const std::uint16_t* map = nullptr; // width x height half-precision floats, row by row.
    int width = 0;
    int height = 0;
    int screenWidth = 0; // The screen image it was made from.
    int screenHeight = 0;
    // The game's interface on that image (core/UiLayout's pieces), in the screen's pixels: the network guesses a
    // depth there too, of windows over the world, which says nothing of the world behind.
    const GreyArea* ignore = nullptr;
    int ignoreCount = 0;
};

// A map as the depth finder made it (engine/DepthFinder), for what reads depth beside the Show flag Depth (engine/
// GroundFinder: the camera's pitch, heuristics/DepthTilt). Never changed once made: shared between threads.
struct DepthMade {
    std::uint64_t image = 0;   // The screen image it was made from (ImageStamp::number),
    std::int64_t imageQpc = 0; // and when the screen showed it.
    int width = 0;
    int height = 0;
    int screenWidth = 0;
    int screenHeight = 0;
    std::vector<std::uint16_t> map; // The network's.
    std::vector<GreyArea> ignore;   // The interface's pieces on that image.

    DepthInput input() const {
        DepthInput in;
        in.map = map.data();
        in.width = width;
        in.height = height;
        in.screenWidth = screenWidth;
        in.screenHeight = screenHeight;
        in.ignore = ignore.data();
        in.ignoreCount = static_cast<int>(ignore.size());
        return in;
    }
};

class Depthing {
public:
    static constexpr int kPaletteSize = 256;

    virtual ~Depthing() = default;

    // Shades `input`: writes a level per pixel into `levels` (width x height bytes, row by row) and the colour each
    // level is shown in into `palette` (kPaletteSize of 0xAARRGGBB, which is a BGRA pixel's bytes). False if it failed
    // (nothing to show).
    virtual bool shade(const DepthInput& input, std::uint8_t* levels, std::uint32_t* palette) = 0;
};

// `count` levels painted with `palette` into BGRA pixels `out`.
inline void paintDepth(const std::uint8_t* levels, const std::uint32_t* palette, std::size_t count,
                       std::uint32_t* out) {
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = palette[levels[i]];
    }
}

// A half-precision float's value (IEEE 754 binary16: what the depth network gives).
float halfToFloat(std::uint16_t half);

} // namespace observex
