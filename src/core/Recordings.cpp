#include "core/Recordings.h"

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace observex {

namespace {

unsigned toUnsigned(std::string_view text) {
    unsigned value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

constexpr std::string_view kReviewedKey = "reviewed-"; // Then the category id.

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Writes `text` to `file` through a temporary file, so a crash never leaves it half written.
bool writeFileInOneStep(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    return !error;
}

} // namespace

double recordingSecondsLeft(RecordKind kind, unsigned every, unsigned saved, double elapsed) {
    const double left = saved < kRecordImages ? kRecordImages - saved : 0.0;
    if (kind == RecordKind::Every) {
        return left * every;
    }
    constexpr unsigned kOwnPaceAfter = 5;
    constexpr double kNewPlaceDuePerSaved = 1.5; // 74 due, 50 saved in the user's first new place.
    return left * (saved >= kOwnPaceAfter ? elapsed / saved : every * kNewPlaceDuePerSaved);
}

const char* recordKindId(RecordKind kind) {
    return kind == RecordKind::Unsure ? "unsure" : "every";
}

RecordKind parseRecordKind(std::string_view id) {
    return id == "unsure" || id == "auto" ? RecordKind::Unsure : RecordKind::Every;
}

std::string formatRecordingInfo(const RecordingInfo& info) {
    std::string text = "# observex recording: what observex knows about the images in this folder.\n";
    text += "started=" + info.started + "\n";
    text += "seconds=" + std::to_string(info.seconds) + "\n";
    text += "screen=" + info.screen + "\n";
    text += "size=" + std::to_string(info.width) + "x" + std::to_string(info.height) + "\n";
    text += "every=" + std::to_string(info.every) + "\n";
    text += std::string("kind=") + recordKindId(info.kind) + "\n";
    text += "images=" + std::to_string(info.images) + "\n";
    for (const auto& [category, time] : info.reviewed) {
        text += std::string(kReviewedKey) + category + "=" + time + "\n";
    }
    return text;
}

RecordingInfo parseRecordingInfo(std::string_view text) {
    RecordingInfo info;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::size_t equals = line.find('=');
        if (line.empty() || line.front() == '#' || equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, equals);
        const std::string_view value = line.substr(equals + 1);
        if (key == "started") {
            info.started = value;
        } else if (key == "seconds") {
            info.seconds = toUnsigned(value);
        } else if (key == "screen") {
            info.screen = value;
        } else if (key == "size") {
            const std::size_t x = value.find('x');
            if (x != std::string_view::npos) {
                info.width = toUnsigned(value.substr(0, x));
                info.height = toUnsigned(value.substr(x + 1));
            }
        } else if (key == "every") {
            info.every = toUnsigned(value);
        } else if (key == "kind") {
            info.kind = parseRecordKind(value);
        } else if (key == "unsure" && value == "1") { // Written on 2026-11-02 only, before kind.
            info.kind = RecordKind::Unsure;
        } else if (key == "images") {
            info.images = toUnsigned(value);
        } else if (key.starts_with(kReviewedKey) && key.size() > kReviewedKey.size()) {
            info.reviewed[std::string(key.substr(kReviewedKey.size()))] = value;
        }
    }
    return info;
}

bool writeRecordingInfo(const std::filesystem::path& folder, const RecordingInfo& info) {
    return writeFileInOneStep(folder / kRecordingFile, formatRecordingInfo(info));
}

RecordingInfo readRecordingInfo(const std::filesystem::path& folder) {
    return parseRecordingInfo(readFile(folder / kRecordingFile));
}

std::string setRecordingValue(std::string_view text, std::string_view key, std::string_view value) {
    const std::string keyLine = std::string(key) + "=" + std::string(value) + "\n";
    std::string result;
    bool set = false;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end == std::string_view::npos ? end : end + 1); // With its \n.
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.starts_with(key) || !line.substr(key.size()).starts_with('=')) {
            result += line;
        } else if (!set) { // The key's line. A second one is dropped.
            result += keyLine;
            set = true;
        }
    }
    if (!set) {
        result += result.empty() || result.back() == '\n' ? "" : "\n";
        result += keyLine;
    }
    return result;
}

bool writeReviewed(const std::filesystem::path& folder, std::string_view category, std::string_view time) {
    const std::filesystem::path file = folder / kRecordingFile;
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) {
        return false;
    }
    const std::string key = std::string(kReviewedKey) + std::string(category);
    return writeFileInOneStep(file, setRecordingValue(readFile(file), key, time));
}

std::vector<RecordingEntry> listRecordings(const std::filesystem::path& root) {
    std::vector<RecordingEntry> recordings;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const std::filesystem::path file = it->path() / kRecordingFile;
        std::error_code missing;
        if (!it->is_directory(missing) || !std::filesystem::is_regular_file(file, missing)) {
            continue;
        }
        recordings.push_back({it->path(), parseRecordingInfo(readFile(file))});
    }
    std::sort(recordings.begin(), recordings.end(), [](const RecordingEntry& a, const RecordingEntry& b) {
        return a.info.started != b.info.started ? a.info.started > b.info.started : a.folder > b.folder;
    });
    return recordings;
}

std::vector<std::string> listImages(const std::filesystem::path& folder) {
    std::vector<std::string> images;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        std::wstring extension = it->path().extension().wstring();
        for (wchar_t& c : extension) {
            c = static_cast<wchar_t>(std::towlower(c));
        }
        if (extension == L".jpg" || extension == L".jpeg" || extension == L".png") {
            const std::u8string name = it->path().filename().u8string();
            images.emplace_back(reinterpret_cast<const char*>(name.data()), name.size());
        }
    }
    std::sort(images.begin(), images.end());
    return images;
}

} // namespace observex
