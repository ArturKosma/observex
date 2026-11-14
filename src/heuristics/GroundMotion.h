#pragma once

#include "core/Ground.h"
#include "core/Heuristics.h"
#include "heuristics/MinimapCone.h"

#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

namespace observex {

// How the ground moved from one screen image to the next, for core/Ground's GroundFit (the user asked, 2026-11-12: a
// ground plane that follows the camera). Patches on a fixed grid over where the ground can be are learned on one image
// (their pixels, and where each is on the ground, by the camera then) and found again on the next; patches reaching
// into the interface or onto characters (they move by themselves) and those at or above the horizon are left out.
//
// A camera turn turns the ground round the own character's feet: up to 0.4 radians and 270 pixels between two images
// a fifteenth of a second apart, and it starts at once (clip 2026-11-11_22-10-08, measured with SIFT), and it blurs
// the image. A patch merely shifted (as heuristics/Snapper finds things) is no match then, however far it is looked
// for. So the patches are moved as the ground would move: first how far the camera turned and slid is searched on the
// eighth-size image, all patches at once (each candidate moves every patch's pixels through the camera, turn and
// perspective included, and scores how well they match); then each patch, moved so, is looked for a few pixels around
// on the quarter-size image, which says where it went to a fraction of a pixel (what GroundFit fits the pitch to).
// Only those two levels are read (kFirstLevel on). Use one from one thread.
class GroundMotion {
public:
    static constexpr int kColumns = 10;
    static constexpr int kRows = 6;
    static constexpr int kPatches = kColumns * kRows;
    // A patch: kSamples x kSamples pixels of the quarter-size image (kSpacing full-size pixels apart): 64 pixels.
    static constexpr int kSamples = 16;
    static constexpr float kSpacing = 4.0f;
    static constexpr int kFineLevel = 2;   // The quarter size,
    static constexpr int kCoarseLevel = 3; // the eighth: the search's, on every kCoarseEvery-th sample each way.
    static constexpr int kCoarseEvery = 3;
    static constexpr int kFirstLevel = kFineLevel; // The levels of a GreyView it reads: from this one to the last.
    // The search: turns this far (radians) and slides this far (own heights) either way of what the camera was
    // doing (GroundCamera's speeds), and tilts this far (radians) from the first image to the second. While the
    // camera's pitch is not measured yet (GroundCamera::pitchMeasured), once it has turned at least kPitchTurn
    // (radians; a still camera says nothing of it), its pitch is searched from kLowestPitch to kHighestPitch too: a
    // camera looking down steeply moves the ground far otherwise than one looking ahead (clip 2026-11-12_11-34-03, 50
    // to 78 degrees, lost the ground at the start pitch). The distance is searched too, kZoomReach either way of
    // what the own character's box says (as a log of how much farther): looking down steeply its box says little
    // (GroundFit::kOwnSteepPitch), and the user scrolls.
    static constexpr float kTurnReach = 0.45f;
    // When the minimap says how far the camera turned (heuristics/MinimapCone), only this far either way of that.
    static constexpr float kKnownTurnReach = 0.04f;
    static constexpr float kSlideReach = 0.6f;
    static constexpr float kTiltReach = 0.12f;
    static constexpr float kZoomReach = 0.3f;
    static constexpr float kPitchTurn = 0.03f;
    // From nearly level (lower took clip 2026-11-12_11-34-03's tilts for looking up: 43 images fitted, not 75) to
    // straight down (clip 2026-11-12_15-50-36 goes there).
    static constexpr float kLowestPitch = 0.05f;
    static constexpr float kHighestPitch = 1.55f;
    // A tilt may go lower than the sweep starts: looking nearly level (clip 2026-11-12_17-43-14 after its turns, 1
    // degree) the fit says so, and a search held at kLowestPitch tilted the camera 5 degrees down on every image.
    static constexpr float kLowestTilt = -0.1f;
    // What the search weighs against how well the patches match (their mean correlation): a camera tilting this much
    // a radian, zooming this much a log unit, its feet sliding this much an own height more than they were going. In
    // a fast turn the image blurs and the patches match poorly whatever the motion, and a tilt that matched a little
    // better by chance added up to 50 degrees over a turn that tilted none (clip 2026-11-12_17-43-14, frames 612-660
    // and 828-876); a real tilt matches far better than none (0.9 against 0.5).
    static constexpr float kTiltCost = 0.5f;
    static constexpr float kZoomCost = 0.3f;
    static constexpr float kSlideCost = 0.1f;
    // A patch moved by the search's turn and slide is looked for this many quarter-size pixels around (kWideReach
    // when fewer than GroundFit::kMinMatches were found so: a tilt with the camera nearly level matched poorly,
    // 0.3, and the search's motion was off by more; clip 2026-11-12_17-43-14, 6.9 to 7.2 s, found none),
    static constexpr int kPatchReach = 3;
    static constexpr int kWideReach = 6;
    // and found where it matches at least this well (zero-mean normalised cross-correlation, -1 to 1).
    static constexpr float kMinMatch = 0.6f;
    // When the patches found do not fit a motion (in the fastest turns the image is blurred: few match that well),
    // the search's own motion is taken if the patches matched there at least this well on average (a wrong turn
    // matches near 0; the blurred turns of clip 2026-11-11_22-10-08 matched 0.2 to 0.6, and were right to 0.03
    // radians, as SIFT on the far ground measured them).
    static constexpr float kMinSearch = 0.15f;
    // A search matching less well than this does not count as following the ground (GroundFollower::kLostMs).
    static constexpr float kSureSearch = 0.3f;
    // A search matching less well than this turns and slides the camera but does not tilt it, unless the far patches
    // say how (farPitch): blurred, a tilt that matched a little better by chance added 27 degrees over five images of
    // a camera that tilted none (clip 2026-11-12_17-43-14, 5.5 to 5.9 s, matching 0.33 to 0.49), and in a fast turn
    // 6 degrees to one that tilted 5 and back (3.0 to 3.4 s).
    static constexpr float kSureTilt = 0.6f;
    // Above the ground's patches (the horizon on the screen), patches on what is far away (trees, buildings, the far
    // ground) say how far the camera tilted: a tilt moves them up or down as one, a turn sideways (as far things turn
    // with the minimap's turn), walking and zooming hardly at all. Looking nearly level a tilt moves the ground's
    // patches as no flat ground does (the camera rises over them, and the ground is not flat that near): they matched
    // 0.3 and the tilt was missed, while these matched 0.8 and rose 19 to 38 quarter-size pixels an image (clip
    // 2026-11-12_17-43-14, 6.9 to 7.2 s, 20 degrees down). Each is looked for kFarReach eighth-size pixels up and down
    // and kFarSideReach sideways of where the turn takes it, then to a pixel on the quarter size, found where it
    // matches there at least kFarMatch; at least kFarLeast found, their middle shift is the tilt the search starts at
    // (and costs kTiltCost a radian off it), the fit's (GroundFit::Start::tiltKnown) and a blurred search's
    // (kSureTilt). Only with the turn known.
    static constexpr int kFarReach = 26;
    static constexpr int kFarSideReach = 6;
    static constexpr float kFarMatch = 0.8f;
    static constexpr int kFarLeast = 5;
    // How far away (own heights) the far things are taken to be: the trees round the user's fields are 20 to 60.
    static constexpr float kFarRange = 50.0f;
    // The grid's rows, as parts of the screen's height: their middles from kTop to kBottom (below the top's
    // interface, above the hotbar's), each patch at least kBelowHorizon below the horizon (far ground is a blur).
    static constexpr float kTop = 0.12f;
    static constexpr float kBottom = 0.9f;
    static constexpr float kBelowHorizon = 0.06f;

