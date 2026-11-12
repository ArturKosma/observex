#pragma once

#include "core/Heuristics.h"
#include "heuristics/Snapper.h"

#include <array>
#include <cstdint>
#include <vector>

namespace observex {

// Today's snapping (core/Heuristics' Snapping): the things the heuristics show, put onto each newer screen image by
// how they looked where the model found them (heuristics/Snapper).
//
// - On each look it learns how each thing found then looks, on the look's image at the box the model found (the
//   tracker hands boxes out Tracker::kLeadMs ahead of that), and takes over the look's things: their ids, sureness,
//   kinds and sizes, in its order (surest first). Things the look no longer shows go.
// - On each new image it looks for each thing near where its speed takes it from where it was last put: on the image
//   before, or by the look (one the look did not find stays where it was snapped: the tracker's box for it is only
//   its guess). Found, it is there to a fraction of a pixel; not, it is shown where its speed takes it (and held), on
//   by at most Tracker::kMaxAheadMs from each image to the next (so over a gap with no new image, when the game stood
//   still, only that). Its speed is a line through where it was snapped over the last kSpeedMs (at least its last two
//   images), started again where it jumped off it (Tracker::kJumpPixels); until it was snapped twice, the tracker's.
//   Also started again where a look puts one it could not find on the last image it looked on, with the tracker's
//   speed meanwhile: lost as a camera turn sped up, a thing was looked for at the speed it had then and never found
//   again, its box sliding on behind it between looks (the user, frame 159 of the 22:10 clip of 2026-11-11: the
//   tracker had it at 7-9 px/ms, the snapping at 1.1, up to 100 px behind). On the 22:37 clip (the game limited to
//   120 images a second, so its video has nearly all of them; each set against the model's boxes on the image it was
//   snapped onto, kLeadMs taken back): boxes on nothing 552 -> 519, the model's things without a box 550 -> 506,
//   boxes moving 0.8 px/ms or faster 817 -> 881 of them on their things and 2.4 -> 2.7 px off; in its turns held
//   boxes 2 px/ms or more off the others' speed 220 -> 148.
// - After most of the things it expected to find were lost on one image at once (the camera jumped: the game stood
//   still, then caught up), it looks for them kJumpReach around on the next image, and takes what it finds only where
//   enough say alike how far the scene went (kJumpSupport), holding the others there; until then their boxes stayed
//   where they had been, up to 250 pixels off, until the next look (the user, frame 178 of the 22:37 clip of
//   2026-11-11). On that clip it looked so on 14 of 1795 images; at frame 178 the five small things' boxes went from
//   114-250 pixels off their things to 1-26. On the other four clips of 2026-11-11 as before or a little better.
//   Until the evening of 2026-11-11 at most Tracker::kMaxAheadMs in all: a box held in a camera turn stood still on
//   the screen after 20 ms while everything moved on (the user: one "stays on the same place on the screen" for five
//   frames, 2026-11-11). On the four clips of 2026-11-11 (--clip-guesses) the boxes as near their things as before
//   (fast ones 0.1 px nearer) and as many on nothing the model found (0.3% fewer).
// - Each box is handed out kLeadMs ahead along its speed: about when it is on the screen. Not one found neither by the
//   looks nor on the images for kGoneMs, and not one the look did not find snapped onto another's box (the same
//   thing, lost by the tracker and followed again as new).
// - It says which parts of the images it reads (learnAreas, snapAreas: the looks' boxes, and around where each thing
//   is looked for), by the same steps as looked() and snap(), so only those are read back from the GPU.
// - It notes each search it makes and what it found (finds(), which a clip records), and in a clip's replay takes the
//   recorded result of a search that is the same (replay(), core/Heuristics' SnapFind) instead of searching the video's
//   pixels: so a new clip's Today is exactly what was shown live, until the heuristics change.
// On the 12:10 clip of 2026-11-11 (observex_bench --clip-guesses --snap, Today's sets snapped onto its video frames):
// on the image snapped, boxes moving 0.8 px/ms or faster were 2.2 pixels off the model's own boxes there on average
// and 2.6% of their middles outside them (Today's, as shown: 11.1 px, 24%); all boxes 0.8 px. But a box is shown
// until the next image's is made, and on that next image fast ones are 8.7 px off, 15.1% outside: the game moves
// things unevenly from image to image (moved on by exactly the time to the next image, which is not known live, still
// 7.2 px, 11.5%). About 0.4 ms an image for 7 boxes; the parts it reads were 7.4% of the images. The 19:08 clip
// (faster turns, and the game standing still 50 ms, then the camera jumping 120-160 pixels): 6.4 px, 12.7% on the
// image snapped (Today's 23.3 px, 47%), 17.2 px, 37% on the next. Until the evening of 2026-11-11 (before kGoneMs,
// the jumps and holding its own place) that clip's were 8.3 px, 14.6% and 19.6 px, 37%, boxes on nothing the model
// found 1141 -> 637, and on the 12:10 one 570 -> 427.
class SnapRules final : public Snapping {
public:
    // How far ahead of their image the boxes are put: until the next image, about. On that clip (150 images a second,
    // 6.7 ms apart on average) 6 put fast boxes nearest their things on the next image (3: 11.0 px off, 4.5: 9.2,
    // 7.5: 10.4, 9: 12.7).
    static constexpr double kLeadMs = 6.0;
    // How far from where its speed takes it a thing is looked for: kReach pixels both ways, and kReachShare more of
    // each pixel its speed took it since it was put there (the game moves things unevenly: in a steady camera turn
    // an orc moved 51, 5, 51, 15, 30 pixels on images 8, 4, 8, 4, 8 ms apart).
    static constexpr float kReach = 20.0f;
    static constexpr float kReachShare = 1.0f;
    // Its speed's line: on that clip 12, 20, 30 and 40 ms (and the tracker's speed, from the looks) were within half a
    // pixel of each other on the next image; 30 was a little best.
    static constexpr double kSpeedMs = 30.0;
    // A thing found neither by the looks nor on the images for this long is not handed out (the tracker may still
    // hold it, up to 150 ms): it is gone, or not where it is held. On the 19:08 clip of 2026-11-11 boxes on nothing
    // the model found 1006 -> 627 and on the 12:10 one 553 -> 431, boxes coming or going about as often (18.3 -> 19.4
    // a second, 5.5 -> 5.5); after 30 ms 495 and 388, but 21.4 and 5.9 a second; after 100 ms 786 and 481.
    static constexpr double kGoneMs = 60.0;
    // How far around a thing is looked for on the image after the camera jumped: after most of the things it expected
    // to find (and at least Tracker::kShiftSupport) were lost on one image at once. On the 22:37 clip the scene went
    // 183 pixels on from one image to the next, and 200-250 by the next (more at the screen's edges).
    static constexpr float kJumpReach = 250.0f;
    // How many of them found about as far on (within Tracker::kShiftNear of their larger side and kJumpShare of how
    // far) say how far it jumped: the others are then looked for there (as far as what was read allows), and held
    // there if not found. Looked for that far around, a small thing is easily found on another like it (two of the
    // five there were; motion blur in the turn lowered the real matches to 0.65-0.9).
    static constexpr int kJumpSupport = 2;
    static constexpr float kJumpShare = 0.25f;

