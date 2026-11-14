#pragma once

#include "core/Depth.h"
#include "core/Detection.h"
#include "core/Heuristics.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace observex {

// The ground under the user's character, as Lineage 2's camera sees it (the user asked, 2026-11-12: the first step of
// the depth plan, a ground plane that follows the camera, shown by the flag Ground live and on clips). The game's
// camera circles a point on the user's own character (kGroundTarget of its height above its feet, always in the
// screen's middle) at some distance, looking down at some pitch, with a lens that never changes. So on flat ground two
// numbers, the pitch and the distance, say where every ground pixel is. Lengths are in own-character heights.
//
// Neither number can be read off the own character's box alone: looking down more and coming nearer change it the
// same way (20 degrees more pitch moves its head 2 pixels: less than a box's jitter). The ground's motion between two
// images tells them apart: when the camera turns, the ground turns round the character's feet; walking slides it;
// tilting and zooming move it as only that pitch and distance would. So the pitch comes from the ground's motion
// (heuristics/GroundMotion's matches) whenever it moves, and the distance from the own character's box at that pitch.
//
// Measured on the user's clips (2026-11-12; scratchpad ground/ of that session, Python): the ground's motion between
// frames of 73 camera turns of clip 2026-11-11_22-10-08 fits a lens of 1700 pixels on the 2560-pixel-wide screen best
// (74 x 46 degrees; 1600 and 1800 fit worse), there with the pitch at 18-22 degrees and the distance steady; fitted
// jointly with the own character's box over 201 moving frame pairs of that clip and 2026-11-12_11-34-03 (a tilt from
// 50 to 78 degrees and back): the point circled at 0.856 of the character's height, its box's bottom its feet 0.167
// of its height nearer the camera, its top its head 0.02 farther.
inline constexpr float kGroundFocal = 1700.0f;    // The lens, in pixels, on a 2560-pixel-wide screen.
inline constexpr float kGroundTarget = 0.856f;    // The point the camera circles, of the own character's height.
inline constexpr float kOwnFeetReach = 0.167f;    // Its box's bottom: its feet this far nearer the camera,
inline constexpr float kOwnHeadReach = 0.02f;     // its top: its head this far farther (own heights).
inline constexpr float kGroundStartPitch = 0.35f; // Radians (20 degrees), until the ground first moves.

// The game's camera, as far as the ground needs it.
struct GroundCamera {
    bool known = false;
    bool pitchMeasured = false; // The pitch came from the ground's motion (not kGroundStartPitch yet).
    float pitch = 0.0f;         // How far it looks down, in radians: 0 level, pi/2 straight down, below 0 up.
    float distance = 0.0f;      // From the point it circles, in own-character heights.
    float focal = 0.0f;         // Its lens, in pixels.
    float middleX = 0.0f;       // The screen's middle, in pixels: where the point it circles is.
    float middleY = 0.0f;
    // Where it has turned and gone since it was first known, from the ground's motion: so something drawn fixed to
    // the ground stays on it. The ground's own map: `yaw` radians how far the camera has turned (its right, away
    // turned that far from the map's), the own character's feet at (`feetX`, `feetY`) on it (own heights).
    double timeMs = 0.0; // The image it is of (any clock; the one GroundFit is given).
    float yaw = 0.0f;
    float feetX = 0.0f;
    float feetY = 0.0f;
    float yawSpeed = 0.0f;   // How fast it turns and tilts (radians a millisecond) and the feet go on the map (own
    float pitchSpeed = 0.0f; // heights a millisecond), lately.
    float feetSpeedX = 0.0f;
    float feetSpeedY = 0.0f;
};

// `camera` at `timeMs`, turned and gone on along its speeds (at most kGroundAheadMs either way): for an image newer
// than the one it is of.
inline constexpr double kGroundAheadMs = 150.0;
GroundCamera groundAt(const GroundCamera& camera, double timeMs);
// Between `before` and `after` (of later images) at `timeMs`, e.g. on a clip's frames between two fits: the nearer one
// where it is not between them, or only one is known.
GroundCamera groundBetween(const GroundCamera& before, const GroundCamera& after, double timeMs);
// Among `cameras` (in time order, e.g. a clip's, engine/ClipGround), the camera at `timeMs`, between the two around it
// (groundBetween); unknown if there are none.
GroundCamera groundAmong(std::span<const GroundCamera> cameras, double timeMs);
// A point on the ground's map (as GroundCamera's yaw and feet) as right and away of the own character's feet.
void mapToGround(const GroundCamera& camera, float mapX, float mapY, float& right, float& away);

