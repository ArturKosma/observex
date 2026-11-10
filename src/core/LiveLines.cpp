#include "core/LiveLines.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>

namespace observex {

namespace {

constexpr std::size_t kThingChars = 64; // Most one thing takes on both lines together.

// Where the line goes on after std::snprintf wrote `wrote` characters at `at`.
std::size_t grown(std::size_t at, int wrote) {
    return wrote > 0 ? at + static_cast<std::size_t>(wrote) : at;
}

// Puts the words of `line` after `word` and a space into `words`; false if it does not start so.
bool after(std::string_view line, std::string_view word, std::istringstream& words) {
    if (line.size() <= word.size() || line.substr(0, word.size()) != word || line[word.size()] != ' ') {
        return false;
    }
    words.str(std::string(line.substr(word.size() + 1)));
    return true;
}

} // namespace

std::size_t writeLook(std::span<char> out, unsigned long long image, double timeMs, std::span<const Detection> found) {
    if (out.size() < 2 * kThingChars) {
        return 0;
    }
    // Only as many things as surely fit, so neither line is cut and both hold the same ones.
    const std::size_t count = std::min(found.size(), (out.size() - 2 * kThingChars) / kThingChars);
    char* const text = out.data();
    const std::size_t size = out.size();
    std::size_t length = grown(0, std::snprintf(text, size, "look %llu %.1f", image, timeMs));
    for (std::size_t i = 0; i < count; ++i) {
        const Detection& thing = found[i];
        length = grown(length, std::snprintf(text + length, size - length, " %.2f %.0f %.0f %.0f %.0f",
                                             static_cast<double>(thing.sure), static_cast<double>(thing.x),
                                             static_cast<double>(thing.y), static_cast<double>(thing.width),
                                             static_cast<double>(thing.height)));
    }
    text[length++] = '\n';
    if (count > 0 && found[0].second >= 0.0f) { // A model with kinds.
        length = grown(length, std::snprintf(text + length, size - length, "kinds %llu", image));
        for (std::size_t i = 0; i < count; ++i) {
            const Detection& thing = found[i];
            length = grown(length, std::snprintf(text + length, size - length, " %u %.2f %u %.2f",
                                                 static_cast<unsigned>(thing.kind),
                                                 static_cast<double>(kindSureOf(thing)),
                                                 static_cast<unsigned>(thing.secondKind),
                                                 static_cast<double>(thing.second)));
        }
        text[length++] = '\n';
    }
    return length;
}

bool readLook(std::string_view line, unsigned long long& image, double& timeMs, std::vector<Detection>& found) {
    std::istringstream words;
    if (!after(line, "look", words) || !(words >> image >> timeMs)) {
        return false;
    }
    found.clear();
    Detection thing;
    while (words >> thing.sure >> thing.x >> thing.y >> thing.width >> thing.height) {
        found.push_back(thing);
    }
    return true;
}

bool readKinds(std::string_view line, unsigned long long image, std::vector<Detection>& found) {
    std::istringstream words;
    unsigned long long of = 0;
    if (!after(line, "kinds", words) || !(words >> of) || of != image) {
        return false;
    }
    for (Detection& thing : found) {
        unsigned kind = 0;
        unsigned secondKind = 0;
        float sure = 0.0f;
        float second = 0.0f;
        if (!(words >> kind >> sure >> secondKind >> second)) {
            break;
        }
        thing.kind = static_cast<std::uint8_t>(kind);
        thing.kindSure = sure;
        thing.secondKind = static_cast<std::uint8_t>(secondKind);
        thing.second = second;
    }
    return true;
}

} // namespace observex
