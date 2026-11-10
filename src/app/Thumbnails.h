#pragma once

#include "app/ScreenImage.h"
#include "engine/Gpu.h"

#include <filesystem>
#include <map>
#include <memory>

namespace observex {

// Small pictures of image files, for the Review button's list (each recording's first image; the user asked,
// 2026-11-02). Read on the UI thread, at most one a frame (about 6 ms for a 2560 x 1440 JPEG read 8 times smaller),
// so the list opens at once and its pictures fill in over a few frames. Kept until observex closes: the next
// opening shows them at once.
class Thumbnails {
public:
    // Pictures are read this wide at most (1/8 of a 2560 pixel wide screen), which stays sharp at the list's size.
    static constexpr unsigned kWidth = 320;

    explicit Thumbnails(ID3D11Device* device) : device_(device) {}

    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;

    // The picture of `file`, or null while it is not read yet or if it cannot be read. Reads it now if no picture
    // was read yet this frame.
    const ScreenImage* get(const std::filesystem::path& file);

private:
    ID3D11Device* device_;
    std::map<std::filesystem::path, std::unique_ptr<ScreenImage>> pictures_; // Null for files that cannot be read.
    int readFrame_ = -1;                                                     // The ImGui frame that last read one.
};

} // namespace observex
