#include "core/ClipLines.h"

#include "core/LiveLines.h"

#include <cstdio>
#include <sstream>

namespace observex {

namespace {

void appendf(std::string& out, const char* format, auto... values) {
    char text[256];
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
    appendf(out, "frame %u %llu %s\n", frame, image, ExactNumber(timeMs).text);
}

bool readFrame(std::string_view line, unsigned& frame, unsigned long long& image, double& timeMs) {
    std::string_view rest;
    if (!after(line, "frame", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    return static_cast<bool>(words >> frame >> image >> timeMs);
}

// The things of a shown or snapped line, each after a space, then the line's end.
static void appendThings(std::string& out, std::span<const Tracked> things) {
    for (const Tracked& thing : things) {
        appendf(out, " %u %s %u %s %u %s %s %s %s %s %s %s %d", static_cast<unsigned>(thing.id),
                ExactNumber(thing.sure).text, static_cast<unsigned>(thing.kind), ExactNumber(kindSureOf(thing)).text,
                static_cast<unsigned>(thing.secondKind), ExactNumber(thing.second).text, ExactNumber(thing.x).text,
                ExactNumber(thing.y).text, ExactNumber(thing.width).text, ExactNumber(thing.height).text,
                ExactNumber(thing.speedX).text, ExactNumber(thing.speedY).text, thing.held ? 1 : 0);
    }
    out += '\n';
}

// The things after a shown or snapped line's numbers, into `things` (replaced).
static void readThings(std::istringstream& words, std::vector<Tracked>& things) {
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
}

void appendShown(std::string& out, unsigned long long image, double doneMs, std::span<const Tracked> things) {
    appendf(out, "shown %llu %s", image, ExactNumber(doneMs).text);
    appendThings(out, things);
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
    readThings(words, things);
    return true;
}

void appendSnapped(std::string& out, unsigned long long image, double timeMs, double doneMs, double leadMs,
                   std::span<const Tracked> things) {
    appendf(out, "snapped %llu %s", image, ExactNumber(timeMs).text);
    appendf(out, " %s %s", ExactNumber(doneMs).text, ExactNumber(leadMs).text);
    appendThings(out, things);
}

bool readSnapped(std::string_view line, unsigned long long& image, double& timeMs, double& doneMs, double& leadMs,
                 std::vector<Tracked>& things) {
    std::string_view rest;
    if (!after(line, "snapped", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    if (!(words >> image >> timeMs >> doneMs >> leadMs)) {
        return false;
    }
    readThings(words, things);
    return true;
}

void appendFinds(std::string& out, unsigned long long image, unsigned long long lookImage,
                 std::span<const SnapFind> finds) {
    appendf(out, "finds %llu %llu %u", image, lookImage, finds.empty() ? 0u : static_cast<unsigned>(finds[0].finder));
    for (const SnapFind& find : finds) {
        appendf(out, " %u %llu %s %s %s %s", static_cast<unsigned>(find.id),
                static_cast<unsigned long long>(find.learnedImage), ExactNumber(find.learnedX).text,
                ExactNumber(find.learnedY).text, ExactNumber(find.learnedWidth).text,
                ExactNumber(find.learnedHeight).text);
        appendf(out, " %s %s %s %d %s %s %s", ExactNumber(find.guessX).text, ExactNumber(find.guessY).text,
                ExactNumber(find.reach).text, find.found ? 1 : 0, ExactNumber(find.x).text, ExactNumber(find.y).text,
                ExactNumber(find.match).text);
    }
    out += '\n';
}

bool readFinds(std::string_view line, unsigned long long& image, unsigned long long& lookImage,
               std::vector<SnapFind>& finds) {
    std::string_view rest;
    if (!after(line, "finds", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    unsigned finder = 0;
    if (!(words >> image >> lookImage >> finder)) {
        return false;
    }
    finds.clear();
    SnapFind find;
    find.finder = finder;
    unsigned id = 0;
    unsigned long long learned = 0;
    int found = 0;
    while (words >> id >> learned >> find.learnedX >> find.learnedY >> find.learnedWidth >> find.learnedHeight >>
           find.guessX >> find.guessY >> find.reach >> found >> find.x >> find.y >> find.match) {
        find.id = id;
        find.learnedImage = learned;
        find.found = found != 0;
        finds.push_back(find);
    }
    return true;
}

void appendGround(std::string& out, unsigned long long image, double timeMs, unsigned long long followed,
                  const GroundCamera& camera) {
    appendf(out, "ground %llu %s %llu %d %d", image, ExactNumber(timeMs).text, followed, camera.known ? 1 : 0,
            camera.pitchMeasured ? 1 : 0);
    for (const float value : {camera.pitch, camera.distance, camera.focal, camera.middleX, camera.middleY, camera.yaw,
                              camera.feetX, camera.feetY}) {
        appendf(out, " %s", ExactNumber(value).text);
    }
    out += '\n';
}

bool readGround(std::string_view line, unsigned long long& image, double& timeMs, unsigned long long& followed,
                GroundCamera& camera) {
    std::string_view rest;
    if (!after(line, "ground", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    int known = 0;
    int measured = 0;
    GroundCamera read;
    if (!(words >> image >> timeMs >> followed >> known >> measured >> read.pitch >> read.distance >> read.focal >>
          read.middleX >> read.middleY >> read.yaw >> read.feetX >> read.feetY)) {
        return false;
    }
    read.known = known != 0;
    read.pitchMeasured = measured != 0;
    read.timeMs = timeMs;
    camera = read;
    return true;
}

void appendDepth(std::string& out, unsigned long long image, double timeMs, unsigned long long map) {
    appendf(out, "depth %llu %s %llu\n", image, ExactNumber(timeMs).text, map);
}

bool readDepth(std::string_view line, unsigned long long& image, double& timeMs, unsigned long long& map) {
    std::string_view rest;
    if (!after(line, "depth", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    return static_cast<bool>(words >> image >> timeMs >> map);
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

void appendLog(std::string& out, double timeMs, const LogLine& line) {
    appendf(out, "log %.1f ", timeMs);
    appendLogLine(out, line);
}

bool readLog(std::string_view line, double& timeMs, LogLine& out) {
    std::string_view rest;
    if (!after(line, "log", rest)) {
        return false;
    }
    const std::size_t space = rest.find(' ');
    if (space == std::string_view::npos) {
        return false;
    }
    std::istringstream words{std::string(rest.substr(0, space))};
    return static_cast<bool>(words >> timeMs) && readLogLine(rest.substr(space + 1), out);
}

} // namespace observex
