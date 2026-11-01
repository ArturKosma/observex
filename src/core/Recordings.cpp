#include "core/Recordings.h"

#include <algorithm>
#include <charconv>
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

} // namespace

std::string formatRecordingInfo(const RecordingInfo& info) {
    std::string text = "# observex recording: what observex knows about the images in this folder.\n";
    text += "started=" + info.started + "\n";
    text += "seconds=" + std::to_string(info.seconds) + "\n";
    text += "screen=" + info.screen + "\n";
    text += "size=" + std::to_string(info.width) + "x" + std::to_string(info.height) + "\n";
    text += "every=" + std::to_string(info.every) + "\n";
    text += "images=" + std::to_string(info.images) + "\n";
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
        } else if (key == "images") {
            info.images = toUnsigned(value);
        }
    }
    return info;
}

bool writeRecordingInfo(const std::filesystem::path& folder, const RecordingInfo& info) {
    const std::filesystem::path file = folder / kRecordingFile;
    std::filesystem::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << formatRecordingInfo(info);
        if (!out) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    return !error;
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
        std::ifstream in(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        recordings.push_back({it->path(), parseRecordingInfo(text)});
    }
    std::sort(recordings.begin(), recordings.end(), [](const RecordingEntry& a, const RecordingEntry& b) {
        return a.info.started != b.info.started ? a.info.started > b.info.started : a.folder > b.folder;
    });
    return recordings;
}

} // namespace observex
