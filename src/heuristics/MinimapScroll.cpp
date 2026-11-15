#include "heuristics/MinimapScroll.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

namespace observex {

namespace {

// The window's icons the map does not slide under (screen pixels from its top left; the second from its bottom left):
// the compass button and the resize mark.
constexpr int kTopIcon = 48;
constexpr int kBottomIcon = 30;
// The window's middle is below its top strip, as heuristics/MinimapCone finds the arrow.
constexpr int kTopStrip = 18;

} // namespace

StatsArea MinimapScroll::area(const UiPiece& piece, int width, int height) {
    return {std::max(piece.x0, 0), std::max(piece.y0, 0), std::min(piece.x1, width), std::min(piece.y1, height)};
}

void MinimapScroll::edges(const UiPiece& piece, const ColourPart& part) {
    // From an even screen pixel, so the key's grid and this image's are whole half pixels apart.
    const auto even = [](int value) { return (value + 1) & ~1; };
    const int x0 = even(std::max(piece.x0 + kInset, part.area.x0));
    const int y0 = even(std::max(piece.y0 + kInset, part.area.y0));
    cornerX_ = x0;
    cornerY_ = y0;
    const int x1 = std::min(piece.x1 - kInset, part.area.x1);
    const int y1 = std::min(piece.y1 - kInset, part.area.y1);
    w_ = std::max((x1 - x0) / 2, 0);
    h_ = std::max((y1 - y0) / 2, 0);
    const std::size_t size = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
    grey_.assign(size, 0.0f);
    gx_.assign(size, 0.0f);
    gy_.assign(size, 0.0f);
    for (int y = 0; y < h_; ++y) {
        const std::uint8_t* row0 = part.bgra + static_cast<std::size_t>(y0 + 2 * y - part.area.y0) * part.pitch +
                                   static_cast<std::size_t>(x0 - part.area.x0) * 4;
        const std::uint8_t* row1 = row0 + part.pitch;
        float* out = grey_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w_);
        for (int x = 0; x < w_; ++x) {
            const std::uint8_t* a = row0 + static_cast<std::size_t>(x) * 8;
            const std::uint8_t* b = row1 + static_cast<std::size_t>(x) * 8;
            const int sum = a[0] + 2 * a[1] + a[2] + a[4] + 2 * a[5] + a[6] + b[0] + 2 * b[1] + b[2] + b[4] +
                            2 * b[5] + b[6];
            out[x] = static_cast<float>(sum) * (1.0f / 16.0f);
        }
    }
    for (int y = 1; y + 1 < h_; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(w_);
        for (int x = 1; x + 1 < w_; ++x) {
            const std::size_t i = row + static_cast<std::size_t>(x);
            gx_[i] = grey_[i + 1] - grey_[i - 1];
            gy_[i] = grey_[i + static_cast<std::size_t>(w_)] - grey_[i - static_cast<std::size_t>(w_)];
        }
    }
    middleX_ = (static_cast<float>(piece.x0 + piece.x1) * 0.5f - static_cast<float>(x0)) * 0.5f;
    middleY_ = (static_cast<float>(piece.y0 + piece.y1 + kTopStrip) * 0.5f - static_cast<float>(y0)) * 0.5f;
}

bool MinimapScroll::leftOut(int x, int y) const {
    if (x < 1 || y < 1 || x + 1 >= w_ || y + 1 >= h_) {
        return true;
    }
    const float dx = static_cast<float>(x) - middleX_;
    const float dy = static_cast<float>(y) - middleY_;
    constexpr float kHalfCentre = kCentre * 0.5f;
    if (dx * dx + dy * dy < kHalfCentre * kHalfCentre) {
        return true;
    }
    const int sx = 2 * x + (cornerX_ - pieceX0_); // Screen pixels from the window's top left.
    const int sy = 2 * y + (cornerY_ - pieceY0_);
    const int bottom = pieceY1_ - pieceY0_;
    return (sx < kTopIcon && sy < kTopIcon) || (sx < kBottomIcon && sy > bottom - kBottomIcon);
}

