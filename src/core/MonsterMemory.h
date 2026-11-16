#pragma once

#include "core/Ground.h"

#include <cstdint>
#include <span>
#include <vector>

namespace observex {

// A monster on one look: its tracker id and where its feet stand on the ground, right and away of the own character's
// feet (own heights, core/Distances' groundUnder) as the camera looked then.
struct MonsterSighting {
    std::uint32_t id = 0;
    float right = 0.0f;
    float away = 0.0f;
};

// The monsters seen round the own character, kept while the camera turns away from them (the user asked, 2026-11-16:
// "before we decide to select a monster, we first rotate around, remember all monsters and their distances, and then
// decide on the closest"). Each is kept where it stands round the own character's feet, turned by the camera's turn
// when it was seen (core/MouseCamera's counts: exact to about a tenth of a degree) into a frame that does not turn with
// the camera, so the camera can come back round to it. It is fed by every look while the camera is known, the Look
// Around task forgets it all first (the own character may have walked since), and Select Target goes for the nearest.
// - The same monster: the same tracker id, or (the tracker lost it and found it again, the camera came back round) a
//   sighting within kMatchNear own heights or kMatchShare of its distance of where one not seen on this look stands.
// - Forgotten: its spot in view (on the screen, ahead, within kSureView) and nothing found there for kGoneSeconds
//   (killed, walked off, never there), or not seen for kForgetSeconds, or forget().
// - Given up on (giveUp, kGiveUps times by Select Target, or at once): kept, so it is not found again as a new one, but
//   no longer counted or offered, until clear().
class MonsterMemory {
public:
    struct Monster {
        std::uint32_t key = 0; // Its own, never reused.
        std::uint32_t id = 0;  // The tracker id it was last seen with.
        float x = 0.0f;        // Where it stands from the own character's feet (own heights): right and ahead of the
        float y = 0.0f;        // camera's heading at turn 0.
        double seenAt = 0.0;   // Seconds, the clock look() is given.
        double missingSince = -1.0; // In view and not found since then (-1: seen, or not in view).
        int giveUps = 0;
        bool givenUp = false;
        float distance() const;
    };

    // One look at `now` (seconds): the camera turned `heading` radians (+ right; MouseCamera::View::turned) and `camera`
    // its ground camera (pitch, distance, lens) on a screen `width` x `height`, `seen` the monsters it shows.
    void look(double now, double heading, const GroundCamera& camera, float width, float height,
              std::span<const MonsterSighting> seen);
    void clear(); // Keys go on counting.
    // Select Target gave up on it: given up on after kGiveUps (`now`: at once).
    void giveUp(std::uint32_t key, bool now = false);
    void forget(std::uint32_t key);

    const std::vector<Monster>& monsters() const { return monsters_; }
    const Monster* withKey(std::uint32_t key) const;
    const Monster* withId(std::uint32_t id) const; // The one last seen with tracker id `id`, or null.
    int count() const;                 // Those not given up on.
    const Monster* nearest() const;    // Of those not given up on; null if none.
    // Where `monster` is as the camera turned `heading` looks: right and away of the own feet (own heights), and its
    // bearing (radians, + right of straight ahead, -pi to pi).
    static void relative(const Monster& monster, double heading, float& right, float& away);
    static float bearing(const Monster& monster, double heading);

    static constexpr float kMatchNear = 1.5f;   // Own heights.
    static constexpr float kMatchShare = 0.25f; // Of its distance (the distances are about 19% off, 2026-11-16).
    static constexpr double kGoneSeconds = 1.0;
    static constexpr double kForgetSeconds = 30.0;
    static constexpr float kSureView = 30.0f;   // Own heights (1410 game units): farther, the model may miss it.
    static constexpr float kViewMargin = 0.06f; // Of the screen's width and height kept off each edge.
    static constexpr int kGiveUps = 2;

private:
    std::vector<Monster> monsters_;
    std::uint32_t nextKey_ = 1;
};

} // namespace observex
