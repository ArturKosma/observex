#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>

namespace observex {

// What the user can set, kept in observex.cfg next to observex.exe.
struct Settings {
    std::filesystem::path recordingsFolder; // Each recording gets its own folder inside it.

    // Reads `file` (one "key=value" per line, UTF-8). A missing file or key keeps the default.
    static Settings load(const std::filesystem::path& file);
    // Writes the settings to `file`. Returns false if it could not be written.
    bool save(const std::filesystem::path& file) const;
};

// The folder observex.exe is in.
std::filesystem::path appFolder();

// Where recordings go unless the user picks another folder: "recordings" next to observex.exe.
std::filesystem::path defaultRecordingsFolder();

// Shows Windows' folder picker titled `title`, starting in `start`. Returns the chosen folder, or nothing if
// cancelled.
std::optional<std::filesystem::path> pickFolder(HWND owner, const std::filesystem::path& start, const wchar_t* title);

// Opens `folder` in File Explorer, creating it first if it does not exist yet.
void showInExplorer(const std::filesystem::path& folder);

} // namespace observex