    // Places the patches to learn on the next image, on a screen `width` x `height` seen by `camera` (if known; its
    // horizon: the ground's below it, the far ones above), leaving out those reaching into `avoid`.
    void place(const GroundCamera& camera, int width, int height, std::span<const GreyArea> avoid);
    // Learns the patches placed, on `image` (shown at `timeMs`; its levels from kFirstLevel on read), seen by
    // `camera` (known).
    void learn(const GreyView& image, const GroundCamera& camera, double timeMs);
    // Finds the patches learned last on `image` (shown at `timeMs`, later), `camera` being the camera they were
    // learned with as it was going then, `turn` how far it turned since if known (radians, as GroundFit's): writes
    // where each went into `out` (cleared). Returns how many.
    int find(const GreyView& image, const GroundCamera& camera, double timeMs, std::optional<float> turn,
             std::vector<GroundMatch>& out);
    // Between follows (GroundFollower::head): how far the camera turned from the image the patches were learned on to
    // `image` (shown at `timeMs`; only its eighth size read, kCoarseLevel), `camera` being the camera they were
    // learned with as it was going then; looked for from `reach` short of the nearer of `stopped` and `onward` to
    // `reach` beyond the other (radians, as GroundFit's: as far as it had turned, and on at the speed it went) and to
    // a fraction of kFineTurnStep, the rest of the motion foretold as find() foretells it. Its match is how well the
    // patches matched there (below 0: nothing to search with).
    struct Turned {
        float turn = 0.0f;
        float match = -1.0f;
    };
    static constexpr float kTurnStep = 0.005f;
    static constexpr float kFineTurnStep = 0.001f;
    Turned findTurn(const GreyView& image, const GroundCamera& camera, double timeMs, float stopped, float onward,
                    float reach) const;

