#pragma once

#include "core/Detection.h"
#include "core/Ground.h"
#include "core/Heuristics.h"

#include <atomic>
#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// A saved clip's clip.txt read back whole (engine/ClipRecorder writes it; core/ClipLines and core/LiveLines are its
// lines), and what the clip screen draws over the video from it: the guesses (the user asked, 2026-11-11: switch
// between the guesses recorded with the clip and what changed heuristics make of the same seconds).

// One look of the recogniser in the clip.
struct ClipLook {
    unsigned long long image = 0; // The screen image it looked at,
    double timeMs = 0.0;          // which the screen showed then.
    bool looked = false;          // It has its look line (what the model found); always, in a clip that is whole.
    std::vector<Detection> found; // What the model found, surest first, down to 10% sure, with kinds.
    bool showed = false;          // It has its shown line: when the app showed what it made of it,
    double doneMs = 0.0;
    std::vector<Tracked> shown;   // and what it showed from then on.
};

struct ClipNote {
    double timeMs = 0.0;
    std::string text;
};

// The boxes the app's snapper put onto one screen image (engine/BoxSnapper, since 2026-11-11).
struct ClipSnap {
    unsigned long long image = 0; // The screen image,
    double timeMs = 0.0;          // which the screen showed then.
    double doneMs = 0.0;          // When the app had them; it showed them from then on,
    double leadMs = 0.0;          // each box this far ahead of where it is on the image, along its speed.
    std::vector<Tracked> things;
    // How it got them (its finds line, since the evening of 2026-11-11): the look it took in just before (its image; 0:
    // none) and each search it made.
    bool hasFinds = false;
    unsigned long long lookImage = 0;
    std::vector<SnapFind> finds;
};

// The grid the Show flag Ground drew over one screen image live (since 2026-11-12; core/ClipLines' ground line).
struct ClipGroundLine {
    unsigned long long image = 0;    // The screen image it was drawn over,
    double timeMs = 0.0;             // which the screen showed then.
    unsigned long long followed = 0; // The image the ground finder followed last (0: none).
    GroundCamera camera;             // As drawn (its timeMs: timeMs); not known: nothing drawn from then on.
};

struct ClipText {
    std::vector<std::string> kinds; // The model's kinds by number, e.g. "monster" (from the header); empty: none.
    std::vector<double> frameMs;    // frameMs[n - 1]: when the screen showed the image of video frame n (or a moment
                                    // later, engine/ClipRecorder), and
    std::vector<unsigned long long> frameImage; // which image it was.
    std::vector<ClipLook> looks;    // In the order they were made.
    std::vector<ClipSnap> snaps;    // Likewise; none before 2026-11-11 or without the snapper.
    std::vector<ClipNote> notes;
    std::vector<ClipGroundLine> grounds; // In time order; none before 2026-11-12.
};

// Skips the lines it does not know and malformed ones.
ClipText readClipText(std::string_view text);

// The header line "# Kinds: 0 monster, 1 player, ..." into `kinds` (replaced). False if `line` is not one.
bool readKindNames(std::string_view line, std::vector<std::string>& kinds);

// The grid the app drew live when the screen showed the image of `timeMs` (Recorded's ground): the last line at or
// before it (the last of those at the same time); not known before the first or where nothing was drawn.
GroundCamera recordedGround(std::span<const ClipGroundLine> lines, double timeMs);
// The screen images the live ground finder followed, in order: the ones following it again over the clip (Today's
// ground) follows, so it does what live did. Empty for a clip without ground lines.
std::vector<unsigned long long> followedImages(const ClipText& clip);

// What a guesser shows over a clip: from each time on, a set of things, as the live view did (it showed what the
// latest look done by then made of its image).
class Guesses {
public:
    // A set shown from `fromMs` on (not before the last one's time).
    void add(double fromMs, std::span<const Tracked> things);
    // What it shows at `timeMs`: the last set from at or before it; none before the first.
    std::span<const Tracked> at(double timeMs) const;
    std::size_t sets() const { return from_.size(); }
    double fromMs(std::size_t set) const { return from_[set]; }
    std::span<const Tracked> set(std::size_t set) const;

