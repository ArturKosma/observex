#pragma once

#include "core/Detection.h"
#include "core/UiLayout.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// One gauge of the game's interface as read (core/GameStats' StatsReading): how full it is and what is written on it.
// Three kinds, by stats.txt's lines:
// - A bar ("bar"): name e.g. "HP", full how full, text the number on it.
// - The cast bar ("cast", piece "Casting"): name "Casting", full how far the cast is, text the added skill being cast
//   (the one whose miniature went on cooldown last), empty if not known.
// - An added skill ("skills", piece e.g. "Hotbar", one gauge a skill): name the skill's, seen if its miniature was
//   found, full the share of its cooldown left (0: ready; always 0 for a skill without a cooldown), text the seconds
//   written on it, empty if none was read; pressed while its miniature shows the flash after its key is pressed (dark,
//   framed light blue, for half a second or so, before the cast begins: neither ready nor cooling).
// - What the model sees around (piece "Nearby", put in by the app, not read off the interface): names "Characters"
//   (every character on screen but you) and "Monsters", text the count.
struct GameGauge {
    std::array<char, 16> piece{}; // The piece it is in (ui.txt's name, e.g. "Status").
    std::array<char, 32> name{};  // stats.txt's name, e.g. "HP", or the skill's.
    bool seen = false;            // It was there to read (its piece found, its bar or its number seen).
    bool pressed = false;         // A skill: its miniature shows the flash after its key was pressed.
    float full = -1.0f;           // 0 to 1: how full it is (its number's share when one was read); below 0 if
                                  // that could not be seen (no piece, or a window over the bar).
    std::array<char, 32> text{};  // What is written on it as read, e.g. "238/256" or "26.64%"; empty if none was read.
    std::array<char, 32> doubted{}; // A number read that did not agree with the bar (so not in text); empty if none.
};

// The pieces and names of the gauges that are not bars (stats.txt's lines name them; the state tree's conditions read
// them by these).
inline constexpr char kCastingPiece[] = "Casting"; // The cast bar: piece and name "Casting".
inline constexpr char kSkillsPiece[] = "Hotbar";   // The added skills: piece "Hotbar", each named as the skill.
inline constexpr char kTargetPiece[] = "Target";   // The target's name: piece "Target", name "Name".
inline constexpr char kTargetNameGauge[] = "Name";
inline constexpr char kNearbyPiece[] = "Nearby";   // The model's count: names "Characters" and "Monsters".
inline constexpr char kCharactersGauge[] = "Characters";
inline constexpr char kMonstersGauge[] = "Monsters";

// A skill the user added (the Skills tab, kept in the profile), as the stats reader gets it: its miniature when added
// (ready), grey (core/Heuristics' grey), side x side.
struct SkillPicture {
    std::array<char, 32> name{};
    bool cooldown = true; // It has a cooldown to read (the user asked, 2026-11-14: "Has Cooldown?").
    int side = 0;
    const std::uint8_t* grey = nullptr;
};

// What observex read from the game's interface on one screen image: every gauge stats.txt names, in its order (the
// user asked, 2026-11-12: numbers to feed a state machine, recorded into clips so a saved clip shows them).
struct GameStats {
    static constexpr int kMax = 24;
    std::uint64_t image = 0;   // The screen image (ImageStamp::number); 0: none read yet.
    std::int64_t shownQpc = 0; // When the screen showed it.
    float readMs = 0.0f;       // How long reading them took on the CPU.
    int count = 0;
    std::array<GameGauge, kMax> gauges{};
    // The target, without looking through the gauges (the state tree's Has Target and Target Name): whether its window
    // shows, and its name as last read while it does (kept while the same target stays, so it is there on every image
    // though a name is read only when it changes); empty if not known. From a clip's line: any Target gauge seen, and
    // the Target Name gauge's text.
    bool hasTarget = false;
    std::array<char, 32> targetName{};
    // The Target Changed event (the user asked, 2026-11-13, for the log): `target` is the target as None (empty) or its
    // name, changed only once a new one held on two readings in a row (a window lost for one image, or a name not yet
    // read, says nothing); `targetChanged` is true on the reading it changed. A reader that may skip readings (the UI
    // takes the newest) compares `target` with the one it saw before instead. From a clip's line: the target window's
    // name as read there (empty while none shows), changed on no reading.
    std::array<char, 32> target{};
    bool targetChanged = false;
    // Why what is not seen is not (the user asked, 2026-11-14: "be very verbal in your ring buffer", so a clip saved
    // when it happens says), e.g. "Target: no window (target-left-up 0.90 at 1076,0 < 0.96) | Wind Strike: not at
    // 1021,1365 (far 34.1, alike 0.35), hotbar looked over: 6 miniatures, nearest 29.2"; empty when all is seen.
    // Written into clips' clip-stats.txt as "why" lines.
    std::array<char, 512> why{};

