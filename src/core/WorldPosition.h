#pragma once

#include "core/GameStats.h"

#include <cstdint>
#include <string>

namespace observex {

// Where the user's character is in the game's world, for the Geodata flag (the user asked, 2026-11-15: "a single /loc
// doesnt tell you anything when im moving"; "if the system is not sure where i am it writes in the middle of the
// screen type /loc and it keeps watching the chat window ingame to read it and sync").
//
// A /loc answer (core/GameStats' LocationRead) says where it is. From there the minimap says how far it walked: its
// north-up map slides the other way (MinimapRead), times the minimap's scale, game units a pixel. The scale is learned
// from two answers with the map followed between them (the walk between them over how far the map slid, if the map
// slid at least kLearnPixels the way the character walked), so the first walk after the first /loc asks for a second
// one; it is then kept (the app saves it in observex.cfg). It is sure while the map is followed in the run the last
// answer was read in; else it says why not (Doubt), and the app asks for /loc. Height: the app sets it from the
// geodata's floor there (settle). Platform-free; use one from one thread.
class WorldPosition {
public:
    enum class Doubt : std::uint8_t {
        None,
        NoLocation,     // No /loc read yet.
        Moved,          // It walked since the last /loc, and the minimap's scale is not known yet.
        MinimapLost,    // The minimap's map was not followed since the last /loc (covered, zoomed, a teleport).
        MinimapHidden,  // The minimap is not seen.
    };
    struct Spot {
        bool known = false; // Located at least once (x, y, z are the last known, also while in doubt).
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        Doubt doubt = Doubt::NoLocation;
    };

    static constexpr float kLearnPixels = 40.0f; // The map slid at least this far between two answers: the scale.
    static constexpr double kLearnWalk = 150.0;  // And the character walked at least this far (units).
    static constexpr float kStillPixels = 2.0f;  // Without a scale: the map within this of where it was: not moved.
    static constexpr double kLearnAngle = 0.94;  // The walk and the map's slide within ~20 degrees (cosine).

    // A new answer (its count new); returns a line for the log (the answer, how far it was from where the minimap
    // said, the scale learned).
    std::string located(const LocationRead& read);
    // The minimap on the newest reading.
    void minimap(const MinimapRead& map);
    // The floor's height where it is now (the geodata's).
    void settle(double z) { spot_.z = z; }

    const Spot& spot() const { return spot_; }
    bool sure() const { return spot_.doubt == Doubt::None; }
    // Game units a pixel of the minimap's map; 0 while not known.
    float scale() const { return scale_; }
    void setScale(float scale) {
        scale_ = scale;
        weight_ = scale > 0.0f ? 400.0f : 0.0f;
    }

private:
    Spot spot_;
    LocationRead anchor_{}; // The last answer taken (count 0: none).
    float scale_ = 0.0f;
    float weight_ = 0.0f; // How many pixels of slide the scale was measured over (a new one counts by its own).
};

} // namespace observex
