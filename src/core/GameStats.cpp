#include "core/GameStats.h"

#include "core/LiveLines.h"
#include "core/Marks.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace observex {

namespace {

template <std::size_t N> void copyName(std::array<char, N>& to, std::string_view from) {
    to.fill('\0');
    std::memcpy(to.data(), from.data(), std::min(from.size(), N - 1));
}

// A pressed skill's TEXT in a stats line.
constexpr char kPressed[] = "pressed";

// A name or text as one word of a line: its spaces as "_" (skill names have spaces; none has a "_",
// heuristics/Miniatures' skillFileName), and back.
void appendWord(std::string& text, const char* word) {
    for (const char* c = word; *c != '\0'; ++c) {
        text += *c == ' ' ? '_' : *c;
    }
}

template <std::size_t N> void copyWord(std::array<char, N>& to, std::string_view from) {
    copyName(to, from);
    std::replace(to.begin(), to.end(), '_', ' ');
}

std::vector<std::string_view> splitWords(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) {
            ++at;
        }
        std::size_t end = at;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r') {
            ++end;
        }
        if (end > at) {
            words.push_back(line.substr(at, end - at));
        }
        at = end;
    }
    return words;
}

} // namespace

const GameGauge* GameStats::find(std::string_view piece, std::string_view name) const {
    for (int i = 0; i < count; ++i) {
        const GameGauge& gauge = gauges[static_cast<std::size_t>(i)];
        if (piece == gauge.piece.data() && name == gauge.name.data()) {
            return &gauge;
        }
    }
    return nullptr;
}

void appendStatsLine(std::string& text, std::uint64_t image, double ms, const GameStats& stats) {
    char head[96];
    std::snprintf(head, sizeof(head), "stats %llu %s", static_cast<unsigned long long>(image), ExactNumber(ms).text);
    text += head;
    for (int i = 0; i < stats.count; ++i) {
        const GameGauge& gauge = stats.gauges[static_cast<std::size_t>(i)];
        char full[32];
        std::snprintf(full, sizeof(full), "%.4f", static_cast<double>(gauge.full));
        text += ' ';
        text += gauge.piece.data();
        text += ' ';
        appendWord(text, gauge.name.data());
        text += ' ';
        text += gauge.full >= 0.0f ? full : "-";
        text += ' ';
        appendWord(text, gauge.pressed ? kPressed : gauge.text[0] != '\0' ? gauge.text.data() : "-");
    }
    text += '\n';
    if (stats.why[0] != '\0') {
        std::snprintf(head, sizeof(head), "why %llu ", static_cast<unsigned long long>(image));
        text += head;
        for (const char* c = stats.why.data(); c < stats.why.data() + stats.why.size() && *c != '\0'; ++c) {
            text += *c == '\n' ? ' ' : *c;
        }
        text += '\n';
    }
}

bool readStatsLine(std::string_view line, std::uint64_t* image, double* ms, GameStats* stats) {
    const std::vector<std::string_view> words = splitWords(line.substr(0, line.find('\n')));
    if (words.size() < 3 || words[0] != "stats" || (words.size() - 3) % 4 != 0 ||
        (words.size() - 3) / 4 > static_cast<std::size_t>(GameStats::kMax)) {
        return false;
    }
    unsigned long long number = 0;
    if (std::from_chars(words[1].data(), words[1].data() + words[1].size(), number).ec != std::errc() ||
        std::from_chars(words[2].data(), words[2].data() + words[2].size(), *ms).ec != std::errc()) {
        return false;
    }
    *image = number;
    stats->count = 0;
    for (std::size_t at = 3; at < words.size(); at += 4) {
        GameGauge& gauge = stats->gauges[static_cast<std::size_t>(stats->count++)];
        gauge = {};
        copyName(gauge.piece, words[at]);
        copyWord(gauge.name, words[at + 1]);
        if (words[at + 2] != "-") {
            if (std::from_chars(words[at + 2].data(), words[at + 2].data() + words[at + 2].size(), gauge.full).ec !=
                std::errc()) {
                return false;
            }
            gauge.seen = true;
        }
        if (words[at + 3] == kPressed && std::string_view(gauge.piece.data()) == kSkillsPiece) {
            gauge.pressed = true;
            gauge.seen = true;
        } else if (words[at + 3] != "-") {
            copyWord(gauge.text, words[at + 3]);
            gauge.seen = true;
        }
    }
    stats->why = {};
    stats->image = number;
    stats->hasTarget = false;
    stats->targetName = {};
    for (int i = 0; i < stats->count; ++i) {
        const GameGauge& gauge = stats->gauges[static_cast<std::size_t>(i)];
        if (std::strcmp(gauge.piece.data(), kTargetPiece) == 0) {
            stats->hasTarget = stats->hasTarget || gauge.seen;
            if (std::strcmp(gauge.name.data(), kTargetNameGauge) == 0) {
                stats->targetName = gauge.text;
            }
        }
    }
    stats->target = stats->hasTarget ? stats->targetName : std::array<char, 32>{};
    stats->targetChanged = false;
    return true;
}

