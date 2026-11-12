#include "core/LiveLines.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <sstream>
#include <string>

namespace observex {

namespace {

constexpr std::size_t kThingChars = 64;       // Most one thing takes on both lines together,
constexpr std::size_t kExactThingChars = 128; // written exactly (7 ExactNumbers of at most 15 characters).

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

ExactNumber::ExactNumber(float value) {
    *std::to_chars(text, text + sizeof(text) - 1, value).ptr = '\0';
}

ExactNumber::ExactNumber(double value) {
    *std::to_chars(text, text + sizeof(text) - 1, value).ptr = '\0';
}

std::size_t writeLook(std::span<char> out, unsigned long long image, double timeMs, std::span<const Detection> found,
                      bool exact) {
    const std::size_t thingChars = exact ? kExactThingChars : kThingChars;
    if (out.size() < 2 * thingChars) {
        return 0;
    }
    // Only as many things as surely fit, so neither line is cut and both hold the same ones.
    const std::size_t count = std::min(found.size(), (out.size() - 2 * thingChars) / thingChars);
    char* const text = out.data();
    const std::size_t size = out.size();
    const auto exactly = [](float value) { return ExactNumber(value); };
    std::size_t length = grown(0, exact ? std::snprintf(text, size, "look %llu %s", image, ExactNumber(timeMs).text)
                                        : std::snprintf(text, size, "look %llu %.1f", image, timeMs));
    for (std::size_t i = 0; i < count; ++i) {
        const Detection& thing = found[i];
        const int wrote =
            exact ? std::snprintf(text + length, size - length, " %s %s %s %s %s", exactly(thing.sure).text,
                                  exactly(thing.x).text, exactly(thing.y).text, exactly(thing.width).text,
                                  exactly(thing.height).text)
                  : std::snprintf(text + length, size - length, " %.2f %.0f %.0f %.0f %.0f",
                                  static_cast<double>(thing.sure), static_cast<double>(thing.x),
                                  static_cast<double>(thing.y), static_cast<double>(thing.width),
                                  static_cast<double>(thing.height));
        length = grown(length, wrote);
    }
    text[length++] = '\n';
    if (count > 0 && found[0].second >= 0.0f) { // A model with kinds.
        length = grown(length, std::snprintf(text + length, size - length, "kinds %llu", image));
        for (std::size_t i = 0; i < count; ++i) {
            const Detection& thing = found[i];
            const auto kind = static_cast<unsigned>(thing.kind);
            const auto secondKind = static_cast<unsigned>(thing.secondKind);
            const int wrote =
                exact ? std::snprintf(text + length, size - length, " %u %s %u %s", kind,
                                      exactly(kindSureOf(thing)).text, secondKind, exactly(thing.second).text)
                      : std::snprintf(text + length, size - length, " %u %.2f %u %.2f", kind,
                                      static_cast<double>(kindSureOf(thing)), secondKind,
                                      static_cast<double>(thing.second));
            length = grown(length, wrote);
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
