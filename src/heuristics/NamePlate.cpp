#include "heuristics/NamePlate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace observex {

namespace {

// An orb (measured 2026-11-15 on the user's screenshots and clips 2026-11-12_23-25-45 and 2026-11-13_00-50-14): its
// ring's middle kRing px from its centre (7.5 to 8.5), and its inside within kInside.
constexpr float kRingNear = 7.5f;
constexpr float kRingFar = 8.5f;
constexpr int kRingAngles = 16;
constexpr int kRingLeast = 13; // Of kRingAngles, ring-coloured at either distance.
constexpr int kReach = 9;      // How far the ring reaches out (the orb is 19 px across).
constexpr int kInside = 4;
// Orbs closer than this are one.
constexpr int kOrbApart = 4;
// The two orbs of a plate: on one row (this far apart at most), this far apart across (a name of 2 letters to a long
// one), the name's white letters between them, this many pixels at least, its first letter this far from the left
// orb's middle and its last this far from the right one's (14-16 and 13 on every plate seen; the button panel's round
// icons, the one look-alike left on 44 frames, sit inside the interface).
constexpr int kRowPlay = 2;
constexpr int kApartLeast = 24;
constexpr int kApartMost = 520;
constexpr int kTextRows = 7; // Either way of the orbs' row.
constexpr int kWhite = 200;  // Each channel at least: the name's letters.
constexpr int kWhiteLeast = 12;
constexpr int kLeftGapLeast = 12;
constexpr int kLeftGapMost = 19;
constexpr int kRightGapLeast = 10;
constexpr int kRightGapMost = 17;
// A plate is over the box when its middle is within the box's sides, this much wider, and above a third down it.
constexpr int kSidePlay = 40;

struct Orb {
    int x = 0;
    int y = 0;
    int ring = 0;          // Angles ring-coloured.
    bool coloured = false; // Inside blue or red, not grey.
};

// The ring: bronze, or cream where it catches the light. Warm (red over green over blue) but not red: green at least 0.6
// of red (an orb's red inside is under 0.5).
bool ringColour(const std::uint8_t* p) {
    const int b = p[0];
    const int g = p[1];
    const int r = p[2];
    return r >= 90 && r - b >= 25 && r - b <= 120 && r >= g && g + 8 >= b && g * 10 >= r * 6;
}

} // namespace

StatsArea plateArea(StatsArea over, int width, int height) {
    const int h = over.y1 - over.y0;
    const int middle = (over.x0 + over.x1) / 2;
    const StatsArea area{std::max(middle - 320, 0), std::max(over.y0 - std::max(180, h * 6 / 10), 0),
                         std::min(middle + 320, width), std::min(over.y0 + std::max(20, h / 3) + kReach, height)};
    return area.x1 > area.x0 && area.y1 > area.y0 ? area : StatsArea{};
}

