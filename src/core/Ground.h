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
// of its height nearer the camera, its top its head 0.02 farther. The mouse's counts pinned the lens closer (the night
// of 2026-11-15, core/MouseCamera: a count turns 0.19775 degrees by the minimap's cone and by a 360 lined up from the
// images alone; the points of 1291 turning frames moved 1739.8 pixels a radian, of 949 tilting ones 1743.9): 1741.
inline constexpr float kGroundFocal = 1741.0f;    // The lens, in pixels, on a 2560-pixel-wide screen.
inline constexpr float kGroundTarget = 0.856f;    // The point the camera circles, of the own character's height.
inline constexpr float kOwnFeetReach = 0.167f;    // Its box's bottom: its feet this far nearer the camera,
inline constexpr float kOwnHeadReach = 0.02f;     // its top: its head this far farther (own heights).
inline constexpr float kGroundStartPitch = 0.35f; // Radians (20 degrees), until the ground first moves.

// The game's camera, as far as the ground needs it.
struct GroundCamera {
    bool known = false;
    bool pitchMeasured = false; // The pitch came from the ground's motion (not kGroundStartPitch yet).
    bool pitchByTurns = false;  // It is the one the camera's turns said (heuristics/TurnPitch): the ground's slope
                                // does not lean it.
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
// Whether an interface piece (`windows`, core/UiLayout's) hides the own character's feet: its box (from `top` to
// `bottom`, screen rows) starts above a piece across the screen's `middleX` and ends at most kFeetHiddenSlack above
// its top edge, or into it. Its bottom is then the piece's edge, not the feet: zoomed in, the hotbar cut the box at
// its top row and put the camera 1.5 own heights too far (clips 2026-11-16_09-06-57 and 09-07-58).
inline constexpr float kFeetHiddenSlack = 4.0f;
bool ownFeetHidden(float top, float bottom, float middleX, std::span<const GreyArea> windows);

// A patch of the screen found again on a later image: its middle on the first and on the second, in pixels.
struct GroundMatch {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

// The camera's distance moves only by the mouse's wheel (the user, 2026-11-16, on clip 2026-11-16_00-02-01: the ring
// round the feet "sinusoidally grows and gets smaller, making the whole grid unstable ... it should only grow when we
// zoom in"; then "you could listen to the scroll and calculate how much we move in/out"). The own character's box,
// which says the distance, is as tall as its pose: running, it went from 134 to 255 pixels tall and back about three
// times a second, and the distance drawn swung 13% either way with the wheel never turned. So the distance is the
// middle of every distance the box said (of the last kKept, as logs; on that clip 1.2% either way after the first
// 1.5 s), and each notch of the wheel moves it and them kStep (measured on the user's clips 2026-11-16_09-06-57 and
// 09-07-58, 17 notches between steady boxes: the game's zoom is whole on the first frame that shows it, about
// MouseCamera::kWheelLagMs after the report, and each notch went 0.64 to 0.98 own heights, 0.85 on average, nearer
// and farther alike, by the box's bottom at 18 to 53 degrees down; a ratio fitted worse, 0.81 to 0.94 a notch).
// Nearer than kFineBelow the notches come kFineStep (looking straight down, by the box's width: 1.18, 0.99, 0.80,
// 0.60, 0.37 in, 1.35, 1.51, 1.67, 1.80, 2.66 out), down to kNearest; no farther than kFarthest (both clips stopped
// at 9.8 to 10.1 looking down 19 to 25 degrees; the box put it farther looking down steeper, which is not followed).
// Notches past a limit go for nothing, as in the game. After a notch the box is not taken for kSettleMs (the
// tracker's box leads for two frames after a jump), and only the last kAfterStep distances are kept, moved: if the
// game went otherwise the box's own say outweighs them a third of a second on (keeping 8, the replays of both clips
// were off a little longer). A box saying nearer than kNearest or farther than kFarthest is taken as there (still
// above or below the middle: it moves only if most are beyond): looking down 50 degrees at the farthest notch it said
// 19.8 (its width said a sixth farther than at 19 degrees, the box's bottom twice as far; the tree's branches round
// the camera), and held there the distance was a third too far for half a second after tilting back (clip 09-07-58,
// 8.3 to 8.8 s). Left out instead, the running clip 2026-11-15_23-34-30 swung 2.6% either way, not 2.1. The box
// puts the camera farther the steeper it looks (at 50 degrees 10 to 18% farther than at 18 at the same notch), which
// the middle follows only slowly; kept for a shorter time (64), running pulsed again (2.7% either way, not 1). The motion fit and the search keep it
// (GroundFit::motion and searched, heuristics/GroundMotion's find). It outlasts the camera being lost and found
// again (GroundFit::reset): the wheel did not turn, so the distance is the same. The wheel not watched (a clip saved
// before the mouse was), the distance follows the box as before.
class ZoomHold {
public:
    static constexpr float kStep = 0.85f;      // Own heights a notch,
    static constexpr float kFineBelow = 1.6f;  // and nearer than this kFineStep.
    static constexpr float kFineStep = 0.2f;
    static constexpr float kNearest = 0.4f;
    static constexpr float kFarthest = 10.0f;
    static constexpr double kSettleMs = 40.0;
    static constexpr int kAfterStep = 4;       // About a quarter of a second of follows at 15 a second.
    static constexpr int kKept = 512;          // About 34 s of them.
    // The image at `timeMs`: whether the wheel is watched, and its notches the game has taken by then (GroundHints'
    // wheel). Moves the distance held by the notches since the last image (from `current`, the camera's, if nothing
    // was taken yet; 0 unknown); returns whether it did.
    bool wheel(bool watched, double notches, double timeMs, float current);
    // The wheel is watched: the distance holds (once anything said it).
    bool holds() const { return holds_; }
    // A distance the own character's box said on the image at `timeMs` (own heights), taken while it holds.
    void take(float distance, double timeMs);
    // The middle of the distances taken, moved by the notches since; none before the first.
    std::optional<float> distance() const;
    int samples() const { return count_; }
    void clear() { *this = ZoomHold(); }
    // `distance` moved by `notches` (+ farther): kStep each (kFineStep nearer than kFineBelow), kNearest to kFarthest
    // (beyond one, only back towards it).
    static float stepped(float distance, double notches);

private:
    void push(float log);
    void keepLast(float shift); // Only the last kAfterStep, moved by `shift` (a log).
    void middle(); // middle_ from the logs.

