#include "core/Marks.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <system_error>

namespace observex {

namespace {

std::string_view trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// Splits off the first space-separated word of `text`.
std::string_view takeWord(std::string_view& text) {
    text = trim(text);
    const std::size_t end = std::min(text.find_first_of(" \t"), text.size());
    const std::string_view word = text.substr(0, end);
    text.remove_prefix(end);
    return word;
}

template <typename T> bool readNumber(std::string_view word, T& value) {
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
    return error == std::errc() && end == word.data() + word.size();
}

// "yes 1012 540 80 120" or "0.72 1012 540 80 120".
bool parseMark(std::string_view line, Mark& mark) {
    const std::string_view kind = takeWord(line);
    if (kind == "yes") {
        mark.answer = Answer::Yes;
    } else if (kind == "no") {
        mark.answer = Answer::No;
    } else if (!readNumber(kind, mark.guess) || mark.guess < 0.0f || mark.guess > 1.0f) {
        return false;
    }
    Box& box = mark.box;
    return readNumber(takeWord(line), box.x) && readNumber(takeWord(line), box.y) &&
           readNumber(takeWord(line), box.width) && readNumber(takeWord(line), box.height) && trim(line).empty() &&
           box.width > 0 && box.height > 0;
}

} // namespace

float overlap(const Box& a, const Box& b) {
    const int width = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
    const int height = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
    if (width <= 0 || height <= 0) {
        return 0.0f;
    }
    const double shared = static_cast<double>(width) * height;
    const double total = static_cast<double>(a.width) * a.height + static_cast<double>(b.width) * b.height - shared;
    return static_cast<float>(shared / total);
}

ImageMarks parseMarks(std::string_view text) {
    ImageMarks marks;
    std::vector<Mark>* image = nullptr; // The image the next marks belong to.
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = trim(text.substr(0, end));
        text.remove_prefix(std::min(end + 1, text.size()));
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::string_view rest = line;
        if (takeWord(rest) == "image") {
            const std::string_view name = trim(rest);
            image = name.empty() ? nullptr : &marks[std::string(name)];
            continue;
        }
        Mark mark;
        if (image != nullptr && parseMark(line, mark)) {
            image->push_back(mark);
        }
    }
    return marks;
}

std::string formatMarks(const ImageMarks& marks, std::string_view comment) {
    std::string text;
    while (!comment.empty()) {
        const std::size_t end = std::min(comment.find('\n'), comment.size());
        text += "# ";
        text += comment.substr(0, end);
        text += '\n';
        comment.remove_prefix(std::min(end + 1, comment.size()));
    }
    char line[96];
    for (const auto& [name, imageMarks] : marks) {
        text += "image ";
        text += name;
        text += '\n';
        for (const Mark& mark : imageMarks) {
            const Box& box = mark.box;
            if (mark.answer != Answer::None) {
                std::snprintf(line, sizeof(line), "%s %d %d %d %d\n", mark.answer == Answer::Yes ? "yes" : "no", box.x,
                              box.y, box.width, box.height);
            } else if (mark.guess >= 0.0f) {
                std::snprintf(line, sizeof(line), "%.3f %d %d %d %d\n", static_cast<double>(mark.guess), box.x, box.y,
                              box.width, box.height);
            } else {
                continue;
            }
            text += line;
        }
    }
    return text;
}

int carryAnswers(const std::vector<Mark>& previous, std::vector<Mark>& marks, float minOverlap) {
    int carried = 0;
    for (Mark& mark : marks) {
        if (mark.answer != Answer::None) {
            continue;
        }
        const Mark* best = nullptr;
        float bestOverlap = minOverlap;
        for (const Mark& before : previous) {
            const float amount = before.answer != Answer::None ? overlap(mark.box, before.box) : 0.0f;
            if (amount >= bestOverlap) {
                best = &before;
                bestOverlap = amount;
            }
        }
        if (best != nullptr) {
            mark.answer = best->answer;
            mark.carried = true;
            ++carried;
        }
    }
    return carried;
}

int presumeSure(std::vector<Mark>& marks, float minSure) {
    int presumed = 0;
    for (Mark& mark : marks) {
        if (mark.answer == Answer::None && mark.guess >= minSure) {
            mark.answer = Answer::Yes;
            mark.presumed = true;
            ++presumed;
        }
    }
    return presumed;
}

} // namespace observex
