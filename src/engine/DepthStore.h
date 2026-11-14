#pragma once

#include "core/Depth.h"
#include "core/Heuristics.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace observex {

// A clip's depth maps (the Show flag Depth): depth.bin in its folder, beside clip.txt, whose depth lines say which map
// was shown over which screen image (core/ClipLines).
inline constexpr char kClipDepth[] = "depth.bin";

// One depth map as the clip buffer keeps it (engine/ClipRecorder): what the network gave, and what was shown of it,
// both packed without loss (DepthPacker), so a clip replays exactly what was seen (Recorded: the levels and palette)
// and can be shaded again by today's heuristics (Today: the map and the interface left out).
struct DepthRecord {
    std::uint64_t image = 0;   // The screen image it was made from (ImageStamp::number).
    std::int64_t imageQpc = 0; // When the screen showed it (only while in the buffer).
    int width = 0;             // The map's size,
    int height = 0;
    int screenWidth = 0;       // and the screen's.
    int screenHeight = 0;
    std::vector<GreyArea> ignore;      // DepthInput::ignore: the interface's pieces on that image.
    std::vector<std::uint8_t> map;     // The network's map, packed.
    std::vector<std::uint8_t> levels;  // The levels shown, packed,
    std::array<std::uint32_t, Depthing::kPaletteSize> palette{}; // in these colours (Depthing::shade).
};

// Packs maps and levels without loss and unpacks them: each pixel as its difference from the one to its left (depth
// changes smoothly, so most differences are small), then Windows' own compression (XPRESS with Huffman coding,
// cabinet.dll). Use one from one thread.
class DepthPacker {
public:
    DepthPacker(); // Throws if Windows' compression cannot be opened.
    ~DepthPacker();

    DepthPacker(const DepthPacker&) = delete;
    DepthPacker& operator=(const DepthPacker&) = delete;

    // Packs `map` (record.width x record.height half floats) into record.map and `levels` into record.levels.
    // False if it could not.
    bool pack(const std::uint16_t* map, const std::uint8_t* levels, DepthRecord& record);
    // The map and the levels back, record.width x record.height each. False if they do not unpack.
    bool unpackMap(const DepthRecord& record, std::vector<std::uint16_t>& map);
    bool unpackLevels(const DepthRecord& record, std::vector<std::uint8_t>& levels);

private:
    bool compress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);
    bool decompress(const std::vector<std::uint8_t>& packed, std::uint8_t* out, std::size_t size);

    void* compressor_ = nullptr; // COMPRESSOR_HANDLE, DECOMPRESSOR_HANDLE.
    void* decompressor_ = nullptr;
    std::vector<std::uint8_t> scratch_;
};

// Writes `records` into `file` (depth.bin). Throws on failure.
void writeDepthFile(const std::filesystem::path& file, std::span<const std::shared_ptr<const DepthRecord>> records);
// Reads them back (imageQpc 0), in the order written. Empty and `error` set (if given) when it cannot.
std::vector<DepthRecord> readDepthFile(const std::filesystem::path& file, std::string* error = nullptr);

} // namespace observex
