#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace observex {

// Reads words off a small grey picture with Windows' own OCR (Windows.Media.Ocr, the user's profile languages; en-US
// and en-GB on the user's PC), for text no font of glyphs covers, e.g. a target's name (the user asked, 2026-11-12).
// It takes tens of milliseconds, so callers ask only for a picture they have not read before (heuristics/StatsReader
// keeps every name it read). Made lazily on first use, on the calling thread (which it joins to Windows' threads as
// multi-threaded if it is not yet); use one from one thread.
class TextOcr {
public:
    TextOcr();
    ~TextOcr();
    TextOcr(const TextOcr&) = delete;
    TextOcr& operator=(const TextOcr&) = delete;

    // The words on `grey` (`width` x `height`, rows `stride` bytes apart; dark letters on a light ground read best), as
    // one line joined by spaces, UTF-8; "" if none, or if Windows has no OCR (then *error says why, if given).
    std::string read(const std::uint8_t* grey, int width, int height, std::ptrdiff_t stride, std::string* error);

private:
    struct Engine;
    std::unique_ptr<Engine> engine_;
    bool failed_ = false;
};

} // namespace observex
