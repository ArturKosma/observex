#pragma once

#include <cstdint>
#include <vector>

namespace observex {

// --depth: the Show flag Depth over a saved clip (bench/DepthBench.cpp).
struct DepthBenchOptions {
    const char* clip = nullptr;       // A clip folder (its clip.mp4) or an .mp4 file.
    const char* model = nullptr;      // The depth network's folder (default models/depth).
    const char* layout = nullptr;     // ui.txt's folder (default models/ui), for the interface to leave out.
    const char* heuristics = nullptr; // An observex_heuristics.dll to shade with (default: the rules built in).
    const char* out = nullptr;        // Where the pictures and depth.bin go (none: nothing written).
    unsigned every = 1;               // Every Nth frame through the network.
    std::vector<std::uint64_t> at;    // Frames whose picture is written, and compared with --compare.
    const char* compare = nullptr;    // A .npy of the Python network's map of the first --at frame.
};

int depthClip(const DepthBenchOptions& options);

} // namespace observex