// The camera on a screen `width` x `height` pixels at `pitch` (radians) and `distance` (own heights).
GroundCamera groundCamera(float pitch, float distance, float width, float height);

// A point on the screen, in pixels; `ahead` false when it is behind the camera (not on the screen at all).
struct GroundPoint {
    float x = 0.0f;
    float y = 0.0f;
    bool ahead = false;
};

// Where the point `right`, `away` and `up` own-character heights from the own character's feet is on the screen
// (right: to the screen's right; away: away from the camera, along the ground; up: above the ground).
GroundPoint worldToScreen(const GroundCamera& camera, float right, float away, float up);
inline GroundPoint groundToScreen(const GroundCamera& camera, float right, float away) {
    return worldToScreen(camera, right, away, 0.0f);
}
// How far ahead of the camera's eye (along where it looks) the point is, in own heights; at most 0: behind it.
float aheadOfCamera(const GroundCamera& camera, float right, float away, float up);
// The ground point under screen pixel (x, y), in own heights from the own character's feet as above (`x` right,
// `y` away); none at or above the horizon.
std::optional<GroundPoint> screenToGround(const GroundCamera& camera, float x, float y);
// The screen row the flat ground ends at, far away (can be off the screen).
float horizonRow(const GroundCamera& camera);

// The own character's box, as screen rows, that `camera` shows: its feet's edge nearest the camera and its head's
// farthest.
struct OwnRows {
    float top = 0.0f;
    float bottom = 0.0f;
};
OwnRows ownRows(const GroundCamera& camera);
// The distance at which `camera`'s pitch shows the own character's box down to `bottom` (a screen row); none if no
// distance can (its bottom above the middle).
std::optional<float> distanceForOwnBottom(const GroundCamera& camera, float bottom);

// The own character among `things`: the surest one of `ownKind` standing in the screen's middle (within a tenth of
// its width), at least kGroundOwnSure sure; null if none.
inline constexpr float kGroundOwnSure = 0.5f;
const Tracked* ownCharacter(std::span<const Tracked> things, int ownKind, float width);

// A patch of the screen found again on a later image: its middle on the first and on the second, in pixels.
struct GroundMatch {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

// Follows the camera: the pitch from the ground's motion, the distance from the own character's box.
class GroundFit {
public:
    // The distance follows the own character's box over this long (its box jitters a pixel or two from look to look;
    // the distance only changes while the user scrolls).
    static constexpr double kDistanceSmoothMs = 80.0;
    // The box tells the distance only while it is this tall to as tall as the character would be there (its bottom
    // the feet); else the distance is held.
    static constexpr float kOwnLeastTall = 0.65f;
    static constexpr float kOwnMostTall = 1.5f;
    // A box this many times taller or shorter than the last one taken is another thing's: it is not taken (as if the
    // own character were not seen). The tracker gave the own character's id to a wolf in front of it, whose box put
    // the camera 33 away for half a second (clip 2026-11-12_17-43-14, 1.55 to 2.08 s); zooming changes the box at
    // most a sixth from look to look.
    static constexpr float kOwnMostJump = 2.0f;
    // Looking down steeper than this (radians) it tells nothing: its bottom hardly moves as the camera comes nearer
    // (a fifth of what it does at 20 degrees), and it is not the feet (clip 2026-11-12_15-50-36, tilting to straight
    // down, put the camera from 4 to 8 away while the ground did not grow). The distance is held then, and only how
    // the ground grows or shrinks changes it (heuristics/GroundMotion's search, the fit).
    static constexpr float kOwnSteepPitch = 1.2f;
    // Unknown again this long after the own character was last seen.
    static constexpr double kKeepMs = 1000.0;
    // A motion with its patches moving less than this (pixels, the middle of them) says too little of the pitch: it
    // is held, and only how the camera turned and went is fitted.
    static constexpr float kMinMotion = 1.5f;
    // Fewer matches than this that agree with the ground: not fitted.
    static constexpr int kMinMatches = 10;
    // The most the camera turns (radians), tilts (radians) and goes (own heights) in a millisecond: a fit beyond is
    // matches agreeing by chance (the user's fastest flick of the mouse, by the minimap: 0.45 radians in a fifteenth
    // of a second, clip 2026-11-11_22-10-08; the run is about 5 own heights).
    static constexpr double kMostTurn = 0.008;
    static constexpr double kMostTilt = 0.004;
    static constexpr double kMostGo = 0.02;