float MinimapScroll::alike(int dx, int dy) const {
    // The correlation of the key's edges with the image's, each image edge at most kStronger times as strong as the
    // key's there: a few points under something bright (the window's buttons shown under the mouse) dropped it from
    // 0.86 to 0.40 when they counted whole (2026-11-15).
    constexpr float kStronger = 2.0f;
    double dot = 0.0;
    double key = 0.0;
    double seen = 0.0;
    int used = 0;
    for (const Point& p : points_) {
        const int x = p.x + dx + shiftX_;
        const int y = p.y + dy + shiftY_;
        if (leftOut(x, y)) {
            continue;
        }
        const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x);
        float cx = gx_[i];
        float cy = gy_[i];
        const float keyStrength = p.gx * p.gx + p.gy * p.gy;
        const float strength = cx * cx + cy * cy;
        const float most = kStronger * kStronger * keyStrength;
        if (strength > most) {
            const float down = std::sqrt(most / strength);
            cx *= down;
            cy *= down;
        }
        dot += static_cast<double>(p.gx * cx + p.gy * cy);
        key += static_cast<double>(keyStrength);
        seen += static_cast<double>(std::min(strength, most));
        ++used;
    }
    if (used * 3 < static_cast<int>(points_.size()) || key <= 0.0 || seen <= 0.0) {
        return -1.0f;
    }
    return static_cast<float>(dot / std::sqrt(key * seen));
}

float MinimapScroll::search(int cx, int cy, int radius, float& bestX, float& bestY) const {
    // Tried slides and their likeness, so none is tried twice when the search moves on.
    struct Tried {
        int x;
        int y;
        float score;
    };
    std::vector<Tried> tried;
    const auto score = [&](int x, int y) {
        for (const Tried& t : tried) {
            if (t.x == x && t.y == y) {
                return t.score;
            }
        }
        const float s = alike(x, y);
        tried.push_back({x, y, s});
        return s;
    };
    int bx = cx;
    int by = cy;
    float best = -2.0f;
    // Round the foretold slide; while the best is on the square's edge, round it again (a few times).
    for (int round = 0; round < 4; ++round) {
        const int ox = bx;
        const int oy = by;
        for (int y = oy - radius; y <= oy + radius; ++y) {
            for (int x = ox - radius; x <= ox + radius; ++x) {
                const float s = score(x, y);
                if (s > best) {
                    best = s;
                    bx = x;
                    by = y;
                }
            }
        }
        if (std::abs(bx - ox) < radius && std::abs(by - oy) < radius) {
            break;
        }
    }
    // Between pixels: a parabola through the best and its neighbours, each way.
    const auto refine = [](float before, float at, float after) {
        const float curve = before - 2.0f * at + after;
        if (before < -1.0f || after < -1.0f || curve >= 0.0f) {
            return 0.0f;
        }
        return std::clamp(0.5f * (before - after) / curve, -0.5f, 0.5f);
    };
    bestX = static_cast<float>(bx) + refine(score(bx - 1, by), best, score(bx + 1, by));
    bestY = static_cast<float>(by) + refine(score(bx, by - 1), best, score(bx, by + 1));
    return best;
}

bool MinimapScroll::takeKey() {
    std::vector<Point> points;
    for (int cy = 0; cy < h_; cy += kCell) {
        for (int cx = 0; cx < w_; cx += kCell) {
            Point best;
            float strongest = kMinEdge * kMinEdge;
            for (int y = cy; y < std::min(cy + kCell, h_); ++y) {
                for (int x = cx; x < std::min(cx + kCell, w_); ++x) {
                    if (leftOut(x, y)) {
                        continue;
                    }
                    const std::size_t i =
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x);
                    const float strength = gx_[i] * gx_[i] + gy_[i] * gy_[i];
                    if (strength > strongest) {
                        strongest = strength;
                        best = {x, y, gx_[i], gy_[i]};
                    }
                }
            }
            if (strongest > kMinEdge * kMinEdge) {
                points.push_back(best);
            }
        }
    }
    if (static_cast<int>(points.size()) < kMinPoints) {
        return false;
    }
    points_ = std::move(points);
    return true;
}