    void looked(double lookMs, const GreyView* image, std::span<const Tracked> shown) override;
    int snap(double timeMs, const GreyView& image, Tracked* out, int max) override;
    int learnAreas(std::span<const Tracked> shown, GreyArea* out, int max) override;
    int snapAreas(double timeMs, bool look, double lookMs, std::span<const Tracked> shown, GreyArea* out,
                  int max) override;
    double leadMs() const override { return kLeadMs; }
    std::span<const SnapFind> finds() const override { return finds_; }
    void replay(std::span<const SnapFind> recorded) override { replay_.assign(recorded.begin(), recorded.end()); }
    void clear() override;

private:
    struct Seen {
        double ms = 0.0; // When its middle was snapped where.
        float x = 0.0f;
        float y = 0.0f;
    };
    struct Thing {
        Tracked at;        // As the heuristics last showed it, with its box where it was last put
        double atMs = 0.0; // (by the look, or snapped onto an image shown then) and its speed.
        std::array<Seen, 8> seen{}; // Where it was snapped on its last images, oldest first.
        int seenCount = 0;
        bool jumped = false; // Its line started again where it was last snapped (Tracker::kJumpPixels).
        float aheadMs = 0.0f; // How far along its speed from `at` it was held on the image at stepMs (0: put there
        double stepMs = 0.0;  // then).
        bool wide = false;    // Lost there when most things were (the camera jumped): looked for kJumpReach around.
    };

    struct Search {
        float x = 0.0f; // Where its box's top left is looked for,
        float y = 0.0f;
        float reach = 0.0f;   // and how far from there;
        float aheadMs = 0.0f; // how far along its speed that is from where it was put.
    };
    struct Try { // A thing's search on an image, before snap() takes it.
        Search search;
        Snapper::Found found;
        bool hit = false;      // Found.
        bool expected = false; // Found on the image before, or put by a look since (not held).
    };

    // Notes where it was snapped now, `aheadMs` along its speed from where it was snapped before, and learns its speed
    // from that and the images before.
    static void see(Thing& thing, double timeMs, float aheadMs);
    // Where the look found a thing it shows.
    static Tracked foundOf(const Tracked& shown);
    // The things after a look at `lookMs` that shows `shown`, into `into` (looked() and snapAreas() both go by it).
    void merge(double lookMs, std::span<const Tracked> shown, std::vector<Thing>& into) const;
    // Where snap() at `timeMs` looks for a thing (snap() and snapAreas() both go by it).
    static Search searchOf(const Thing& thing, double timeMs);
    // Two boxes of one thing (Tracker's kMinAreaRatio and kMinInside).
    static bool sameThing(const Tracked& a, const Tracked& b);
    // Looks for `thing` on `image` as `search` says (or takes a recorded search's result), and notes the search.
    bool find(const GreyView& image, const Thing& thing, const Search& search, Snapper::Found& found);
    // How far the camera jumped, by the things looked for wide (tries_) that were found about as far from where they
    // were looked for (Tracker::kShiftNear, kJumpShare, kJumpSupport). False if not enough say alike.
    bool jumpShift(float& shiftX, float& shiftY) const;

    std::vector<Thing> things_; // In the last look's order.
    std::vector<Thing> next_;   // Reused on each look (and snapAreas), so it allocates nothing once warm.
    std::vector<std::uint32_t> ids_;
    std::vector<char> lookFound_; // Per box snap() hands out: the look found its thing (it was not held there).
    std::vector<Try> tries_;      // Per thing, on the image snap() snaps.
    std::vector<SnapFind> finds_;  // The last snap()'s searches,
    std::vector<SnapFind> replay_; // and the recorded ones for the next.
    Snapper snapper_;
};

} // namespace observex