    // How the last motion() went.
    enum class Motion : std::uint8_t {
        Fitted,
        NoCamera,    // Not known: the own character not seen yet.
        NoOwn,       // The own character not seen near either image.
        FewMatches,  // Fewer than kMinMatches patches found again,
        FewAgreeing, // or agreeing with the ground.
        TooFar,      // Fitted beyond what the camera can do.
        Searched,    // Not fitted, but turned and gone as GroundMotion's search found (searched()).
    };
    static constexpr int kMotions = 7;

    // The own character's box on the screen at `timeMs` (milliseconds, any clock), from `top` to `bottom`, on a
    // screen `width` x `height`. Starts the camera at kGroundStartPitch if it is unknown.
    void own(float top, float bottom, float width, float height, double timeMs);
    // The own character not seen on the image at `timeMs`: forgets the camera kKeepMs after it was last.
    void unseen(double timeMs) {
        if (camera_.known && timeMs - ownMs_ > kKeepMs) {
            reset();
        }
    }
    // As own(), from the things shown at `timeMs`, the model's kind `ownKind` being the own character's (-1: the
    // model does not know it); forgets the camera kKeepMs after the own character was last among them.
    void things(std::span<const Tracked> things, int ownKind, float width, float height, double timeMs);
    // How the ground moved from the image at `firstMs` to the one at `secondMs` (`matches`, heuristics/GroundMotion;
    // those off the ground count for nothing). Fits the pitch (and the distance, with the own character's boxes given
    // nearest those times) at the later image, starting from the camera it has. Returns whether it fitted.
    bool motion(std::span<const GroundMatch> matches, double firstMs, double secondMs) {
        return motion(matches, firstMs, secondMs, {camera_.pitch, camera_.pitch, camera_.distance, camera_.distance});
    }
    // Where a fit starts: the camera's pitch (radians) and distance (own heights) at the first image and the second
    // (as heuristics/GroundMotion's search found them), and how far it turned if known (the minimap's cone,
    // heuristics/MinimapCone: the fit's turn is held to it, kKnownTurnSpread either way), and whether the tilt is
    // known (from far things, heuristics/GroundMotion: the fit's tilt is held to it and its pitch at the first image
    // to the one given, kKnownTiltSpread either way; with the camera nearly level the ground's motion says little of
    // the pitch, and a fit took a known 20-degree tilt for 35), and whether the pitch is held (the depth network says
    // it, heuristics/GroundFollower::depth: the fit's pitch at the first image is held to the one given, kHeldPitchSpread
    // either way, so the motion says mostly how it tilts; a turn, which says the pitch, still takes it about a quarter
    // of the way. Free, the own character's box and the matches put it back where they had it at every follow, up to 3
    // degrees off the map's, and the grid went back and forth so).
    struct Start {
        float firstPitch = 0.0f;
        float secondPitch = 0.0f;
        float firstDistance = 0.0f;
        float secondDistance = 0.0f;
        bool turnKnown = false;
        float turn = 0.0f;
        bool tiltKnown = false;
        bool pitchHeld = false;
    };
    static constexpr double kKnownTurnSpread = 0.01;
    static constexpr double kKnownTiltSpread = 0.02;
    static constexpr double kHeldPitchSpread = 0.005;
    bool motion(std::span<const GroundMatch> matches, double firstMs, double secondMs, const Start& start);
    // When motion() did not fit (too few matches, or too few agreeing: in the fastest turns the image is blurred), the
    // motion heuristics/GroundMotion's search found instead: the ground turned `turn` round the feet after sliding
    // (`slideRight`, `slideAway`) of the first image's view, the camera's pitch and distance as `found`. The camera
    // turns, goes, tilts and zooms by it. Returns false (nothing done) beyond what the camera can do.
    bool searched(float turn, float slideRight, float slideAway, const Start& found, double firstMs, double secondMs);
    // When neither motion() nor searched() said how it went: the camera turned `turn` (radians) by the image at
    // `secondMs` and turning on at `turnSpeed` (radians a millisecond), as the images between or the minimap said
    // (heuristics/GroundMotion's head); else it stood at the last image's yaw, and the grid jumped back up to 13
    // degrees in a fast turn (clip 2026-11-12_17-43-14, 8.71 and 8.78 s).
    void turned(float turn, float turnSpeed, double secondMs) {
        if (camera_.known) {
            camera_.timeMs = secondMs;
            camera_.yaw += turn;
            camera_.yawSpeed = turnSpeed;
        }
    }
    // Tilts the camera `by` radians as the depth network's map says it looks (heuristics/GroundFollower::depth): the
    // pitch the ground's motion gave was off by that much.
    void tilt(float by) {
        if (camera_.known) {
            camera_.pitch += by;
        }
    }
    // Where the camera looks on the image last given, as the minimap shows it (radians clockwise from north,
    // heuristics/MinimapCone): its yaw goes there, so it never drifts. At once the first time (the ground's map then
    // has north up) and when the ground did not say how it turned (`moved` false: in the fastest turns nothing
    // matches); else kHeadShare of the way (the cone's readings jitter a little, the ground's turns do not).
    static constexpr float kHeadShare = 0.3f;
    void head(float heading, bool moved);