    struct Summary {
        double things = 0.0;  // Shown at once, on average over the sets.
        double changes = 0.0; // Boxes appearing or vanishing a second: things whose id comes or goes between sets.
    };
    // Over the sets from `fromMs` on (settledMs: where a replay has caught up, so guessers compare on the same
    // seconds).
    Summary summary(double fromMs) const;

private:
    std::vector<double> from_;
    std::vector<std::size_t> first_; // Set i is things_[first_[i]] up to the next set's first.
    std::vector<Tracked> things_;
};

// What the app showed live: exactly the boxes the user saw. From the shown lines, and from the first snapped line on,
// those (the app shows the snapper's boxes while it runs).
Guesses recordedGuesses(const ClipText& clip);

// One screen image a replay of a clip's snapping puts the boxes onto, as the app's snapper did live (snapSteps).
struct SnapStep {
    unsigned long long image = 0;     // The screen image,
    double timeMs = 0.0;              // which the screen showed then,
    double doneMs = 0.0;              // and from when its boxes are shown.
    unsigned long long lookImage = 0; // The look taken in just before (its image; 0: none).
    const std::vector<SnapFind>* finds = nullptr; // The searches made live (null: not recorded).
};
// The images a replay of the clip's snapping goes through, in order. With snapped lines, the ones the snapper put the
// boxes onto live, at their times, taking in the looks it took in (their finds lines, since the evening of
// 2026-11-11; before, the newest look done by the image's time: live it took the newest one when it began on the
// image, a moment later). Without (before 2026-11-11, or with the snapper off), each video frame's new image, its
// boxes kReplayLateMs after it, taking in the newest look done by then.
std::vector<SnapStep> snapSteps(const ClipText& clip);
inline constexpr double kReplayLateMs = 2.0;

// Where a replay's grey images come from, made as the app's snapper makes them (engine/GreyMaker: the newest kKept
// images kept, only the parts read readable): engine/ClipSnapper's from the clip's video, the tests' made up.
class SnapImages {
public:
    static constexpr int kKept = 8; // As engine/GreyMaker keeps them.

    virtual ~SnapImages() = default;
    // Makes screen image `image` (snapSteps' order): the newest kept, in place of the oldest. The video lacks some
    // images (and has none before its first frame): the frame before (or the first) stands for those.
    virtual void make(unsigned long long image) = 0;
    // Makes `areas` of `image` readable once wait() is called (none if it is not kept any more).
    virtual void read(unsigned long long image, std::span<const GreyArea> areas) = 0;
    virtual void wait() = 0;
    // Image `image` once read and while kept (GreyView::image set), else null.
    virtual const GreyView* find(unsigned long long image) const = 0;
};

// Today's boxes snapped as the app's snapper does live (engine/BoxSnapper): `today`'s sets (heuristicGuesses of
// `clip`) put onto the images `images` makes at each of the clip's snapSteps by `snapping` (today's; cleared first),
// each step's set shown from its done time, and before the first step today's sets. A search the same as one recorded
// live (core/Heuristics' SnapFind) takes that one's result instead of the pixels (the video is compressed, and lacks
// images), so with the heuristics unchanged a clip recorded since the evening of 2026-11-11 replays as exactly what
// was shown live (recordedGuesses). Stops early, with what it has, once `stop` is asked; `progress` (if set) goes
// from 0 to 1.
Guesses snapGuesses(const ClipText& clip, const Guesses& today, Snapping& snapping, SnapImages& images,
                    std::stop_token stop = {}, std::atomic<float>* progress = nullptr);