MinimapScroll::MinimapScroll()
    : run_(static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count() >> 8) & 0x3FFFFFFFu) {
}

MinimapRead MinimapScroll::follow(const UiPiece* piece, const ColourPart* part) {
    MinimapRead out;
    out.run = run_;
    if (piece == nullptr || part == nullptr) {
        hidden_ = true;
        lastMatch_ = 0.0f;
        return out;
    }
    pieceX0_ = piece->x0;
    pieceY0_ = piece->y0;
    pieceY1_ = piece->y1;
    edges(*piece, *part);
    // A new run: from this image, as it is.
    const auto start = [&]() {
        if (!takeKey()) {
            points_.clear(); // Tried again on the next image.
            return out;
        }
        ++run_;
        runPiece_ = *piece;
        keyCornerX_ = cornerX_;
        keyCornerY_ = cornerY_;
        shiftX_ = shiftY_ = 0;
        keyX_ = keyY_ = lastX_ = lastY_ = speedX_ = speedY_ = 0.0f;
        misses_ = 0;
        hidden_ = false;
        lastMatch_ = 1.0f;
        out.seen = true;
        out.run = run_;
        out.match = 1.0f;
        return out;
    };
    if (points_.empty() || std::abs(piece->x0 - runPiece_.x0) > kMoved || std::abs(piece->y0 - runPiece_.y0) > kMoved ||
        std::abs(piece->x1 - runPiece_.x1) > kMoved || std::abs(piece->y1 - runPiece_.y1) > kMoved) {
        return start();
    }
    shiftX_ = (keyCornerX_ - cornerX_) / 2;
    shiftY_ = (keyCornerY_ - cornerY_) / 2;
    const bool wide = hidden_ || misses_ > 0;
    const float foreX = wide ? lastX_ : lastX_ + speedX_;
    const float foreY = wide ? lastY_ : lastY_ + speedY_;
    float bx = 0.0f;
    float by = 0.0f;
    const float match = search(static_cast<int>(std::lround(foreX)), static_cast<int>(std::lround(foreY)),
                               wide ? kWideSearch : kSearch, bx, by);
    lastMatch_ = match;
    out.match = std::max(match, 0.0f);
    if (match < kMatched) {
        ++misses_;
        if (hidden_ || misses_ >= kMisses) {
            return start();
        }
        return out;
    }
    speedX_ = wide ? 0.0f : bx - lastX_;
    speedY_ = wide ? 0.0f : by - lastY_;
    lastX_ = bx;
    lastY_ = by;
    misses_ = 0;
    hidden_ = false;
    out.seen = true;
    out.x = keyX_ + 2.0f * bx;
    out.y = keyY_ + 2.0f * by;
    // Slid far from the key: this image is the key from now on (where the map has slid in the run kept to the
    // fraction of a pixel, so walking far adds up only the key changes' small errors). Not when only less alike (the
    // world seen through the see-through map changed as the camera turned): a key taken then holds more of the world,
    // which the follow then went after (2026-11-15: 6 pixels off standing still).
    if (std::abs(bx) > static_cast<float>(kRekey) || std::abs(by) > static_cast<float>(kRekey)) {
        if (takeKey()) {
            keyX_ = out.x;
            keyY_ = out.y;
            keyCornerX_ = cornerX_;
            keyCornerY_ = cornerY_;
            shiftX_ = shiftY_ = 0;
            lastX_ = lastY_ = 0.0f;
        }
    }
    return out;
}

} // namespace observex