std::vector<ClipStats> readClipStats(std::string_view clipText) {
    std::vector<ClipStats> lines;
    std::size_t at = 0;
    while (at < clipText.size()) {
        std::size_t end = clipText.find('\n', at);
        if (end == std::string_view::npos) {
            end = clipText.size();
        }
        const std::string_view line = clipText.substr(at, end - at);
        at = end + 1;
        if (line.size() > 6 && line.compare(0, 6, "stats ") == 0) {
            ClipStats read;
            std::uint64_t image = 0;
            if (readStatsLine(line, &image, &read.ms, &read.stats)) {
                lines.push_back(read);
            }
        } else if (line.size() > 4 && line.compare(0, 4, "why ") == 0 && !lines.empty()) {
            // What the stats line before it (the same image) could not see.
            const std::size_t space = line.find(' ', 4);
            unsigned long long image = 0;
            if (space != std::string_view::npos &&
                std::from_chars(line.data() + 4, line.data() + space, image).ec == std::errc() &&
                image == lines.back().stats.image) {
                copyName(lines.back().stats.why, line.substr(space + 1));
            }
        }
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const ClipStats& a, const ClipStats& b) { return a.stats.image < b.stats.image; });
    return lines;
}

void addNearby(GameStats& stats, bool seen, std::span<const Tracked> things, std::span<const std::string> kinds) {
    if (stats.find(kNearbyPiece, kCharactersGauge) != nullptr || stats.count + 2 > GameStats::kMax) {
        return;
    }
    const auto kindOf = [&](std::string_view id) {
        const auto at = std::find(kinds.begin(), kinds.end(), id);
        return at != kinds.end() ? static_cast<int>(at - kinds.begin()) : -1;
    };
    const int own = kindOf("own");
    const int monster = kindOf("monster");
    const int unknown = kindOf(kUnknownKind);
    int characters = 0;
    int monsters = 0;
    for (const Tracked& thing : things) {
        const bool told = !cantTellKind(thing, unknown);
        if (told && static_cast<int>(thing.kind) == own) {
            continue;
        }
        ++characters;
        monsters += told && static_cast<int>(thing.kind) == monster ? 1 : 0;
    }
    const auto add = [&](const char* name, int count, bool known) {
        GameGauge& gauge = stats.gauges[static_cast<std::size_t>(stats.count++)];
        gauge = {};
        copyName(gauge.piece, kNearbyPiece);
        copyName(gauge.name, name);
        gauge.seen = known;
        if (known) {
            std::snprintf(gauge.text.data(), gauge.text.size(), "%d", count);
        }
    };
    add(kCharactersGauge, characters, seen);
    add(kMonstersGauge, monsters, seen && monster >= 0);
}

const ClipStats* statsAt(const std::vector<ClipStats>& lines, std::uint64_t image) {
    const auto after = std::upper_bound(lines.begin(), lines.end(), image, [](std::uint64_t at, const ClipStats& line) {
        return at < line.stats.image;
    });
    return after == lines.begin() ? nullptr : &*(after - 1);
}

} // namespace observex