PlateFound findPlate(const ColourPart& part, StatsArea over, std::span<const UiPiece> pieces,
                     std::vector<std::uint8_t>& work) {
    PlateFound plate;
    const int x0 = part.area.x0;
    const int y0 = part.area.y0;
    const int w = part.area.x1 - x0;
    const int h = part.area.y1 - y0;
    if (w <= 2 * kReach || h <= 2 * kReach || part.bgra == nullptr) {
        return plate;
    }
    const auto pixel = [&](int x, int y) { // Part coordinates.
        return part.bgra + static_cast<std::size_t>(y) * part.pitch + static_cast<std::size_t>(x) * 4;
    };
    // The ring's colour, a byte a pixel.
    work.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        std::uint8_t* row = work.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        for (int x = 0; x < w; ++x) {
            row[x] = ringColour(pixel(x, y)) ? 1 : 0;
        }
    }
    const auto ring = [&](int x, int y) { return work[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x] != 0; };
    const auto ringNear = [&](int x, int y, int dx, int dy) { // Ring colour 7 to 9 px that way.
        return ring(x + dx * 7, y + dy * 7) || ring(x + dx * 8, y + dy * 8) || ring(x + dx * 9, y + dy * 9);
    };
    static const auto angles = [] {
        std::array<std::array<int, 4>, kRingAngles> offsets{}; // Near x, y, far x, y.
        for (int k = 0; k < kRingAngles; ++k) {
            const double a = 2.0 * 3.14159265358979 * k / kRingAngles;
            offsets[static_cast<std::size_t>(k)] = {
                static_cast<int>(std::lround(kRingNear * std::cos(a))), static_cast<int>(std::lround(kRingNear * std::sin(a))),
                static_cast<int>(std::lround(kRingFar * std::cos(a))), static_cast<int>(std::lround(kRingFar * std::sin(a)))};
        }
        return offsets;
    }();
    const auto inInterface = [&](int sx, int sy) {
        return std::any_of(pieces.begin(), pieces.end(), [&](const UiPiece& piece) {
            return sx >= piece.x0 && sx < piece.x1 && sy >= piece.y0 && sy < piece.y1;
        });
    };

    std::vector<Orb> orbs;
    for (int y = kReach; y < h - kReach; ++y) {
        for (int x = kReach; x < w - kReach; ++x) {
            // Ring colour left, right, above and below first (cheap), then all round.
            if (!ringNear(x, y, -1, 0) || !ringNear(x, y, 1, 0) || !ringNear(x, y, 0, -1) || !ringNear(x, y, 0, 1)) {
                continue;
            }
            int around = 0;
            for (const auto& o : angles) {
                around += ring(x + o[0], y + o[1]) || ring(x + o[2], y + o[3]) ? 1 : 0;
            }
            if (around < kRingLeast) {
                continue;
            }
            // Its inside: grey (hovered) or coloured (the target), never ring-coloured throughout (a bronze blob).
            int sum[3] = {0, 0, 0};
            int ringInside = 0;
            int count = 0;
            for (int dy = -kInside; dy <= kInside; ++dy) {
                for (int dx = -kInside; dx <= kInside; ++dx) {
                    const std::uint8_t* p = pixel(x + dx, y + dy);
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    ringInside += ring(x + dx, y + dy) ? 1 : 0;
                    ++count;
                }
            }
            const int mb = sum[0] / count;
            const int mg = sum[1] / count;
            const int mr = sum[2] / count;
            const bool coloured = mb - mr >= 40 || (mr - mg >= 60 && mr - mb >= 40);
            if (!coloured && (ringInside * 2 > count || std::max({mr, mg, mb}) - std::min({mr, mg, mb}) > 30)) {
                continue;
            }
            if (inInterface(x0 + x, y0 + y)) {
                continue;
            }
            // One orb is hit at a few neighbouring centres: the best kept.
            const auto near = std::find_if(orbs.begin(), orbs.end(), [&](const Orb& o) {
                return std::abs(o.x - x) <= kOrbApart && std::abs(o.y - y) <= kOrbApart;
            });
            if (near == orbs.end()) {
                orbs.push_back({x, y, around, coloured});
            } else if (around > near->ring) {
                *near = {x, y, around, coloured};
            }
        }
    }
    plate.orbs = static_cast<int>(orbs.size());

    // Pairs on one row with a name between them; of those over the box, the nearest over its middle.
    const int overMiddle = (over.x0 + over.x1) / 2;
    const int overLowest = over.y0 + (over.y1 - over.y0) / 3; // In screen pixels.
    float best = 1e9f;
    for (const Orb& left : orbs) {
        for (const Orb& right : orbs) {
            const int apart = right.x - left.x;
            if (apart < kApartLeast || apart > kApartMost || std::abs(right.y - left.y) > kRowPlay ||
                left.coloured != right.coloured) {
                continue;
            }
            const int y = (left.y + right.y) / 2;
            int whites = 0;
            int first = -1;
            int last = -1;
            for (int x = left.x + kReach + 2; x < right.x - kReach - 1; ++x) {
                for (int dy = -kTextRows; dy <= kTextRows; ++dy) {
                    if (y + dy < 0 || y + dy >= h) {
                        continue;
                    }
                    const std::uint8_t* p = pixel(x, y + dy);
                    if (std::min({p[0], p[1], p[2]}) >= kWhite) {
                        ++whites;
                        first = first < 0 ? x : first;
                        last = x;
                    }
                }
            }
            if (whites < kWhiteLeast || first - left.x < kLeftGapLeast || first - left.x > kLeftGapMost ||
                right.x - last < kRightGapLeast || right.x - last > kRightGapMost) {
                continue;
            }
            const int middle = x0 + (left.x + right.x) / 2;
            const int sy = y0 + y;
            if (middle < over.x0 - kSidePlay || middle > over.x1 + kSidePlay || sy > overLowest) {
                continue;
            }
            // Nearest over the middle, and low: just over the head rather than a farther character's over it.
            const float score = static_cast<float>(std::abs(middle - overMiddle)) +
                                0.5f * static_cast<float>(std::abs(sy - (over.y0 - 30)));
            if (score < best) {
                best = score;
                plate.seen = true;
                plate.selected = left.coloured;
                plate.at = {x0 + left.x - kReach, y0 + y - kReach, x0 + right.x + kReach + 1, y0 + y + kReach + 1};
                plate.text = {x0 + first, y0 + y - kTextRows, x0 + last + 1, y0 + y + kTextRows + 1};
            }
        }
    }
    return plate;
}

} // namespace observex