// How two guessers differ over a clip, compared at each set of the first (e.g. Recorded against Today's).
struct GuessesDifference {
    std::size_t sets = 0;        // Sets of the first compared (from skipMs after its first one on);
    std::size_t same = 0;        // of them, ones the second draws the same then: the same boxes, to the pixel, with
                                 // the same tags (kind, "?" and percent), as the clip screen draws them.
    std::size_t matched = 0;     // Boxes both draw (paired by how much they overlap),
    std::size_t onlyFirst = 0;   // only the first draws,
    std::size_t onlySecond = 0;  // only the second draws.
    std::size_t tagDiffers = 0;  // Paired, but tagged differently,
    std::size_t moved = 0;       // tagged the same, but a pixel or more apart.
    std::vector<double> firstDiffering; // When the first few sets that differ start (up to kExamples),
    double lastDiffering = -1.0;        // and the last one (-1: none differs).
    static constexpr std::size_t kExamples = 5;
};
// `unknownKind`: the "can't tell" kind's place in the model's kinds (-1: none), for the "?" (core/Detection's
// cantTellKind).
GuessesDifference compareGuesses(const Guesses& first, const Guesses& second, int unknownKind, double fromMs);

// How well a guesser's boxes sit on the things on the screen (the user asked, 2026-11-11: the bot will click a box's
// middle, so a box behind its thing in a camera turn makes it miss): at each video frame whose own image the
// recogniser looked at (from `fromMs` on), the guesser's boxes then against what the model found on that very image
// at least kTruthSure sure, paired nearest first (middles less than two of the larger side apart, of a similar size).
struct BoxPrecision {
    struct Part {
        std::size_t boxes = 0;    // Paired with a box the model found;
        double meanOff = 0.0;     // how far their middles were from its middle, in pixels, on average,
        double mostOff = 0.0;     // and at most for nine in ten;
        std::size_t inMiddle = 0; // middles in the middle half of the model's box (a click there hits it well),
        std::size_t outside = 0;  // and outside it (a click there misses).
    };
    std::size_t frames = 0; // Frames whose image was looked at.
    Part all;
    Part moving;              // Of the boxes of things moving fast on the screen (a camera turn), by the guesser's
                              // speed: kMovingSpeed or faster.
    std::size_t unpaired = 0; // Boxes paired with nothing the model found.
    std::size_t missed = 0;   // Things the model found (at least kTruthSure sure) with no box paired: shown nowhere
    std::size_t truths = 0;   // near (of that many).
    static constexpr float kTruthSure = 0.5f;
    static constexpr float kMovingSpeed = 0.8f; // Pixels a millisecond: 10 pixels in the 12 ms a set comes late.
};
BoxPrecision boxPrecision(const ClipText& clip, const Guesses& guesses, double fromMs);

// The same at chosen moments: what the guesser shows at `timeMs` against what the model found on `image` (moments
// whose image was not looked at are skipped; `frames` counts the others). E.g. a box snapped onto one image, judged on
// the next (observex_bench --clip-guesses --snap).
struct BoxMoment {
    double timeMs = 0.0;
    unsigned long long image = 0;
};
BoxPrecision boxPrecision(const ClipText& clip, const Guesses& guesses, std::span<const BoxMoment> moments);

// From when guessers are compared on a clip: its first frame, or, for a clip without looks before it (saved before
// 2026-11-11, see core/Recordings' kClipWarmUpSeconds), kSettleMs after its first look, where a replay has caught up
// a little at least (the tracker shows a thing from its third look on, holds it up to kHoldMs, and its sureness and
// kind follow over kSmoothSureMs and kSmoothKindMs).
double settledMs(const ClipText& clip);
inline constexpr double kSettleMs = 500.0;

// The recorded looks (what the model found then) through `heuristics` (today's: core/Heuristics, cleared first) from
// the clip's start, each set shown from when the app showed the recorded look's (so it lines up with recordedGuesses,
// and only the heuristics differ). The first looks show less: the tracker starts with nothing followed, and shows a
// new thing from its third look.
Guesses heuristicGuesses(const ClipText& clip, Heuristics& heuristics);

} // namespace observex
