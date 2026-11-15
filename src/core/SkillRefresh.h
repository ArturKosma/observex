#pragma once

#include "core/CursorGlide.h"
#include "core/GameStats.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// What Windows OCR's text of the area round a hovered skill says of it (readMpCost).
struct MpCostRead {
    bool name = false; // The skill's name is in the text (a letter or two read wrong allowed, as sameName).
    int mp = -1;       // The MP it costs, as written by "MP" ("MP Consumption 10", "Consumes 10 MP"); -1: none.
};

// Reads the MP cost of the skill named `skillName` off `text`, Windows OCR's words of its tooltip and what is round it
// (one line, words joined by spaces). The number nearest after an "MP" (words between it and the number allowed, as
// "Consumption"), else just before one ("10 MP"); never one with a '/' or '%' (the Status bar's "90/108"). OCR's
// usual slips in a number (O for 0, l or I for 1) are taken as the digits.
MpCostRead readMpCost(std::string_view text, std::string_view skillName);

// The part of a `width` x `height` screen image to read the tooltip of a skill whose miniature is at `slot`: round it,
// mostly above (the hotbar is at the screen's bottom, so the game puts its tooltips above it), kept on the image.
StatsArea tooltipArea(const StatsArea& slot, int width, int height);

// A skill of the profile for Refresh, as the newest stats reading sees it on the hotbar: where its miniature's middle
// is on the desktop (GetCursorPos' pixels) and its side there, and where the miniature is on the screen image.
struct RefreshSkill {
    std::string name;
    bool seen = false; // On the hotbar on the newest reading (its place below is then that reading's).
    int x = 0;
    int y = 0;
    int side = 0;
    StatsArea at{};
};

// The Skills tab's Refresh (the user asked, 2026-11-15: "takes control over mouse, hovers over every registered skills
// by first finding their icon on the skill bar, and copies the mana cost"). One step a UI frame:
// 1. For each skill of the profile, in its order: onto its miniature's middle on the hotbar (where a reading last saw
//    it; not seen within kFindSeconds: skipped), the cursor eased as Select Target's (core/CursorGlide).
// 2. Held there; from kTooltipDelay after it came (the game shows a tooltip after a moment) the area round it
//    (tooltipArea) is read by Windows OCR on each next image, one read at a time.
// 3. Two reads in a row with the skill's name and the same MP cost: learned. Its name twice with no MP: none written
//    on its tooltip (its cost kept). Nothing kReadSeconds after it came: given up on (the log says what was read).
// 4. After the last, back to where the cursor was when it began; done.
class SkillRefresher {
public:
    struct Input {
        double now = 0.0; // Seconds, the clock `shown` is on.
        double dt = 0.0;  // Since the step before.
        int cursorX = 0;  // On the desktop.
        int cursorY = 0;
        int imageWidth = 0; // The screen image's size.
        int imageHeight = 0;
        std::span<const RefreshSkill> skills; // The profile's, in its order; the same ones on every step.
        // The answer to the read last asked for (Output::read), once it came: the text read, and when the screen showed
        // the image it was read on. Given on one step only.
        bool answered = false;
        double shown = 0.0;
        std::string_view text;
    };
    struct Output {
        int dx = 0; // The cursor's move, desktop pixels.
        int dy = 0;
        bool read = false; // Read `area` of the next screen image by OCR.
        StatsArea area{};
        bool learned = false; // `name`'s MP cost was read: `mp`.
        std::string name;
        int mp = 0;
        bool done = false;
        std::string log; // A line for the log when something happened; empty if nothing did.
    };

    static constexpr double kFindSeconds = 0.6;   // A skill not seen on the hotbar this long is skipped.
    static constexpr double kTooltipDelay = 0.5;  // Held on a skill this long before its tooltip is read.
    static constexpr double kReadSeconds = 3.0;   // On a skill this long with nothing learned: given up on.
    static constexpr double kReturnSeconds = 1.0; // Going back to where the cursor was at most this long.

    // Starts afresh, the cursor at `cursorX`, `cursorY` (where it goes back to at the end).
    void start(int cursorX, int cursorY);
    Output step(const Input& in);

    bool running() const { return running_; }
    // How far it is: the skills done of all (0 to 1).
    float progress() const { return total_ > 0 ? static_cast<float>(index_) / static_cast<float>(total_) : 0.0f; }

private:
    void next(double now);

    bool running_ = false;
    bool returning_ = false;
    int homeX_ = 0;
    int homeY_ = 0;
    std::size_t index_ = 0;  // The skill it is on.
    std::size_t total_ = 0;
    int learned_ = 0;        // Skills whose cost was read.
    std::vector<std::string> missed_; // Names, for the last line.
    double since_ = -1.0;    // On this skill (or going back) since then; -1: from the next step.
    bool placed_ = false;    // Its place on the hotbar is known (seen on a reading).
    RefreshSkill place_{};   // As last seen.
    double cameAt_ = -1.0;   // The cursor came onto it then; -1: not on it.
    bool asked_ = false;     // A read asked for and not yet answered.
    double askedAt_ = 0.0;
    int lastMp_ = -2;        // The read before: its MP (-1 none) with the name seen; -2: none yet.
    std::string lastText_;   // The newest text read on this skill, for the log.
    CursorGlide glide_;
};

} // namespace observex
