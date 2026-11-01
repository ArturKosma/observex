#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// What observex knows about one recording, kept in the recording's folder as recording.observex: a few
// "key=value" lines, written when the recording starts and again when it stops. The review lists recordings by
// these files, so the user picks one by when it was recorded instead of choosing a folder (the user asked,
// 2026-11-01).
struct RecordingInfo {
    std::string started;  // Local time it started: "2026-11-01 12:56:11".
    unsigned seconds = 0; // How long it ran; 0 while it runs, or if observex ended without stopping it.
    std::string screen;   // The recorded monitor's name, e.g. "Odyssey G61SH".
    unsigned width = 0;   // The screen's size in pixels.
    unsigned height = 0;
    unsigned every = 0;  // Seconds between saved images.
    unsigned images = 0; // Images saved, counted when it stopped.
};

inline constexpr char kRecordingFile[] = "recording.observex";

std::string formatRecordingInfo(const RecordingInfo& info);
// Skips comments, unknown keys and malformed lines, so older and newer versions read each other's files.
RecordingInfo parseRecordingInfo(std::string_view text);

// Writes <folder>/recording.observex in one step (a crash never leaves it half written). False if it could not.
bool writeRecordingInfo(const std::filesystem::path& folder, const RecordingInfo& info);

struct RecordingEntry {
    std::filesystem::path folder;
    RecordingInfo info;
};

// The recordings directly inside `root` (the folders holding recording.observex), newest first.
std::vector<RecordingEntry> listRecordings(const std::filesystem::path& root);

} // namespace observex