    bool learned() const { return learnedCount_ > 0; }
    // The camera the patches were learned with tilted `by` radians (GroundFollower::depth: its pitch was that far off),
    // so the next motion fits from the pitch as it is now.
    void tilt(float by) { learnedCamera_.pitch += by; }
    int learnedCount() const { return learnedCount_; }
    double learnedMs() const { return learnedMs_; }
    // What the last find()'s search found: the turn (radians) and slide (own heights) as GroundFit's, the camera's
    // pitch at the first image and the second (radians), its distance at both (own heights), and how well the
    // patches matched there (0: no search).
    float searchTurn() const { return searched_.turn; }
    float searchSlideRight() const { return searched_.slideRight; }
    float searchSlideAway() const { return searched_.slideAway; }
    float searchFirstPitch() const { return searched_.firstPitch; }
    float searchPitch() const { return searched_.secondPitch; }
    float searchFirstDistance() const { return learnedCamera_.distance; }
    float searchDistance() const { return learnedCamera_.distance * std::exp(searched_.zoom); }
    float searchMatch() const { return searchMatch_; }
    // The camera's pitch on the image the last find() looked at as the far patches said (radians); none if fewer than
    // kFarLeast of them were found.
    std::optional<float> farPitch() const { return farPitch_; }
    void clear() { *this = GroundMotion(); }

private:
    static constexpr int kPerPatch = kSamples * kSamples;
    static constexpr int kCoarseSamples = (kSamples + kCoarseEvery - 1) / kCoarseEvery;
    static constexpr int kPerCoarse = kCoarseSamples * kCoarseSamples;

