#include "heuristics/Miniatures.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace observex {

namespace {

// A frame line's pixel: at least this grey, and this much brighter than the pixel two away outside the frame (above
// the top edge, left of the left one). The hotbar's edges are 240-255 against 60-95 outside.
constexpr int kLineGrey = 120;
constexpr int kLineStep = 40;
// How big a miniature may be (the hotbar's are 42 at 2560 x 1440 with the user's UI scale).
constexpr int kSmallest = 24;
constexpr int kLargest = 64;
// The share of the left edge's pixels that must be a line (a letter of the key over it breaks it).
constexpr float kLeftShare = 0.85f;

bool lineAbove(const std::uint8_t* grey, std::ptrdiff_t stride, int x, int y) {
    const int value = grey[stride * y + x];
    return value >= kLineGrey && value - grey[stride * (y - 2) + x] >= kLineStep;
}

bool lineLeft(const std::uint8_t* grey, std::ptrdiff_t stride, int x, int y) {
    const int value = grey[stride * y + x];
    return value >= kLineGrey && value - grey[stride * y + x - 2] >= kLineStep;
}

} // namespace

int findMiniatures(const std::uint8_t* grey, std::ptrdiff_t stride, int width, int height, StatsArea area,
                   StatsArea* out, int max) {
    // Two pixels inside the image all round: the tests look two away.
    const int x0 = std::max(area.x0, 3);
    const int y0 = std::max(area.y0, 2);
    const int x1 = std::min(area.x1, width - 1);
    const int y1 = std::min(area.y1, height);
    int count = 0;
    for (int y = y0; y < y1 && count < max; ++y) {
        int x = x0;
        while (x < x1 && count < max) {
            if (!lineAbove(grey, stride, x, y)) {
                ++x;
                continue;
            }
            // A top edge: as long as the line goes on.
            const int from = x;
            while (x < x1 && lineAbove(grey, stride, x, y)) {
                ++x;
            }
            const int side = x - from;
            if (side < kSmallest || side > kLargest || y + side > height) {
                continue;
            }
            // A left edge as long, where it starts (or a pixel either way).
            for (const int left : {from, from - 1, from + 1}) {
                int lines = 0;
                for (int down = 0; down < side; ++down) {
                    lines += lineLeft(grey, stride, left, y + down) ? 1 : 0;
                }
                if (static_cast<float>(lines) >= kLeftShare * static_cast<float>(side)) {
                    const StatsArea box{left, y, left + side, y + side};
                    // Not one found already a few pixels away (the next row of the same top edge).
                    const bool known = std::any_of(out, out + count, [&](const StatsArea& other) {
                        return std::abs(other.x0 - box.x0) <= 3 && std::abs(other.y0 - box.y0) <= 3;
                    });
                    if (!known) {
                        out[count++] = box;
                    }
                    break;
                }
            }
        }
    }
    return count;
}

std::string skillFileName(std::string_view name) {
    std::size_t first = 0;
    std::size_t last = name.size();
    while (first < last && (name[first] == ' ' || name[first] == '\t')) {
        ++first;
    }
    while (last > first && (name[last - 1] == ' ' || name[last - 1] == '\t')) {
        --last;
    }
    const std::string_view trimmed = name.substr(first, last - first);
    if (trimmed.empty() || trimmed.size() > 31 || trimmed.back() == '.') {
        return {};
    }
    for (const char c : trimmed) {
        if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*_").find(c) != std::string_view::npos) {
            return {};
        }
    }
    return std::string(trimmed);
}

bool saveSkill(const std::filesystem::path& folder, std::string_view name, const std::vector<std::uint8_t>& pixels,
               int side, std::string* error) {
    const std::string file = skillFileName(name);
    if (file.empty() || side <= 0 || pixels.size() != static_cast<std::size_t>(side) * static_cast<std::size_t>(side)) {
        if (error != nullptr) {
            *error = "Not a name a skill can have";
        }
        return false;
    }
    std::error_code made;
    const std::filesystem::path skills = folder / kSkillsFolder;
    std::filesystem::create_directories(skills, made);
    const std::filesystem::path path = skills / std::filesystem::path(std::u8string(
                                                    reinterpret_cast<const char8_t*>(file.data()), file.size()));
    std::ofstream to(path.wstring() + L".pgm", std::ios::binary | std::ios::trunc);
    char head[32];
    const int length = std::snprintf(head, sizeof(head), "P5\n%d %d\n255\n", side, side);
    to.write(head, length);
    to.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (!to) {
        if (error != nullptr) {
            *error = "Could not write " + file + ".pgm into " + skills.string();
        }
        return false;
    }
    return true;
}

} // namespace observex
