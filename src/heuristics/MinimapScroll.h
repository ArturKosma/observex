#pragma once

#include "core/GameStats.h"
#include "core/UiLayout.h"

#include <cstdint>
#include <vector>

namespace observex {

// Follows how far the minimap's map slides under the own arrow (the user asked, 2026-11-15: where the character is
// between two /loc, so a single /loc is enough while walking). The user's minimap is north-up: only the cone turns
// with the camera, the map slides only when the character walks, so how far it slid times the minimap's scale (game
// units a pixel, app/GeodataView learns it from two /loc) is how far the character walked.
//
// The map is compared, at half size, by its edges (how the grey changes, so the world seen through the see-through
// minimap and the time of day matter little), with a picture of it kept as the key: on each image the slide that
// best lines up the key's strongest edges (one a cell of 8 x 8, up to ~900) with the image's is searched round the
// one foretold by the last ones, and refined between pixels. The own arrow, its cone and the ring round them (they
// turn with the camera; kCentre round the middle) and the window's two icons are left out. The slide is measured
// against the key, not image to image, so its errors do not add up while walking; the key is taken again once the map
// has slid kRekey half pixels. A run (MinimapRead::run) ends when the map cannot be followed: unlike the key on
// kMisses images in a row while seen (covered, zoomed, a teleport), or unlike it on the first image it is seen again
// after being hidden (the character walked too far meanwhile), or the window resized.
// Use one from one thread.
class MinimapScroll {
public:
    static constexpr int kInset = 4;           // Pixels left out inside the window's edges (its frame).
    static constexpr float kCentre = 88.0f;    // Round the own arrow (screen pixels): the cone and its ring turn.
    static constexpr int kCell = 8;            // One key point a cell this many half pixels across,
    static constexpr float kMinEdge = 6.0f;    // where the grey changes by at least this much (two half pixels).
    static constexpr int kMinPoints = 60;      // Fewer key points (a map with no features, e.g. the sea): not followed.
    static constexpr float kMatched = 0.55f;   // Alike at least this much (correlation of the edges): followed.
    static constexpr int kSearch = 3;          // Half pixels searched round the foretold slide,
    static constexpr int kWideSearch = 14;     // and after a miss or when seen again.
    static constexpr int kRekey = 20;          // Half pixels slid from the key: the key is taken again.
    static constexpr int kMisses = 6;          // Images in a row not alike while seen (0.6 s at 10 a second): a new run.
    // The window found this far (a side, pixels) from where it was when the run began: it was moved or resized (the
    // map then sits elsewhere in it): a new run. Nearer, it is where it was, found a few pixels off (by its frame or by
    // its compass, 2026-11-15): the map is measured on the screen, not from the window's corner.
    static constexpr int kMoved = 16;

    // Its first run's number from a clock, so a new one (the stats DLL reloaded) never repeats an old one's runs.
    MinimapScroll();

    // The part of the screen it reads: the minimap's window `piece`.
    static StatsArea area(const UiPiece& piece, int width, int height);
    // Follows the map on one image: `piece` the minimap's window (null: not found), `part` its pixels as area() asked
    // (null: not read).
    MinimapRead follow(const UiPiece* piece, const ColourPart* part);

    // How alike the key and the last image were, and how many key points the key has (the bench's, the why's).
    float lastMatch() const { return lastMatch_; }
    int points() const { return static_cast<int>(points_.size()); }

private:
    struct Point {
        int x = 0; // Half pixels in the window, from its inset corner.
        int y = 0;
        float gx = 0.0f;
        float gy = 0.0f;
    };
    // This image's half-size grey's edges (gx_, gy_, width w_ x h_, from screen pixel cornerX_, cornerY_, both even)
    // from `part`, the window at `piece`.
    void edges(const UiPiece& piece, const ColourPart& part);
    // Whether half pixel (x, y) is left out (the arrow's middle, the icons, the edges).
    bool leftOut(int x, int y) const;
    // How alike the key's points are to this image's edges slid by (dx, dy) half pixels (on the screen, from where the
    // key was taken); -1 if too few compare.
    float alike(int dx, int dy) const;
    // The best slide within `radius` of (cx, cy) (each tried once), refined between pixels; its likeness.
    float search(int cx, int cy, int radius, float& bestX, float& bestY) const;
    // Takes this image as the key (its points); false if it has too few.
    bool takeKey();

    int w_ = 0; // This image's half-size edges.
    int h_ = 0;
    float middleX_ = 0.0f; // The own arrow, in half pixels.
    float middleY_ = 0.0f;
    std::vector<float> grey_;
    std::vector<float> gx_;
    std::vector<float> gy_;
    std::vector<Point> points_; // The key's.
    int cornerX_ = 0; // This image's half-size grid's first pixel on the screen (even),
    int cornerY_ = 0;
    int keyCornerX_ = 0; // and the key's.
    int keyCornerY_ = 0;
    int shiftX_ = 0; // The key's grid from this image's, in half pixels.
    int shiftY_ = 0;
    int pieceX0_ = 0; // This image's window (screen pixels).
    int pieceY0_ = 0;
    int pieceY1_ = 0;
    UiPiece runPiece_{}; // The window where the run began.
    float keyX_ = 0.0f; // Where the map had slid in the run when the key was taken (screen pixels).
    float keyY_ = 0.0f;
    float lastX_ = 0.0f; // The last slide from the key (half pixels) and its change from the image before.
    float lastY_ = 0.0f;
    float speedX_ = 0.0f;
    float speedY_ = 0.0f;
    std::uint32_t run_ = 0;
    int misses_ = 0;
    bool hidden_ = false; // Not seen since the last image followed: search wide.
    float lastMatch_ = 0.0f;
};

} // namespace observex
