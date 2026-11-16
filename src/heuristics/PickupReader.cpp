#include "heuristics/PickupReader.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <vector>

namespace observex {

namespace {

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// `text` without spaces and full stops at its ends.
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '.')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '.' || text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

void copyItem(std::array<char, 48>& out, std::string_view text) {
    const std::size_t n = std::min(text.size(), out.size() - 1);
    std::copy_n(text.begin(), n, out.begin());
    out[n] = 0;
}

} // namespace

bool PickupReader::parse(std::string_view text, std::string& item, std::int64_t& amount) {
    // The word, as OCR may read it with a letter wrong at either end, or a space inside ("ear ned"): looked for with
    // the spaces left out, then the name begins after the next space.
    std::string squeezed;
    std::vector<std::size_t> from; // Where each of squeezed's letters is in text.
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != ' ') {
            squeezed += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
            from.push_back(i);
        }
    }
    static constexpr std::string_view kWords[] = {"obtain", "obta", "tained", "earned", "earne", "arned"};
    std::size_t end = std::string::npos;
    for (const std::string_view word : kWords) {
        if (const std::size_t at = squeezed.find(word); at != std::string::npos) {
            end = text.find(' ', from[at + word.size() - 1]);
            break;
        }
    }
    if (end == std::string::npos) {
        return false;
    }
    std::string_view rest = trimmed(text.substr(end));
    // A number first (thousands after ',' or '.'), then a space: how many. "1st Onyx Talisman" begins with no number.
    std::int64_t number = 0;
    std::size_t i = 0;
    int digits = 0;
    while (i < rest.size() && digits <= 12) {
        if (isDigit(rest[i])) {
            number = number * 10 + (rest[i] - '0');
            ++digits;
            ++i;
        } else if (digits > 0 && (rest[i] == ',' || rest[i] == '.') && i + 1 < rest.size() && isDigit(rest[i + 1])) {
            ++i;
        } else {
            break;
        }
    }
    amount = 1;
    if (digits > 0 && i < rest.size() && rest[i] == ' ') {
        amount = number;
        rest = trimmed(rest.substr(i));
    }
    if (std::none_of(rest.begin(), rest.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; })) {
        return false;
    }
    item = lowered(rest) == "adena" ? std::string("Adena") : std::string(rest);
    return amount > 0;
}

void PickupReader::read(MessageLines& lines, TextOcr& ocr, PickupRead& out, std::string& why) {
    const auto say = [&why](const std::string& line) {
        if (!why.empty()) {
            why += " | ";
        }
        why += line;
    };
    if (out.run == 0) {
        // From a clock, so a new reader (the stats DLL reloaded) never repeats an old reader's run.
        out.run = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;
    }
    bool aboveSaid = false; // The line above was a new pickup,
    int aboveBottom = 0;    // ending on this row.
    for (std::size_t i = 0; i < lines.lines().size(); ++i) {
        const MessageLines::Line line = lines.lines()[i];
        if (line.white || !line.fresh) {
            aboveSaid = false;
            continue;
        }
        const std::string said = lines.read(line.top, line.bottom, line.right, true, 3, true, ocr, &error_);
        std::string item;
        std::int64_t amount = 1;
        if (!parse(said, item, amount)) {
            const std::string_view rest = trimmed(said);
            if (aboveSaid && line.top - aboveBottom < 8 && !rest.empty() && out.count != 0) {
                // The rest of a long name.
                Pickup& last = out.items[(out.count - 1) % PickupRead::kMax];
                const std::string name = std::string(last.item.data()) + " " + std::string(rest);
                copyItem(last.item, name);
                say("Pickup: the name goes on: '" + std::string(rest) + "'");
            } else if (!rest.empty()) { // Empty: no letters (bright grass behind the see-through window looks yellow).
                say("Pickup: a new yellow line, not a pickup: '" + std::string(rest) + "'");
            }
            aboveSaid = false;
            continue;
        }
        // Read again as blocks; if the two differ, a third decides (else the first stands).
        std::string other;
        std::int64_t otherAmount = 1;
        const std::string again = lines.read(line.top, line.bottom, line.right, true, 4, false, ocr, &error_);
        if (!parse(again, other, otherAmount) || other != item || otherAmount != amount) {
            std::string third;
            std::int64_t thirdAmount = 1;
            const std::string last = lines.read(line.top, line.bottom, line.right, true, 4, true, ocr, &error_);
            if (parse(last, third, thirdAmount) && !(third == item && thirdAmount == amount) &&
                third == other && thirdAmount == otherAmount) {
                item = third;
                amount = thirdAmount;
            }
            say("Pickup: readings differ: '" + std::string(trimmed(said)) + "', '" + std::string(trimmed(again)) +
                "', '" + std::string(trimmed(last)) + "'");
        }
        ++out.count;
        Pickup& pickup = out.items[(out.count - 1) % PickupRead::kMax];
        copyItem(pickup.item, item);
        pickup.amount = amount;
        if (item == "Adena") {
            out.adena += amount;
        }
        char note[128];
        std::snprintf(note, sizeof(note), "Pickup: %lld %s (read '%s')", static_cast<long long>(amount), item.c_str(),
                      std::string(trimmed(said)).c_str());
        say(note);
        aboveSaid = true;
        aboveBottom = line.bottom;
    }
}

} // namespace observex