    const GroundCamera& camera() const { return camera_; }
    // How many of the last motion's matches agreed with the ground, and how far off they were on average (pixels).
    int lastAgreeing() const { return lastAgreeing_; }
    float lastMiss() const { return lastMiss_; }
    Motion lastMotion() const { return lastMotion_; }
    void reset() { *this = GroundFit(); }

private:
    struct OwnSeen {
        OwnRows rows;
        double timeMs = 0.0;
    };
    // The own character's box given nearest `timeMs` (lately), if any within kOwnNearMs.
    static constexpr double kOwnNearMs = 150.0;
    std::optional<OwnRows> ownAt(double timeMs) const;
    // Turns and moves the camera by a motion of the ground from an image `gapMs` before to the one at `secondMs`
    // (its pitch already set; `tilt` how far it tilted, for its speed).
    void move(float turn, float tilt, float slideRight, float slideAway, double gapMs, double secondMs);
    // Not known to turn, tilt or go on any more (a motion not fitted; searched() may still say how it went).
    void stop() {
        camera_.yawSpeed = 0.0f;
        camera_.pitchSpeed = 0.0f;
        camera_.feetSpeedX = 0.0f;
        camera_.feetSpeedY = 0.0f;
    }

    GroundCamera camera_;
    std::array<OwnSeen, 32> seen_{}; // The own character's boxes last given, oldest first from seenNext_ on.
    int seenCount_ = 0;
    int seenNext_ = 0;
    double ownMs_ = -1e30; // When it was last given.
    int lastAgreeing_ = 0;
    float lastMiss_ = 0.0f;
    Motion lastMotion_ = Motion::NoCamera;
    bool headed_ = false; // The yaw has come from the minimap (head()).
};

// What following the camera needs to know of an image besides its pixels. Plain data: it goes into
// observex_heuristics.dll (heuristics/Abi).
struct GroundHints {
    bool hasOwn = false; // The own character's box was seen (ownCharacter): its rows.
    float ownTop = 0.0f;
    float ownBottom = 0.0f;
    const GreyArea* avoid = nullptr; // Where the ground is hidden or moves by itself: the interface, the characters.
    int avoidCount = 0;
    const GreyArea* windows = nullptr; // The interface's pieces (core/UiLayout), among them the minimap.
    int windowCount = 0;
};

// How following went on the last image (F3's numbers, the bench's notes). Plain data, as GroundHints.
struct GroundNote {
    bool followed = false; // A motion was fitted or tried (false on the first image, or with nothing learned before);
    bool fitted = false;   // it fitted,
    GroundFit::Motion motion = GroundFit::Motion::NoCamera; // or how it went.
    int learned = 0;       // Patches learned on the image before,
    int found = 0;         // found again on this one,
    float moved = 0.0f;    // how far they went (the middle of them, pixels),
    int agreeing = 0;      // agreeing with the camera fitted, and how far off those were on average (pixels).
    float miss = 0.0f;
    float searchTurn = 0.0f;  // heuristics/GroundMotion's search: the turn it found (radians),
    float searchMatch = 0.0f; // how well the patches matched there.
    bool headed = false;      // The minimap's cone was seen (heuristics/MinimapCone): where the camera looks (radians
    float heading = 0.0f;     // clockwise from north).
};

// How a depth map went (Grounding::depth; F3's numbers, the bench's notes). Plain data, as GroundHints.
struct GroundDepthNote {
    bool measured = false; // The ground's depth fell on a straight enough line up the screen to tell the pitch:
    float seen = 0.0f;     // where it reaches 0 (the horizon if the network's offset were 0), as a pitch (radians),
    float pitch = 0.0f;    // the pitch that means (heuristics/DepthTilt's table),
    float straight = 0.0f; // how far its rows were off the line (of their range),
    float then = 0.0f;     // the camera's pitch on the map's image,
    float pulled = 0.0f;   // and how far it was off (radians): the camera tilts there over the next images.
};

// Follows the game's camera over screen images (heuristics/GroundFollower): what the live ground finder
// (engine/GroundFinder) and a clip's (engine/ClipGround) use, made by observex_heuristics.dll (engine/
// HeuristicsLibrary) so that a change to following shows without closing the app (the user asked, 2026-11-12). Each
// image: begin() with what is known of it; then its levels from firstLevel() on read whole, and fullAreas() on its
// full size; then end(). Each image between them (the user asked, 2026-11-12: at 15 a second the grid stood still for
// the first 75 ms of a turn, and went on turning 75 ms after it stopped): headAreas(), those read on its full size,
// and its levels from headFirstLevel() on read whole, then head(). Each depth map, as soon as it is made: depth().
// Use one from one thread.
class Grounding {
public:
    virtual ~Grounding() = default;
    // Before the image shown at `timeMs` (times must not go back) on a screen `width` x `height`: takes `hints`.
    virtual void begin(double timeMs, int width, int height, const GroundHints& hints) = 0;
    // The first of an image's levels end() reads whole.
    virtual int firstLevel() const = 0;
    // The full-size parts of the image begin() was for that end() reads too: up to `max` into `out`; how many.
    virtual int fullAreas(GreyArea* out, int max) const = 0;
    // That image (read as above): how the ground moved since the image before. Returns whether the motion fitted.
    virtual bool end(const GreyView& image) = 0;
    // Between two follows, the image shown at `timeMs` (after the last end()'s; times must not go back): the
    // full-size parts head() reads, up to `max` into `out`; how many (none: nothing to read).
    virtual int headAreas(double timeMs, const GroundHints& hints, GreyArea* out, int max) = 0;
    // The first of an image's levels head() reads whole.
    virtual int headFirstLevel() const = 0;
    // That image (read as above): the camera followed, turned as far as the ground did since (or, where that is not
    // found, the minimap's cone, held between its turns), its tilt and walk gone on along their speeds (groundAt); as
    // followed gone on along all of them where neither says. Returns whether the ground said.
    virtual bool head(const GreyView& image) = 0;
    // The depth network's map of the image shown at `timeMs` (one of those given before: it comes a few milliseconds
    // after its image): the camera's pitch is pulled towards what the ground's depth in it says (the user asked,
    // 2026-11-12: "make the grid position according to it", the pitch only came from the ground's motion and a
    // still camera kept a wrong one). Before the next begin() or headAreas().
    virtual GroundDepthNote depth(const DepthInput& map, double timeMs) = 0;
    // The camera as followed up to the last end(), or the last head() after it.
    virtual GroundCamera camera() const = 0;
    virtual GroundNote note() const = 0;
    // Forgets the camera: found again from the start.
    virtual void clear() = 0;
};

} // namespace observex
