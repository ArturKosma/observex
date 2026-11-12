#pragma once

#include "core/Detection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace observex {

// What observex makes of the model's looks before it shows them (the user's word, 2026-11-11: heuristics). Live
// recognition (engine/Recognizer) and the clip screen's "Today's heuristics" (core/ClipGuesses, on a saved clip's
// recorded looks) both go through one, so a clip shows what observing would show now, on exactly what the model saw
// then.
//
// This is their fixed side: what goes in (the model's Detections) and what comes out (Tracked things). The rules
// themselves are in src/heuristics (heuristics/Rules: today, the things at least kMinSure sure, followed from look to
// look by the tracker), built into observex_heuristics.dll, which the app loads from a copy and loads again whenever
// it is rebuilt (engine/HeuristicsLibrary; the user asked, 2026-11-11: to see a heuristics fix on an open clip without
// closing the app). So a new rule needs only that DLL rebuilt; a change here (or to Detection, Tracked) needs the app
// rebuilt too. Use one from one thread.
class Heuristics {
public:
    // The least sure a thing must be for the recogniser to hand it over when nothing else asks for less (with the clip
    // buffer on, always while observing, it hands everything at least LiveLog::kMinSure sure, as a clip records).
    static constexpr float kMinSure = 0.3f;

    virtual ~Heuristics() = default;

    // Takes what the model found on the image the screen showed at `timeMs` (surest first; times must not go back),
    // and writes up to `max` of the things to show into `out`, surest first. Returns how many.
    virtual int update(double timeMs, std::span<const Detection> found, Tracked* out, int max) = 0;

    // Forgets everything followed so far, e.g. before a clip is replayed from its start.
    virtual void clear() = 0;
};

// One level of a GreyView: `height` rows of `width` grey pixels, `stride` bytes from one row to the next.
struct GreyLevel {
    const std::uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    std::ptrdiff_t stride = 0;
};

// A grey copy of a screen image and its halvings, for heuristics that look at the screen itself (Snapping). Level 0
// is the image's size, each next one half as wide and high (rounded down), each pixel the rounded mean of the 2 x 2
// above it; grey is (blue * 29 + green * 150 + red * 77) >> 8 of the BGRA pixels. It only points at the pixels: whoever
// hands one over keeps them.
struct GreyView {
    static constexpr int kLevels = 4; // Full size, half, quarter, eighth.
    std::array<GreyLevel, kLevels> levels{};
    std::uint64_t image = 0; // The screen image's number (ImageStamp::number), as looks and clips count them; 0: none.

    int width(int level) const { return levels[static_cast<std::size_t>(level)].width; }
    int height(int level) const { return levels[static_cast<std::size_t>(level)].height; }
    const std::uint8_t* row(int level, int y) const {
        const GreyLevel& at = levels[static_cast<std::size_t>(level)];
        return at.pixels + at.stride * y;
    }
};

// A part of a screen image: its pixels from (x0, y0) to just before (x1, y1), which may reach past the image's edges.
struct GreyArea {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    // The same part on `level` of a GreyView `width` x `height` there: halved `level` times (rounded outwards), a pixel
    // wider each way, and cut to the level. Empty (x0 >= x1 or y0 >= y1) if it is off the image.
    GreyArea onLevel(int level, int width, int height) const {
        const int scale = 1 << level;
        const auto down = [scale](int value) { return value >= 0 ? value / scale : -((scale - 1 - value) / scale); };
        const auto clamp = [](int value, int high) { return value < 0 ? 0 : value > high ? high : value; };
        return {clamp(down(x0) - 1, width), clamp(down(y0) - 1, height), clamp(down(x1 + scale - 1) + 1, width),
                clamp(down(y1 + scale - 1) + 1, height)};
    }
};

