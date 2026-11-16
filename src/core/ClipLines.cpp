#include "core/ClipLines.h"

#include "core/LiveLines.h"

#include <algorithm>
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

void appendHeights(std::string& out, unsigned long long image, double timeMs, unsigned long long terrain) {
    appendf(out, "heights %llu %s %llu\n", image, ExactNumber(timeMs).text, terrain);
}

bool readHeights(std::string_view line, unsigned long long& image, double& timeMs, unsigned long long& terrain) {
    std::string_view rest;
    if (!after(line, "heights", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    return static_cast<bool>(words >> image >> timeMs >> terrain);
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

void appendDrops(std::string& out, unsigned long long image, double doneMs, bool kill, const LootSquare& square,
                 std::span<const Detection> drops) {
    appendf(out, "drops %llu %s %d %d %d %d", image, ExactNumber(doneMs).text, kill ? 1 : 0, square.x, square.y,
            square.side);
    for (const Detection& drop : drops) {
        appendf(out, " %s %s %s %s %s", ExactNumber(drop.sure).text, ExactNumber(drop.x).text,
                ExactNumber(drop.y).text, ExactNumber(drop.width).text, ExactNumber(drop.height).text);
    }
    out += '\n';
}

bool readDrops(std::string_view line, unsigned long long& image, double& doneMs, bool& kill, LootSquare& square,
               std::vector<Detection>& drops) {
    std::string_view rest;
    if (!after(line, "drops", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    int killed = 0;
    LootSquare read;
    if (!(words >> image >> doneMs >> killed >> read.x >> read.y >> read.side)) {
        return false;
    }
    kill = killed != 0;
    square = read;
    drops.clear();
    Detection drop;
    while (words >> drop.sure >> drop.x >> drop.y >> drop.width >> drop.height) {
        drops.push_back(drop);
    }
    return true;
}

void appendMouse(std::string& out, const ClipMouse& mouse) {
    appendf(out, "mouse %s %d %d %u %u %d %d %d\n", ExactNumber(mouse.timeMs).text, mouse.dx, mouse.dy, mouse.held,
            mouse.buttons, mouse.wheel, mouse.cursorX, mouse.cursorY);
}

bool readMouse(std::string_view line, ClipMouse& mouse) {
    std::string_view rest;
    if (!after(line, "mouse", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    ClipMouse read;
    if (!(words >> read.timeMs >> read.dx >> read.dy >> read.held >> read.buttons >> read.wheel >> read.cursorX >>
          read.cursorY)) {
        return false;
    }
    mouse = read;
    return true;
}

void appendCamera(std::string& out, unsigned long long image, double timeMs, const CameraShown& camera) {
    appendf(out, "camera %llu %s %d %d %s %d %s %d %s %s\n", image, ExactNumber(timeMs).text, camera.known ? 1 : 0,
            camera.yawKnown ? 1 : 0, ExactNumber(camera.yaw).text, camera.pitchKnown ? 1 : 0,
            ExactNumber(camera.pitch).text, camera.pitchBy, ExactNumber(camera.coneOff).text,
            ExactNumber(camera.turnsOff).text);
}

bool readCamera(std::string_view line, unsigned long long& image, double& timeMs, CameraShown& camera) {
    std::string_view rest;
    if (!after(line, "camera", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    int known = 0;
    int yawKnown = 0;
    int pitchKnown = 0;
    CameraShown read;
    if (!(words >> image >> timeMs >> known >> yawKnown >> read.yaw >> pitchKnown >> read.pitch >> read.pitchBy >>
          read.coneOff >> read.turnsOff)) {
        return false;
    }
    read.known = known != 0;
    read.yawKnown = yawKnown != 0;
    read.pitchKnown = pitchKnown != 0;
    camera = read;
    return true;
}

void appendBrain(std::string& out, unsigned long long image, double timeMs, const statetree::BrainShown& brain) {
    appendf(out, "brain %llu %s %d %d", image, ExactNumber(timeMs).text, brain.acting ? 1 : 0, brain.task);
    if (!brain.state.empty()) {
        out += ' ';
        for (const char c : brain.state) {
            out += c == '\n' || c == '\r' ? ' ' : c;
        }
    }
    out += '\n';
}

bool readBrain(std::string_view line, unsigned long long& image, double& timeMs, statetree::BrainShown& brain) {
    std::string_view rest;
    if (!after(line, "brain", rest)) {
        return false;
    }
    // Four words, then the state's names (the rest of the line, spaces and all).
    std::size_t at = 0;
    for (int word = 0; word < 4; ++word) {
        at = rest.find(' ', at);
        if (at == std::string_view::npos) {
            if (word < 3) {
                return false;
            }
            at = rest.size();
            break;
        }
        ++at;
    }
    std::istringstream words{std::string(rest.substr(0, at))};
    int acting = 0;
    statetree::BrainShown read;
    if (!(words >> image >> timeMs >> acting >> read.task)) {
        return false;
    }
    read.acting = acting != 0;
    read.state = at < rest.size() ? std::string(rest.substr(at)) : std::string();
    brain = std::move(read);
    return true;
}

// A cost's name with its spaces as _, then its numbers, each after a space.
static void appendCostOf(std::string& out, const Cost& cost) {
    out += ' ';
    for (const char c : std::string_view(cost.name.data())) {
        out += c == ' ' || c == '\n' || c == '\r' ? '_' : c;
    }
    appendf(out, " %.3f %.3f %.1f", static_cast<double>(cost.eachMs), static_cast<double>(cost.slowestMs),
            static_cast<double>(cost.perSecond));
}

static bool readCostOf(std::istringstream& words, Cost& cost) {
    std::string name;
    Cost read;
    if (!(words >> name >> read.eachMs >> read.slowestMs >> read.perSecond)) {
        return false;
    }
    std::replace(name.begin(), name.end(), '_', ' ');
    nameCost(read, name);
    cost = read;
    return true;
}

void appendCost(std::string& out, double timeMs, const CostPart& part) {
    appendf(out, "cost %.1f", timeMs);
    appendCostOf(out, part.run);
    for (int i = 0; i < part.stepCount; ++i) {
        if (const Cost& step = part.steps[static_cast<std::size_t>(i)]; step.perSecond > 0.0f) {
            appendCostOf(out, step);
        }
    }
    out += '\n';
}

bool readCost(std::string_view line, double& timeMs, CostPart& part) {
    std::string_view rest;
    if (!after(line, "cost", rest)) {
        return false;
    }
    std::istringstream words{std::string(rest)};
    CostPart read;
    if (!(words >> timeMs) || !readCostOf(words, read.run)) {
        return false;
    }
    while (read.stepCount < CostPart::kMaxSteps &&
           readCostOf(words, read.steps[static_cast<std::size_t>(read.stepCount)])) {
        ++read.stepCount;
    }
    part = read;
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

void appendLog(std::string& out, double timeMs, const LogLine& line) {
    appendf(out, "log %.1f ", timeMs);
    appendLogLine(out, line);
}

void appendTrace(std::string& out, double timeMs, const LogLine& line) {
    appendf(out, "trace %.1f ", timeMs);
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
