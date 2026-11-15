#pragma once

#include "app/Theme.h"
#include "core/GameStats.h"
#include "core/Geodata.h"
#include "core/Ground.h"
#include "core/UiLayout.h"
#include "core/WorldPosition.h"

#include <imgui.h>

#include <filesystem>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace observex {

// The Show flag Geodata (the user asked, 2026-11-15: "a Geodata flag in the live view ... and if the system is not
// sure where i am it writes in the middle of the screen type /loc and it keeps watching the chat window ingame to read
// it and sync"; "the flag draws the whole geodata as grid that matches the world"). Where the character is
// (core/WorldPosition, fed every reading of the stats reader: the minimap followed and the /loc answers read) is kept
// whatever the flag says; the flag draws the geodata's ground round it (core/Geodata's GeoField, built on a thread of
// its own when the character comes near its edge) as a grid fixed to the world, as the ground finder's camera sees it,
// coloured by height against the feet (level lime, lower cyan, higher orange), cells something stands in or no one can
// walk through filled red, a ring round the feet; and in the middle of the screen what to do while it is not sure
// where the character is ("Type /loc", or "Open the minimap"). On the UI thread.
class GeodataView {
public:
    explicit GeodataView(std::filesystem::path folder) : geodata_(std::move(folder)) {}
    ~GeodataView();
    GeodataView(const GeodataView&) = delete;
    GeodataView& operator=(const GeodataView&) = delete;

    // A new reading of the stats reader (`timeMs` when its image was shown): follows the character. Lines for the log
    // go into `log`.
    void read(const GameStats& stats, double timeMs, std::vector<std::string>& log);
    // Draws the grid over the screen image drawn from `min` to `max` (the image `imageWidth` x `imageHeight` pixels),
    // as `camera` sees it (unknown: no grid), leaving the interface's `pieces` clear, and in its middle what to do
    // when it is not sure where the character is. `timeMs` now, on the clock read() is given.
    void draw(ImDrawList* drawList, const theme::Fonts& fonts, ImVec2 min, ImVec2 max, float imageWidth,
              float imageHeight, const GroundCamera& camera, std::span<const UiPiece> pieces, double timeMs,
              float dpiScale);

    const WorldPosition& position() const { return position_; }
    // The minimap's scale (game units a pixel), as observex.cfg keeps it.
    float scale() const { return position_.scale(); }
    void setScale(float scale) { position_.setScale(scale); }

    // A doubt shows only once it has lasted this long (the minimap is not followed for an image or two now and then).
    static constexpr double kDoubtShowMs = 700.0;
    // A new field once the character is this near its edge (units): the grid reaches 4,032.
    static constexpr double kFieldMargin = 4200.0;

private:
    // The field round the character, built anew when it comes near the edge (on a thread of its own).
    void keepField();

    Geodata geodata_;
    WorldPosition position_;
    std::uint32_t lastCount_ = 0; // The last /loc answer taken (its count).
    std::shared_ptr<const GeoField> field_;
    std::future<std::shared_ptr<const GeoField>> building_;
    WorldPosition::Doubt shownDoubt_ = WorldPosition::Doubt::NoLocation;
    double doubtSinceMs_ = 0.0;
    GeoGrid grid_; // Working memory, kept from frame to frame.
};

} // namespace observex
