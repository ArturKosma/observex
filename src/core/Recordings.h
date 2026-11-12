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

// Every stops by itself after this many images, so the Stop button can show how far a recording is and how long is
// left (the user asked, 2026-11-03): about what one place needs, 5.5 minutes at one every 3 s.
inline constexpr unsigned kRecordImages = 110;
// Only unsure records for this long, asking once a second (kUnsureEvery) and saving the image only when the model is
// unsure of it, so at most one image a second and none while it is sure of everything (the user asked, 2026-11-12;
// before, it went on until kRecordImages, at most one every 3 s).
inline constexpr unsigned kUnsureSeconds = 60;
inline constexpr unsigned kUnsureEvery = 1;

// Whether a recording of `kind` is done, with `saved` images saved `elapsed` seconds after it started: Every after
// kRecordImages, Unsure after kUnsureSeconds.
bool recordingDone(RecordKind kind, unsigned saved, double elapsed);

// About how many seconds are left of a recording of `kind`, `elapsed` seconds after it started, with `saved` saved
// and an image due every `every` seconds.
double recordingSecondsLeft(RecordKind kind, unsigned every, unsigned saved, double elapsed);

// How far a recording of `kind` is, 0 to 1, as recordingDone counts.
float recordingProgress(RecordKind kind, unsigned saved, double elapsed);

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

// A clip: the last kClipSeconds of the observed screen as the user saw it, kept in memory all the time while a screen
// is observed (engine/ClipRecorder) and saved when the user asks (2026-11-11: the "Save last 10 s" button), so what
// heuristics would have done can later be compared on the same seconds. Each clip is a folder in
// the clips folder (kClipsFolder, inside the recordings folder), named by when it was saved, holding:
//   clip.mp4       the screen as captured, nothing drawn on it (H.264, a frame each time the screen changed);
//   clip.txt       everything the recogniser saw and followed on every look, the frames, and what else happened;
//   picture.jpg    the screen when it was saved, for the review's list of clips;
//   clip.observex  a few key=value lines (ClipInfo), by which the review lists the clips.
inline constexpr double kClipSeconds = 10.0;
// The recogniser's looks are kept from this long before the clip's first frame too (clip.txt has them at negative
// times), so a replay of the looks through the heuristics (core/ClipGuesses) has caught up with what the app followed
// by the first frame. On a 10 s fight (2026-11-11) a replay from nothing drew a few boxes differently for up to 3.7 s.
inline constexpr double kClipWarmUpSeconds = 5.0;
inline constexpr char kClipsFolder[] = "clips";
inline constexpr char kClipFile[] = "clip.observex";
inline constexpr char kClipVideo[] = "clip.mp4";
inline constexpr char kClipText[] = "clip.txt";
inline constexpr char kClipPicture[] = "picture.jpg";

struct ClipInfo {
    std::string saved;    // Local time it was saved: "2026-11-10 18:22:05".
    double seconds = 0.0; // From its first frame to the save. The last frame is a moment before it (at most half a
                          // second: a still screen's image is repeated that often).
    unsigned frames = 0;  // Video frames.
    unsigned looks = 0;   // The recogniser's looks in it.
    std::string screen;   // The monitor's name, e.g. "Odyssey G61SH".
    unsigned width = 0;   // The screen's size in pixels (the video's).
    unsigned height = 0;
    std::string model; // The model the recogniser ran, e.g. "...\models\character\model.onnx (written 2026-11-10 00:18)".
};

std::string formatClipInfo(const ClipInfo& info);
// Skips comments, unknown keys and malformed lines.
ClipInfo parseClipInfo(std::string_view text);
// Writes <folder>/clip.observex in one step. False if it could not.
bool writeClipInfo(const std::filesystem::path& folder, const ClipInfo& info);
// Reads <folder>/clip.observex; an empty ClipInfo if there is none.
ClipInfo readClipInfo(const std::filesystem::path& folder);

struct ClipEntry {
    std::filesystem::path folder;
    ClipInfo info;
};

// The clips directly inside `clipsFolder` (the folders holding clip.observex), newest first.
std::vector<ClipEntry> listClips(const std::filesystem::path& clipsFolder);

} // namespace observex
