#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// What a recording saves; the Record button asks (the user asked, 2026-11-02).
enum class RecordKind : std::uint8_t {
    Every,  // An image every RecordingInfo::every seconds, whatever is on the screen.
    Unsure, // When an image is due, only if the model sees it differently mirrored (Recognizer::kMirrorDisagrees).
};

// Both kinds stop by themselves after this many images, so the Stop button can show how far a recording is and how
// long is left (the user asked, 2026-11-03): about what one place needs. Every 3 s takes 5.5 minutes; only unsure
// about 8 in a new place, where two of three due images were saved.
inline constexpr unsigned kRecordImages = 110;

inline bool recordingDone(unsigned saved) {
    return saved >= kRecordImages;
}

// About how many seconds a recording of `kind` needs for the rest of its kRecordImages, `elapsed` seconds after it
// started, with `saved` saved and an image due every `every` seconds. Unsure goes at its own pace once it has saved
// a few, before that at a new place's (two of three due images saved).
double recordingSecondsLeft(RecordKind kind, unsigned every, unsigned saved, double elapsed);

// The kind's word in recording.observex and observex.cfg ("every", "unsure"), and back (Every if unknown; "auto",
// the kind of 2026-11-02 and 03 that went on to only unsure, as Unsure).
const char* recordKindId(RecordKind kind);
RecordKind parseRecordKind(std::string_view id);

// What observex knows about one recording, kept in the recording's folder as recording.observex: a few
// "key=value" lines, written when the recording starts and again when it stops, and a "reviewed-<category>" line
// when a review of it changed answers. The review lists recordings by these files, so the user picks one by when it
// was recorded instead of choosing a folder (the user asked, 2026-11-01).
struct RecordingInfo {
    std::string started;  // Local time it started: "2026-11-01 12:56:11".
    unsigned seconds = 0; // How long it ran; 0 while it runs, or if observex ended without stopping it.
    std::string screen;   // The recorded monitor's name, e.g. "Odyssey G61SH".
    unsigned width = 0;   // The screen's size in pixels.
    unsigned height = 0;
    unsigned every = 0; // Seconds between saved images (at least, unless `kind` is Every).
    RecordKind kind = RecordKind::Every;
    unsigned images = 0; // Images saved, counted when it stopped.
    // When the user last changed answers in a review of it, by category id ("character"): local time,
    // "2026-11-07 10:48:12". Saved as "reviewed-character=..." when a review closes that changed any answer, kind or
    // box (the user asked, 2026-11-07). Missing if no review changed any.
    std::map<std::string, std::string> reviewed;
};

inline constexpr char kRecordingFile[] = "recording.observex";

std::string formatRecordingInfo(const RecordingInfo& info);
// Skips comments, unknown keys and malformed lines, so older and newer versions read each other's files.
RecordingInfo parseRecordingInfo(std::string_view text);

// Writes <folder>/recording.observex in one step (a crash never leaves it half written). False if it could not.
bool writeRecordingInfo(const std::filesystem::path& folder, const RecordingInfo& info);
// Reads <folder>/recording.observex; an empty RecordingInfo if there is none.
RecordingInfo readRecordingInfo(const std::filesystem::path& folder);

// Recording file text with `key` set to `value`: the key's line replaced, or a line added at the end. Every other
// line stays as it is.
std::string setRecordingValue(std::string_view text, std::string_view key, std::string_view value);
// Saves in <folder>/recording.observex that the user changed `category`'s answers at `time` ("2026-11-07 10:48:12"),
// in one step, keeping every other line. False if it could not, or if the folder holds no recording file.
bool writeReviewed(const std::filesystem::path& folder, std::string_view category, std::string_view time);

struct RecordingEntry {
    std::filesystem::path folder;
    RecordingInfo info;
};

// The recordings directly inside `root` (the folders holding recording.observex), newest first.
std::vector<RecordingEntry> listRecordings(const std::filesystem::path& root);

// The image files of a recording (.jpg, .jpeg or .png), by file name (UTF-8), in name order. Empty if there are none.
std::vector<std::string> listImages(const std::filesystem::path& folder);

} // namespace observex
