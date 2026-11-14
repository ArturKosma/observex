#pragma once

#include "core/Recordings.h"

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace observex {

// What the user can set, kept in observex.cfg next to observex.exe.
struct Settings {
    std::filesystem::path recordingsFolder; // Each recording gets its own folder inside it.
    std::filesystem::path brainsFolder;     // The State tile's saved trees (.brain files) go into it.
    std::string brainName;                  // The tree last saved or loaded there, loaded again at start; empty: none.
    bool showCharacters = true;             // Show flag: draw the characters found on the observed screen.
    bool showKinds = true; // Show flag: say which kind each one is (monster, player, ...), with a model that knows.
    bool showUi = false;   // Show flag: highlight the game's interface the UI finder found (engine/UiFinder).
    // The tabs under the top bar, which tiles are shown (app/App): Live (the observed screen, a review or a clip),
    // Stats (app/StatsPanel: the numbers read from the screen) and State (app/StateTreeView: the bot's brain).
    bool showLive = true;
    bool showGameStats = false;
    bool showState = false;
    // Skills (app/SkillsView: the profile's skills), closed at first (the user asked, 2026-11-14), covering Stats and State
    // while open.
    bool showSkills = false;
    bool showLog = false; // The tabs' last: Log (app/LogView: what observex did, core/Log).
    bool showGround = false; // Show flag: the ground plane the ground finder follows (engine/GroundFinder).
    bool showDepth = false;  // Show flag: how near each pixel is, the depth finder's map (engine/DepthFinder).
    bool liveLog = true;   // Recordings also log what live recognition sees (character-live.txt).

    // Reads `file` (one "key=value" per line, UTF-8). A missing file or key keeps the default.
    static Settings load(const std::filesystem::path& file);
    // Writes the settings to `file`. Returns false if it could not be written.
    bool save(const std::filesystem::path& file) const;
};

// The folder observex.exe is in.
std::filesystem::path appFolder();

// The project folder: the nearest folder, from observex.exe's up, that has the training tool (tools/train). Empty if
// there is none. observex.exe lives in the project folder; the Debug build further down, in build/. The trained
// models are in its models/ folder.
std::filesystem::path projectFolder();

// Where the trained model for `category` (e.g. "character") is: models/<category> in the project folder, or next to
// observex.exe if there is no project folder.
std::filesystem::path modelFolder(const char* category);

// Where recordings go unless the user picks another folder: "recordings" next to observex.exe.
std::filesystem::path defaultRecordingsFolder();
// Where the State tile's trees go unless the user picks another folder: "brains" next to observex.exe.
std::filesystem::path defaultBrainsFolder();

// Shows Windows' folder picker titled `title`, starting in `start`. Returns the chosen folder, or nothing if
// cancelled.
std::optional<std::filesystem::path> pickFolder(HWND owner, const std::filesystem::path& start, const wchar_t* title);

// Opens `folder` in File Explorer, creating it first if it does not exist yet.
void showInExplorer(const std::filesystem::path& folder);

} // namespace observex