// One search Snapping::snap made for a thing on an image, and what it found: a clip records them (engine/ClipRecorder;
// the user asked, 2026-11-11: a new clip's Today must show exactly what was shown live, so a heuristics change is all
// that changes it). A clip's video is not what the snapper searched: it is compressed, and it lacks the images the
// video encoder had no time for. So a replay (Snapping::replay) takes a recorded search's result for a search that is
// the same: the same way of searching (`finder`), for a thing that looks the same (learned on the same image at the same
// box), at the same place and reach. Any other searches the video's pixels.
struct SnapFind {
    std::uint32_t finder = 0;       // How it searched (heuristics/Snapper's kFinder): changed, recorded ones go unused.
    std::uint32_t id = 0;           // The thing (Tracked::id); not part of what makes two searches the same.
    std::uint64_t learnedImage = 0; // How the thing looked: learned on this image (0: nothing learned of it yet),
    float learnedX = 0.0f;          // at this box (where the look found it).
    float learnedY = 0.0f;
    float learnedWidth = 0.0f;
    float learnedHeight = 0.0f;
    float guessX = 0.0f; // Where its box's top left was looked for,
    float guessY = 0.0f;
    float reach = 0.0f; // and how far from there both ways.
    bool found = false; // Found (a match of at least the finder's least), where (its box's top left),
    float x = 0.0f;
    float y = 0.0f;
    float match = 0.0f; // and how well the best place matched (-1 to 1; also when not found; -2: not searched).

    // The same search: one's result stands for the other's. Within kSameWithin pixels: a search looks at least 20
    // pixels around its place, so one that close finds the same best place; and a replay differs from live a little at
    // first (it starts with nothing followed, 5 s before the clip), and in the last digits (its times are counted from
    // another moment), and taking live's results it is soon the same.
    static constexpr float kSameWithin = 1.0f;
    bool sameSearch(const SnapFind& other) const {
        const auto within = [](float a, float b) { return a - b <= kSameWithin && b - a <= kSameWithin; };
        return finder == other.finder && learnedImage == other.learnedImage && within(learnedX, other.learnedX) &&
               within(learnedY, other.learnedY) && within(learnedWidth, other.learnedWidth) &&
               within(learnedHeight, other.learnedHeight) && within(guessX, other.guessX) && within(guessY, other.guessY) &&
               within(reach, other.reach);
    }
};

// What observex makes of the screen images after the one the model looked at (the user asked, 2026-11-11: boxes
// snapped to the characters, since in a fast camera turn boxes foretold from the looks still miss): the things the
// heuristics show (Heuristics) put onto where they are on each newer image, as soon as it comes. Live
// (engine/BoxSnapper) on every new screen image, which the screen shows every few milliseconds, while a look takes
// about 10. Like Heuristics, its rules are in src/heuristics (heuristics/SnapRules), in observex_heuristics.dll. Use one
// from one thread.
class Snapping {
public:
    virtual ~Snapping() = default;

    // After a look: what the heuristics showed then (`shown`, as Heuristics::update wrote it), made from the image the
    // screen showed at `lookMs`; `image` is that image, or null if it is not at hand any more. Looks come in order,
    // a few images late (lookMs is before the newest snap's time).
    virtual void looked(double lookMs, const GreyView* image, std::span<const Tracked> shown) = 0;

    // A new screen image, which the screen showed at `timeMs` (times must not go back): writes up to `max` of the
    // things to show into `out`, surest first, each where it is on this image moved on leadMs() along its speed.
    // Returns how many.
    virtual int snap(double timeMs, const GreyView& image, Tracked* out, int max) = 0;

    // Where the next calls read their images, so only those parts need be made readable (live, reading whole images
    // back from the GPU slowed the model's looks by 1.5 ms, 2026-11-11); pixels outside them can be anything. Each
    // writes up to `max` parts into `out` and returns how many. learnAreas: the parts of the look's image that
    // looked(lookMs, image, `shown`) reads. snapAreas: the parts of its image that snap(`timeMs`, ...) reads when
    // called next, or with `look` after looked(`lookMs`, image, `shown`).
    virtual int learnAreas(std::span<const Tracked> shown, GreyArea* out, int max) = 0;
    virtual int snapAreas(double timeMs, bool look, double lookMs, std::span<const Tracked> shown, GreyArea* out,
                          int max) = 0;

    // How far ahead of their image snap() puts the boxes, in milliseconds: about when they are on the screen.
    virtual double leadMs() const = 0;

    // The searches the last snap() made, in order, for a clip to record. Valid until the next call.
    virtual std::span<const SnapFind> finds() const = 0;
    // For a clip's replay, before each snap(): the searches recorded live on that image. A search snap() makes that is
    // the same as one of them (SnapFind::sameSearch) takes its result instead of searching the image. Copied.
    virtual void replay(std::span<const SnapFind> recorded) = 0;

    // Forgets everything, e.g. before a clip is replayed from its start.
    virtual void clear() = 0;
};

} // namespace observex