    struct Patch {
        float x = 0.0f; // Its middle on the screen, in pixels (its samples kSpacing apart round it).
        float y = 0.0f;
        bool placed = false;  // To learn on the next image,
        bool distant = false; // as a far patch (not on the ground);
        bool learnedFar = false; // learned as one (place() marks the next image's while this one is looked for),
        bool learned = false; // learned on the last: its samples' grey, less the patch's mean, over their spread
        std::array<float, kPerPatch> fine{};    // (quarter size);
        std::array<float, kPerCoarse> coarse{}; // likewise every kCoarseEvery-th each way, on the eighth size.
    };
    // A motion of the camera: the ground turned `turn` round the feet after sliding (`slideRight`, `slideAway`), as
    // GroundFit's, the camera at `firstPitch` on the first image and `secondPitch` on the second, and `zoom` (the
    // log of how much) farther on the second.
    struct Motion {
        float turn = 0.0f;
        float slideRight = 0.0f;
        float slideAway = 0.0f;
        float firstPitch = 0.0f;
        float secondPitch = 0.0f;
        float zoom = 0.0f;
    };
    // How well the patches match on `image` (seen with `camera`'s lens) after `motion`: their coarse samples' mean
    // correlation; -2 if too few are on the image.
    float score(const GreyView& image, const GroundCamera& camera, const Motion& motion) const;
    // The camera's pitch on `image` as the far patches say, it having turned `turn` since (radians); none if fewer
    // than kFarLeast are found.
    std::optional<float> findFarPitch(const GreyView& image, const GroundCamera& camera, float turn) const;

