#include "app/ReviewLog.h"

#include <windows.h>

#include <share.h>

#include <chrono>
#include <ctime>
#include <system_error>

namespace observex {

namespace {

constexpr char kHeader[] =
    "# observex review log: everything done in this recording's reviews, one event per line with its date and time,\n"
    "# its details on the indented lines below it. Image files are in this folder. Boxes are [x y width height] in\n"
    "# image pixels. Marks are numbered from 0 in the order the review holds them on the image: the finder's guesses\n"
    "# first, boxes the user drew after them (removing a drawn box renumbers the drawn ones after it).\n"
    "# A mark is its kind (monster, player, npc, own = the user's character, unknown = a character the user cannot\n"
    "# tell the kind of, dead = a corpse), no (not a character) or unanswered, then\n"
    "# where that came from: (pre-filled) by the review's rules, (copied from the image before) as the same thing\n"
    "# there was answered, (yours) answered by the user, (kind not checked) answered before kinds. After the colon:\n"
    "#   why a new guess starts so (asked, or the pre-fill rule; a no is never copied onto a guess the review is sure\n"
    "#   of, it is asked instead).\n"
    "# guess N%: how sure the finder was that it is a character. model / re-fit: how sure the model's kind\n"
    "# classifier and the review's re-fit to the answers so far were of each kind; the review shows the re-fit's.\n"
    "# open, close: a review opened and closed. image NAME: the review showed this image, how it started mark by "
    "mark,\n"
    "#   and the re-fit it used (what it learned from, how far it moved from the model's classifier), and the box\n"
    "#   nudge: each guess's box moved and resized the way the user boxed the guesses so far (on the images near this\n"
    "#   one most); a mark's \"nudged from [..]\" is the finder's own box.\n"
    "# select: the user picked a mark (click, Tab) or none (Esc). answer: the user answered a mark (key or button),\n"
    "#   from what to what. draw: the user drew a new box (a yes; its kind until the user picks one is the first).\n"
    "#   remove: the user removed a box they drew. Each with the other marks it overlaps (overlap = intersection over\n"
    "#   union; how much of each box lies inside the other).\n"
    "# next blocked: Next with marks still unanswered. done NAME: the user moved on with Next, the image reviewed: "
    "what\n"
    "#   it holds, and every change from how it was shown (changed, confirmed = answered again the same, drew,\n"
    "#   removed, kept as shown). back: Left / A to the image before, with the changes so far. undo: Ctrl+Z, and what\n"
    "#   it put back.\n"
    "# guesses ready, finder failed: the finder's guesses for this recording, made while the review was open.\n";

std::string now() {
    SYSTEMTIME time;
    GetLocalTime(&time);
    char text[32];
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u", time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return text;
}

} // namespace

ReviewLog::ReviewLog(const std::filesystem::path& file) {
    std::error_code error;
    const bool empty = !std::filesystem::exists(file, error) || std::filesystem::file_size(file, error) == 0;
    file_ = _wfsopen(file.c_str(), L"ab", _SH_DENYWR); // Binary: plain \n line ends. Others may read it meanwhile.
    if (file_ != nullptr && empty) {
        std::fputs(kHeader, file_);
        std::fflush(file_);
    }
}

ReviewLog::~ReviewLog() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

void ReviewLog::write(std::string_view text) {
    if (file_ == nullptr) {
        return;
    }
    std::string lines = now() + " ";
    for (const char c : text) {
        lines += c;
        if (c == '\n') {
            lines += "    ";
        }
    }
    lines += '\n';
    std::fwrite(lines.data(), 1, lines.size(), file_);
    std::fflush(file_);
}

std::string ReviewLog::fileTime(const std::filesystem::path& file) {
    std::error_code error;
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(file, error);
    if (error) {
        return "unknown";
    }
    const std::time_t seconds =
        std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(written));
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) {
        return "unknown";
    }
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

} // namespace observex
