#include "core/ClipLines.h"

#include <cstdio>
#include <sstream>

namespace observex {

namespace {

void appendf(std::string& out, const char* format, auto... values) {
    char text[160];
    const int length = std::snprintf(text, sizeof(text), format, values...);
    if (length > 0) {
        out.append(text, static_cast<std::size_t>(length) < sizeof(text) ? static_cast<std::size_t>(length)
                                                                         : sizeof(text) - 1);
    }
}

// The rest of `line` after `word` and a space; false if it does not start so.
bool after(std::string_view line, std::string_view word, std::string_view& rest) {
    if (line.size() <= word.size() || line.substr(0, word.size()) != word || line[word.size()] != ' ') {
        return false;
    }
    rest = line.substr(word.size() + 1);
    return true;
}

} // namespace

void appendFrame(std::string& out, unsigned frame, unsigned long long image, double timeMs) {
    appendf(out, "frame %u %llu %.1f\n", frame, image, timeMs);
}

bool readFrame(std::string_view line, unsigned& frame, unsigned long long& image, double& timeMs) {
    std::string_view rest;
    if (!after(line, "frame", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    return static_cast<bool>(words >> frame >> image >> timeMs);
}

void appendShown(std::string& out, unsigned long long image, double doneMs, std::span<const Tracked> things) {
    appendf(out, "shown %llu %.1f", image, doneMs);
    for (const Tracked& thing : things) {
        appendf(out, " %u %.2f %u %.2f %u %.2f %.0f %.0f %.0f %.0f %.3f %.3f %d", static_cast<unsigned>(thing.id),
                static_cast<double>(thing.sure), static_cast<unsigned>(thing.kind),
                static_cast<double>(kindSureOf(thing)), static_cast<unsigned>(thing.secondKind),
                static_cast<double>(thing.second), static_cast<double>(thing.x), static_cast<double>(thing.y),
                static_cast<double>(thing.width), static_cast<double>(thing.height),
                static_cast<double>(thing.speedX), static_cast<double>(thing.speedY), thing.held ? 1 : 0);
    }
    out += '\n';
}

bool readShown(std::string_view line, unsigned long long& image, double& doneMs, std::vector<Tracked>& things) {
    std::string_view rest;
    if (!after(line, "shown", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    if (!(words >> image >> doneMs)) {
        return false;
    }
    things.clear();
    Tracked thing;
    unsigned id = 0;
    unsigned kind = 0;
    unsigned secondKind = 0;
    int held = 0;
    while (words >> id >> thing.sure >> kind >> thing.kindSure >> secondKind >> thing.second >> thing.x >> thing.y >>
           thing.width >> thing.height >> thing.speedX >> thing.speedY >> held) {
        thing.id = id;
        thing.kind = static_cast<std::uint8_t>(kind);
        thing.secondKind = static_cast<std::uint8_t>(secondKind);
        thing.held = held != 0;
        things.push_back(thing);
    }
    return true;
}

void appendNote(std::string& out, double timeMs, std::string_view text) {
    appendf(out, "note %.1f ", timeMs);
    for (const char c : text) {
        out += c == '\n' || c == '\r' ? ' ' : c;
    }
    out += '\n';
}

bool readNote(std::string_view line, double& timeMs, std::string& text) {
    std::string_view rest;
    if (!after(line, "note", rest)) {
        return false;
    }
    const std::size_t space = rest.find(' ');
    std::istringstream words{std::string(rest.substr(0, space))};
    if (!(words >> timeMs)) {
        return false;
    }
    text = space == std::string_view::npos ? std::string() : std::string(rest.substr(space + 1));
    return true;
}

} // namespace observex