    std::array<Patch, kPatches> patches_{};
    int width_ = 0;
    int height_ = 0;
    int learnedCount_ = 0; // On the ground,
    int farCount_ = 0;     // and far.
    double learnedMs_ = 0.0;
    GroundCamera learnedCamera_; // The camera the patches were learned with (its distance).
    Motion searched_;
    float searchMatch_ = 0.0f;
    std::optional<float> farPitch_;
};

// Follows the game's camera over screen images (core/Ground's Grounding), as GroundMotion's matches and the own
// character's box fit it (core/Ground's GroundFit), its heading by the minimap (MinimapCone): live (engine/
// GroundFinder) and on a clip (engine/ClipGround), the same way, through observex_heuristics.dll (heuristics/Plugin).
// It reads an image's levels from GroundMotion::kFirstLevel on (whole: 0.3 MB for a 2560 x 1440 screen) and the full
// size round the minimap's cone (60 x 60 pixels once found). Use one from one thread.
class GroundFollower final : public Grounding {
public:
    void begin(double timeMs, int width, int height, const GroundHints& hints) override;
    int firstLevel() const override { return GroundMotion::kFirstLevel; }
    int fullAreas(GreyArea* out, int max) const override;
    // How the ground moved since the image before, then the patches to follow on the next. If the motion did not fit,
    // the camera may still have turned and gone as GroundMotion's search found (GroundFit::Motion::Searched).
    bool end(const GreyView& image) override;
    // Between follows the camera turns as the screen did: the patches learned on the image followed are looked for on
    // the eighth size of each image, turned only (GroundMotion::findTurn, from stopped where the image before was to
    // going on as it went, kHeadTurnReach either way: looked for only round going on, a turn that stopped went on 14
    // degrees, clip 2026-11-11_22-10-08, frames 57 and 58). Where they do not match (GroundMotion::kMinSearch), as far
    // as the minimap's cone turned
    // since that image, held between its turns: the game redraws it only about 30 times a second, steps of 4 to 9
    // degrees in a fast turn (clip 2026-11-12_18-58-01, frames 562 to 596, against the far trees' shift).
    int headAreas(double timeMs, const GroundHints& hints, GreyArea* out, int max) override;
    int headFirstLevel() const override { return GroundMotion::kCoarseLevel; }
    bool head(const GreyView& image) override;
    // The depth network's map (heuristics/DepthTilt): where its ground's line reaches 0 says the pitch (by the measured
    // table); the camera on the map's image (kPitched images kept) is compared with it, and from then on the camera,
    // the patches learned and the images kept tilt towards it on every image (pullDepth), smoothly: by the time
    // constant kDepthMs over the map's weight (less looking steeply down, where the table is steep; at least
    // kDepthLeast), so where it counts whole 95% of the way in 0.3 s, looking steeply down in 1.2 s. The ground's motion
    // still says how it tilts between maps, and the next map says how far is left. It is the better pitch: the motion
    // alone held 35 degrees where the camera looked nearly level (the horizon in the screen's middle, 5 by the map: clip
    // 2026-11-12_20-16-13, 0.8 s), 38 where far trees stood a third down the screen (12 by it: 2026-11-12_20-09-30,
    // 8.2 s) and 16 looking down past the screen's top (23 to 30 by it: 2026-11-12_20-16-13, 9 s). While the pitch is
    // only kGroundStartPitch (GroundCamera::pitchMeasured) and no map has said it since the camera was found, all the
    // way at once. Not where its rows are off their line more than kDepthStraight (the horizon in the band, a hill, a
    // wall) or the map is older than the images kept. Within kDepthHeldMs of the last map that said it, if it weighed
    // at least kDepthHeldWeight (said 25 degrees or less), a follow's fit keeps the pitch (GroundFit::Start::pitchHeld):
    // its motion says how the camera tilts, not where it looks. Steeper the maps say little (the table is steep, the
    // map's 39 degrees 60 to 82), and held so the grid lagged a fast tilt to straight down by 12 degrees (clip
    // 2026-11-12_16-53-29, 5.6 s) and one back up by 20 (2026-11-12_15-50-36, 7.95 s): the fit has it as before.
    GroundDepthNote depth(const DepthInput& map, double timeMs) override;
    static constexpr float kDepthMs = 100.0f;
    static constexpr double kDepthHeldMs = 1000.0;
    static constexpr float kDepthHeldWeight = 0.4f;
    static constexpr float kDepthLeast = 0.25f;
    static constexpr float kDepthStraight = 0.02f;
    static constexpr int kPitched = 48;
    static constexpr float kHeadTurnReach = 0.05f;
    // While the cone holds still (no change of kConeMoved in kConeGapMs) the camera turns at most kConeSlack (radians)
    // from where it was when the cone stopped: looking level or up the ground's patches turned the camera 4 to 14
    // degrees the cone did not (clip 2026-11-12_11-34-03). While the cone turns the ground's turn is free: redrawn
    // every 25 to 42 ms, the cone lags a fast turn, and held to it the grid went on 14 degrees after a turn stopped
    // and lagged 4 at its start (clip 2026-11-11_22-10-08, frames 48 to 58).
    static constexpr float kConeSlack = 0.03f;
    // How far the cone may lag a turn at a follow: as far as it turns in kConeLagMs.
    static constexpr double kConeLagMs = 50.0;
    // The cone's speed: how far it moved at its last change (at least kConeMoved, radians: its readings jitter by
    // half a degree) over the time since the change before, taken as kConeRedrawMs to kConeGapMs; none once it held
    // still longer than kConeGapMs.
    static constexpr float kConeMoved = 0.0175f;
    static constexpr double kConeRedrawMs = 25.0;
    static constexpr double kConeGapMs = 60.0;
    GroundCamera camera() const override { return between_ ? betweenCamera_ : fit_.camera(); }
    GroundNote note() const override;
    void clear() override;

    // The same parts as fullAreas().
    std::span<const GreyArea> fullParts() const { return {coneAreas_.data(), coneAreaCount_}; }
    const GroundFit& fit() const { return fit_; }
    const GroundMotion& motion() const { return motion_; }
    const MinimapCone& cone() const { return cone_; }
    int lastMatches() const { return static_cast<int>(matches_.size()); }
    // Where the last end() found the patches learned on the image before.
    std::span<const GroundMatch> lastFound() const { return matches_; }
    // The last end() fitted a motion (or tried: GroundFit::lastMotion says how it went); false on the first image, or
    // with nothing learned on the one before.
    bool followed() const { return followed_; }