    bool holds_ = false;
    bool counted_ = false; // notches_ is the last image's.
    double notches_ = 0.0;
    double settleMs_ = -1e300; // The box is taken again from then on.
    std::array<float, kKept> logs_{}; // The logs of the distances taken, the oldest at next_ once full.
    int count_ = 0;
    int next_ = 0;
    float middle_ = 0.0f; // Their middle (a log).
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

    // The mouse's wheel on the image at `timeMs` (GroundHints' wheel; each image, before own()): its notches move the
    // distance held (ZoomHold), and the camera's with it at once.
    void wheel(bool watched, double notches, double timeMs);
    // The distance held: the middle of the own boxes', taken at a measured pitch, moved by the wheel's notches since;
    // none before the first (the distance then as before), or while the wheel is not watched.
    std::optional<float> heldDistance() const;
    const ZoomHold& zoom() const { return zoom_; }
    // The own character's box on the screen at `timeMs` (milliseconds, any clock), from `top` to `bottom`, on a
    // screen `width` x `height`. Starts the camera at kGroundStartPitch if it is unknown (at the distance held, if
    // one is). While the wheel is watched, what the box says at a measured pitch is taken into the distance held
    // (ZoomHold::take) and the distance is that; else it follows the box over kDistanceSmoothMs.
    // `feetSeen` false (ownFeetHidden): the box keeps the camera known but says no distance.
    void own(float top, float bottom, float width, float height, double timeMs, bool feetSeen = true);
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
    // While the distance holds (heldDistance()), the fit's distance at the second image is held to it this tightly
    // (a log), and at the first to the one given (the camera's then: a notch between moved it), and the own box counts
    // for nothing: its bob would only move into the pitch instead.
    static constexpr double kHeldDistanceSpread = 0.001;
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
    // The camera's yaw is `yaw` (radians), turning at `yawSpeed` radians a millisecond, and, if `pitched`, its pitch
    // `pitch` tilting at `pitchSpeed`: the mouse's counts said them (core/MouseCamera). Not pitched, the pitch stays
    // the fit's (and whether it was measured).
    void rotated(float yaw, float yawSpeed, bool pitched, float pitch, float pitchSpeed) {
        if (camera_.known) {
            camera_.yaw = yaw;
            camera_.yawSpeed = yawSpeed;
            if (pitched) {
                camera_.pitch = pitch;
                camera_.pitchSpeed = pitchSpeed;
                camera_.pitchMeasured = true;
            }
        }
    }
    // The pitch is the one the camera's turns say (heuristics/TurnPitch, `on`), tilting `pitchSpeed` radians a
    // millisecond lately: GroundCamera::pitchByTurns; else not.
    void byTurns(bool on, float pitchSpeed) {
        camera_.pitchByTurns = on && camera_.known;
        if (camera_.pitchByTurns) {
            camera_.pitchMeasured = true;
            camera_.pitchSpeed = pitchSpeed;
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
    // Forgets the camera, but not the distance held: the wheel did not turn, so it is the same when found again.
    void reset() {
        const ZoomHold zoom = zoom_;
        *this = GroundFit();
        zoom_ = zoom;
    }
    // Forgets everything (another screen, another clip).
    void forget() { *this = GroundFit(); }

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
    ZoomHold zoom_;
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
    // The camera on the image by the mouse's counts (core/MouseCamera, since the night of 2026-11-15): how far they
    // turned it since they began (radians, unwrapped), its yaw if the minimap's cone anchored them (radians
    // clockwise from north) and its pitch if something said it (radians, + down). They are the camera's: the
    // follower turns and tilts it so, and only fits how it went and how far it is.
    bool mouseKnown = false;
    bool mouseYawKnown = false;
    bool mousePitchKnown = false;
    double mouseTurned = 0.0;
    float mouseYaw = 0.0f;
    float mousePitch = 0.0f;
    // The mouse's wheel (since 2026-11-16, ABI 12): whether it is watched, and its notches the game has taken by the
    // image (core/MouseCamera's wheelNotches): the game's camera comes nearer or goes farther only by them, so the
    // camera's distance holds between them and moves by them (ZoomHold). Not watched (a clip saved before the mouse
    // was), as before it.
    bool wheelWatched = false;
    double wheelNotches = 0.0;
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
    // The pitch by the camera's turns (heuristics/TurnPitch): points followed from the image before, of them agreeing
    // with the two cameras fitted to them (if they fitted), how far those were off (full-size pixels), how far the
    // camera turned and the pitch they say on this image and how sure it is (radians); whether it was taken, and
    // whether the turns hold the pitch now (the camera's pitch is theirs), how sure of it. And how far the points say
    // it tilted (radians, + down; since the evening of 2026-11-15, for the mouse's counts: bench --ground).
    int turnPoints = 0;
    int turnAgreeing = 0;
    bool turnFitted = false;
    bool turnTaken = false;
    bool turnHolds = false;
    float turnMiss = 0.0f;
    float turnTurn = 0.0f;
    float turnTilt = 0.0f;
    float turnPitch = 0.0f;
    float turnSpread = 0.0f;
    float turnHoldSpread = 0.0f;
    float turnMs = 0.0f; // How long it took (milliseconds).
    // The distance held (the wheel watched: ZoomHold), the middle of this many own boxes.
    bool zoomHeld = false;
    int zoomSamples = 0;
};

// How a depth map went (Grounding::depth; F3's numbers, the bench's notes). Plain data, as GroundHints.
struct GroundDepthNote {
    bool measured = false; // The ground's depth fell on a straight enough line up the screen to tell the pitch:
    float seen = 0.0f;     // where it reaches 0 (the horizon if the network's offset were 0), as a pitch (radians),
    float pitch = 0.0f;    // the pitch that means (heuristics/DepthTilt's table),
    float straight = 0.0f; // how far its rows were off the line (of their range),
    float then = 0.0f;     // the camera's pitch on the map's image,
    float pulled = 0.0f;   // and how far it was off (radians): the camera tilts there over the next images.
    // The terrain (GroundHeights, heuristics/DepthTerrain): whether the map went into it, and if so its scale and
    // offset fitted to the flat ground round the feet (network = scale x inverse depth + offset, inverse depth in
    // 1 / own heights), over how many samples, how far they were off it (of its range), and how many cells it said.
    bool shaped = false;
    float shapeScale = 0.0f;
    float shapeOffset = 0.0f;
    int shapeSamples = 0;
    float shapeMiss = 0.0f;
    int shapeCells = 0;
};

// The terrain under the grid (the user asked, 2026-11-16: "the grid points should wrap around terrain which we can
// get the shape of from the depth"): how high the ground is in cells of kCell own heights on the ground's own map
// (GroundCamera's yaw and feet), a square of kCells each way round where the feet were, made from the depth network's
// maps (heuristics/DepthTerrain). Heights are own heights on one level for the whole square (the feet's when it was
// begun): the camera's feet are at the ground's height under them, so a point is groundHeight() above them. Where no
// map saw the ground it is filled from round it, and towards the flat ground through the feet farther out. Plain
// data: it comes out of observex_heuristics.dll (heuristics/Abi).
struct GroundHeights {
    static constexpr int kCells = 128;
    static constexpr float kCell = 0.5f;
    std::uint64_t version = 0; // Another number each time it changes; 0: none yet.
    bool known = false;        // Some map has said it (else every height is the flat ground's).
    double timeMs = 0.0;       // The image of the newest map in it.
    float firstX = 0.0f;       // The ground's map point of cell (0, 0)'s middle (own heights); cell (x, y) is kCell x
    float firstY = 0.0f;       // and y farther along.
    std::array<float, kCells * kCells> height{}; // Row by row (y), own heights.
    std::array<std::uint8_t, kCells * kCells> seen{}; // How surely: 0 never seen (filled), 255 seen well many times.
    // What the newest map in it saw in front (since 2026-11-16: the user asked to hide the grid behind trunks and hill
    // crests): for each cell of its screen, how far ahead of `frontCamera`'s eye (the camera on that map's image; not
    // known: nothing) a point there can be and still be seen, farther being behind what the map saw there (what it
    // saw and the depth's doubt: heuristics/DepthTerrain), as frontCode() (0: nothing said there: the interface, the
    // sky, too far). A pixel (x, y) of frontCamera's screen is in cell (x times frontPerPixelX, y times
    // frontPerPixelY), rounded down, below frontColumns and frontRows; row by row. `frontFeetLevel`: the level its
    // feet were on (heightOnMap's).
    static constexpr int kFrontColumns = 320;
    static constexpr int kFrontRows = 180;
    GroundCamera frontCamera;
    float frontFeetLevel = 0.0f;
    float frontPerPixelX = 0.0f;
    float frontPerPixelY = 0.0f;
    int frontColumns = 0;
    int frontRows = 0;
    std::array<std::uint8_t, kFrontColumns * kFrontRows> front{};
};
// GroundHeights::front's codes: how far ahead of the eye (own heights) on a log scale from kFrontNearest (code 1;
// nearer is 1 too) to kFrontFarthest (255); farther is 0, nothing.
inline constexpr float kFrontNearest = 0.25f;
inline constexpr float kFrontFarthest = 400.0f;
std::uint8_t frontCode(float ahead);
float frontAhead(std::uint8_t code);
// Whether the newest map in `heights` saw something in front of the ground's map point (mapX, mapY) on `level`
// (heightOnMap's levels): a trunk, a wall, a crest the point is behind.
bool groundHidden(const GroundHeights& heights, float mapX, float mapY, float level);
// The level of `heights` at the ground's map point (mapX, mapY), between its cells; the nearest edge's outside them.
float heightOnMap(const GroundHeights& heights, float mapX, float mapY);
// How far the ground at map point (mapX, mapY) is above `camera`'s feet (own heights); 0 where `heights` is not known.
float groundHeight(const GroundHeights& heights, const GroundCamera& camera, float mapX, float mapY);
// The ground's map point `right` and `away` own heights from `camera`'s feet (mapToGround's other way).
void groundToMap(const GroundCamera& camera, float right, float away, float& mapX, float& mapY);

// The grid the Show flag Ground draws (app/Overlay, the bench's frames): cells kGridCell own heights wide (about
// 2.5 m) on the ground's map, kGridCells each way from the cell under the feet, each cell's side in kGridSteps pieces
// so it bends over the terrain (since 2026-11-16; GroundHeights' cells are a third of a grid cell).
inline constexpr float kGridCell = 1.5f;
inline constexpr int kGridCells = 20;
inline constexpr int kGridSteps = 3;
inline constexpr int kGridSide = 2 * kGridCells * kGridSteps + 1; // Points along a side.
inline constexpr std::size_t kGridPoints = static_cast<std::size_t>(kGridSide) * kGridSide;
struct GridPoint {
    float x = 0.0f; // On the screen, in pixels,
    float y = 0.0f;
    float ahead = 0.0f; // this far ahead of the camera's eye (own heights).
    float shown = 0.0f; // How strongly: fading out towards kGridCell x kGridCells from the feet (half as strong while
                        // the pitch is only presumed, GroundCamera::pitchMeasured).
    bool ok = false;    // Well ahead of the camera's eye (else not on the screen, or about to pass it).
    bool hidden = false; // Something the terrain's newest map saw stands in front of it (groundHidden).
    bool front = false;  // On the screen of the camera on that map's image, where (as x, y, ahead above):
    float frontX = 0.0f;
    float frontY = 0.0f;
    float frontAhead = 0.0f;
};
// `camera`'s grid on `heights` (may be null: the flat ground) into `out` (kGridPoints): point i along the map's x and j
// along its y is out[j * kGridSide + i]; points i and j multiples of kGridSteps are the cells' corners.
void groundGrid(const GroundCamera& camera, const GroundHeights* heights, std::span<GridPoint> out);
// The grid's lines hidden in parts (behind a trunk thinner than a piece): a piece between two points tested every
// kGridTestPixels on the screen, in at most kGridMostSteps steps.
inline constexpr float kGridTestPixels = 6.0f;
inline constexpr int kGridMostSteps = 16;
struct GridStep {
    float x = 0.0f; // On the screen, in pixels.
    float y = 0.0f;
    bool hidden = false;
};
// How many steps the piece from `a` to `b` (next to each other in groundGrid's out, both ok) is tested in: 1 when
// neither is on the screen of the terrain's newest map.
int gridStepCount(const GridPoint& a, const GridPoint& b);
// `steps` + 1 points evenly along that piece on the ground (the first a's, the last b's) into `out`: where on the
// screen, and whether hidden, as GridPoint's (`heights`: what groundGrid had them on).
void gridSteps(const GroundHeights& heights, const GridPoint& a, const GridPoint& b, int steps, std::span<GridStep> out);

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
    // The terrain the depth maps made (since 2026-11-16, ABI 13): copied into `out` if its version is not `version`
    // (true), else nothing (false).
    virtual bool heights(std::uint64_t version, GroundHeights& out) const = 0;
    // The camera as followed up to the last end(), or the last head() after it.
    virtual GroundCamera camera() const = 0;
    virtual GroundNote note() const = 0;
    // Forgets the camera: found again from the start.
    virtual void clear() = 0;
};

} // namespace observex