    // The gauge named `name` in piece `piece`, or null.
    const GameGauge* find(std::string_view piece, std::string_view name) const;
};

// A part of a screen image to read, in screen pixels (x1 and y1 just past the last).
struct StatsArea {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

// The pixels of one StatsArea: BGRA (ScreenCapture::kImageFormat), rows `pitch` bytes apart.
struct ColourPart {
    StatsArea area;
    const std::uint8_t* bgra = nullptr;
    std::size_t pitch = 0;
};

// Reading the game's bars and the numbers on them, on the pieces of its interface the UI finder found (core/UiLayout).
// The rules are heuristics/StatsReader, by models/ui/stats.txt; the app reads through observex_stats.dll, which it
// loads again whenever it is rebuilt (engine/StatsLibrary), so a change to them shows in the running app, live and on
// an open clip next to what was read when it was recorded. Use one from one thread.
class StatsReading {
public:
    virtual ~StatsReading() = default;

    // The parts of the image read() needs on an image with `pieces` (a few small ones), inside a `width` x `height`
    // image. Writes up to `max` into `out`, returns how many.
    virtual int areas(std::span<const UiPiece> pieces, int width, int height, StatsArea* out, int max) const = 0;
    // Reads every bar on the image with `pieces` from `parts` (the areas() asked for, in any order): into `out`'s
    // gauges (count and gauges; the image, time and readMs are the caller's).
    virtual void read(std::span<const UiPiece> pieces, std::span<const ColourPart> parts, GameStats& out) = 0;
    // The skills to look for from the next image on (copied; none until given).
    virtual void setSkills(std::span<const SkillPicture> skills) = 0;
};

// A line of a clip's clip-stats.txt (kClipStatsFile) with what was read on screen image IMAGE (the UI finder reads 10
// a second), shown MS milliseconds before the clip was saved (so below 0):
//   stats IMAGE MS [PIECE NAME FULL TEXT]...
// FULL "-" if how full the bar is was not seen, TEXT "-" if nothing was read ("pressed" for a skill pressed); a space
// in NAME or TEXT is written "_"; e.g.
//   stats 812 -5021.4 Status HP 0.9262 238/256 Status XP 0.2646 26.64% Casting Casting 0.4100 Wind_Strike
//   Hotbar Wind_Strike 0.7500 5 Target HP - -
// and, when GameStats::why says anything, a line after it (read back into why):
//   why 812 Target: no window (...)
void appendStatsLine(std::string& text, std::uint64_t image, double ms, const GameStats& stats);

// The Nearby gauges (kNearbyPiece; the user asked, 2026-11-14: "read from the model number of currently visible
// characters") added to `stats`, unless it has them: Characters, how many `things` the model shows (the own character
// left out), and Monsters, how many of them it can tell are monsters (core/Detection's cantTellKind); their text the
// count. `kinds` are the model's kinds by number ("monster", "own", ...; none: Monsters is not seen). `seen` false:
// nothing is looked at (both not seen).
void addNearby(GameStats& stats, bool seen, std::span<const Tracked> things, std::span<const std::string> kinds);
// One such line back (with or without its newline); false if it is not one.
bool readStatsLine(std::string_view line, std::uint64_t* image, double* ms, GameStats* stats);

// The file in a clip's folder with what was read while it was recorded (clips since 2026-11-12 evening): its
// clip.txt's frame lines say which screen image each frame is, so a frame's stats are the last read on that image or
// one before it.
inline constexpr char kClipStatsFile[] = "clip-stats.txt";

// What a clip-stats.txt says was read, in image order.
struct ClipStats {
    double ms = 0.0;
    GameStats stats; // Its image too.
};
// The stats lines of `text` (a clip-stats.txt's whole text); other lines are left out.
std::vector<ClipStats> readClipStats(std::string_view text);
// The last of `lines` read on screen image `image` or one before it, or null.
const ClipStats* statsAt(const std::vector<ClipStats>& lines, std::uint64_t image);

} // namespace observex