    // Lost: nothing followed this long (neither fitted nor surely searched: GroundMotion::kSureSearch), the camera is
    // forgotten and found again from the start (its pitch searched), not held where it was last right (and where
    // nothing matches).
    static constexpr double kLostMs = 600.0;

private:
    GroundMotion motion_;
    GroundFit fit_;
    MinimapCone cone_;
    std::array<GreyArea, MinimapCone::kCandidates> coneAreas_{};
    std::size_t coneAreaCount_ = 0;
    bool learnedHeaded_ = false; // The cone was seen on the image the patches were learned on: its heading,
    float learnedHeading_ = 0.0f;
    bool learnedHeld_ = false;   // and whether it held still then (else that heading was a redraw behind).
    bool between_ = false; // The last image was head()'s: the camera there.
    GroundCamera betweenCamera_;
    double headMs_ = 0.0;        // The image headAreas() was for.
    float lastTurn_ = 0.0f;      // The turn since the image followed on the last image it was known on (end()'s: 0),
    double lastTurnMs_ = 0.0;    // that image,
    float lastTurnSpeed_ = 0.0f; // and how fast it went then (radians a millisecond);
    bool lastByScreen_ = false;  // whether the ground said so (head()).
    bool held_ = false;          // The cone held still on the last image between, the camera at heldTurn_ then.
    float heldTurn_ = 0.0f;
    bool coneKnown_ = false;     // The cone's last change: when, its heading, and how fast it turned to it.
    double coneMs_ = 0.0;
    float coneHeading_ = 0.0f;
    float coneSpeed_ = 0.0f;
    // Takes the cone's reading on the image at `timeMs` (seen or not) into coneMs_ and the rest.
    void noteCone(bool seen, double timeMs);
    // Whether the cone held still up to `timeMs` (kConeGapMs and more).
    bool coneHeld(double timeMs) const { return coneKnown_ && timeMs - coneMs_ > kConeGapMs; }
    // How far the cone turned since the image followed (`followed` the camera there): from its heading there if it
    // held still then, else from the yaw followed (in a turn the cone there was a redraw behind, up to 11 degrees:
    // clip 2026-11-11_22-10-08, 0.47 s).
    float coneTurn(const GroundCamera& followed) const;
    std::vector<GroundMatch> matches_;
    std::vector<GreyArea> avoid_; // begin()'s hints.avoid: the characters and the interface, out of the depth too.
    struct Pitched {
        double timeMs = 0.0;
        float pitch = 0.0f;
    };
    std::array<Pitched, kPitched> pitched_{}; // The camera's pitch on the last images (end()'s and head()'s), oldest
    int pitchedCount_ = 0;                    // first from pitchedNext_ on.
    int pitchedNext_ = 0;
    bool depthTaken_ = false; // A map has said the pitch since the camera was last found.
    float depthLeft_ = 0.0f;  // How far the last map's pitch is from the camera's still (radians),
    float depthRate_ = 0.0f;  // the share of it to go a millisecond (its weight over kDepthMs),
    double depthMs_ = 0.0;    // and the image pullDepth() was last for.
    double depthSaidMs_ = 0.0; // The image of the last map that said the pitch,
    float depthWeight_ = 0.0f; // and its weight.
    // Tilts the camera, the patches learned and the images kept towards the last map's pitch, as far as the time since
    // the image before says; then
    void pullDepth(double timeMs);
    // tilts them all `by` radians.
    void tiltAll(float by);
    // Keeps the camera's pitch on the image at `timeMs`; forgets them all while the camera is not known.
    void notePitch(double timeMs);
    // The camera's pitch on the image at `timeMs`, between the two kept round it; none if older than all.
    std::optional<float> pitchAt(double timeMs) const;
    double timeMs_ = 0.0;       // The image begin() was for.
    double lostSinceMs_ = -1.0; // The first image not followed since the last one followed; -1: none.
    bool followed_ = false;
    bool fitted_ = false;
    int learnedBefore_ = 0; // Patches learned on the image before the last end()'s.
};

} // namespace observex
