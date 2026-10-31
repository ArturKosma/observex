#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace observex {

enum class ImageFormat {
    Png,  // Lossless; for UI snapshots.
    Jpeg, // Quality 85: about 0.7 MB for a 2560 x 1440 game image; for recordings.
};

// Writes 8-bit BGRA pixels (rows `stride` bytes apart; alpha is ignored) as an image file, replacing any
// file already there. Not for the hot path: it encodes on the calling thread. Throws on failure.
void writeImageFile(const std::filesystem::path& path, ImageFormat format, const void* bgra, unsigned width,
                    unsigned height, unsigned stride);

// An image read from a file: 8-bit BGRA pixels, rows `width * 4` bytes apart, alpha 255.
struct ImageData {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<std::uint8_t> bgra;
};

// Reads a PNG or JPEG file (or any format Windows can read). Throws on failure.
ImageData readImageFile(const std::filesystem::path& path);

} // namespace observex
