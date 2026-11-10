#pragma once

#include "core/Recordings.h"

#include <windows.h>

#include <filesystem>
#include <optional>

namespace observex {

// What the user can set, kept in observex.cfg next to observex.exe.
struct Settings {
    std::filesystem::path recordingsFolder; // Each recording gets its own folder inside it.
    bool showCharacters = true;             // Show flag: draw the characters found on the observed screen.
    bool showKinds = true; // Show flag: say which kind each one is (monster, player, ...), with a model that knows.
    bool liveLog = true;       // Recordings also log what live recognition sees (character-live.txt) and take marks.
    RecordKind recordKind = RecordKind::Every; // The kind Record last started; the hotkey starts it too.

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

// Shows Windows' folder picker titled `title`, starting in `start`. Returns the chosen folder, or nothing if
// cancelled.
std::optional<std::filesystem::path> pickFolder(HWND owner, const std::filesystem::path& start, const wchar_t* title);

// Opens `folder` in File Explorer, creating it first if it does not exist yet.
void showInExplorer(const std::filesystem::path& folder);

} // namespace observex
