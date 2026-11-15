#include "core/SkillRefresh.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace observex {

namespace {

constexpr double kAskAgain = 1.0;     // A read not answered this long is asked for again.
constexpr double kSkillSeconds = 6.0; // On one skill this long in all (the cursor never came onto it): given up on.
constexpr std::size_t kTextInLog = 160;

bool isLetterOrDigit(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

// A word without the marks round it ("MP:" is "MP", "(10)" is "10").
std::string_view bare(std::string_view word) {
    while (!word.empty() && !isLetterOrDigit(word.front())) {
        word.remove_prefix(1);
    }
    while (!word.empty() && !isLetterOrDigit(word.back())) {
        word.remove_suffix(1);
    }
    return word;
}

bool sameWord(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

bool startsWith(std::string_view word, std::string_view start) {
    return word.size() >= start.size() && sameWord(word.substr(0, start.size()), start);
}

bool isMp(std::string_view word) {
    word = bare(word);
    return sameWord(word, "mp") || sameWord(word, "mana");
}

// A word saying what the number by it is for: "Consumption", "Consumes", "Cost", "Uses", "Required".
bool isCostWord(std::string_view word) {
    word = bare(word);
    return startsWith(word, "consum") || startsWith(word, "cost") || startsWith(word, "use") ||
           startsWith(word, "requir") || startsWith(word, "need");
}

// The number a word is (OCR's O for 0, l or I for 1 taken as the digits); -1 if it is not one, or has a '/' or '%'
// in it (a bar's "90/108", a share).
int numberOf(std::string_view word) {
    if (word.find('/') != std::string_view::npos || word.find('%') != std::string_view::npos) {
        return -1;
    }
    word = bare(word);
    if (word.empty() || word.size() > 5) {
        return -1;
    }
    int value = 0;
    bool digit = false;
    for (const char c : word) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            digit = true;
        } else if (c == 'O' || c == 'o') {
            value *= 10;
        } else if (c == 'l' || c == 'I') {
            value = value * 10 + 1;
        } else {
            return -1;
        }
    }
    return digit ? value : -1;
}

std::vector<std::string_view> wordsOf(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0) {
            ++at;
        }
        const std::size_t start = at;
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) == 0) {
            ++at;
        }
        if (at > start) {
            words.push_back(text.substr(start, at - start));
        }
    }
    return words;
}

bool hasName(std::string_view text, const std::vector<std::string_view>& words, std::string_view name) {
    const std::vector<std::string_view> nameWords = wordsOf(name);
    if (nameWords.empty()) {
        return false;
    }
    // In the text as it is ("Sit/Stand" has "Sit")...
    std::string lowText(text.size(), ' ');
    std::transform(text.begin(), text.end(), lowText.begin(), lower);
    std::string lowName(name.size(), ' ');
    std::transform(name.begin(), name.end(), lowName.begin(), lower);
    if (lowText.find(lowName) != std::string::npos) {
        return true;
    }
    // ...or as many words in a row, a letter or two read wrong.
    for (std::size_t i = 0; i + nameWords.size() <= words.size(); ++i) {
        std::string joined;
        for (std::size_t j = 0; j < nameWords.size(); ++j) {
            joined += (j > 0 ? " " : "") + std::string(bare(words[i + j]));
        }
        if (sameName(joined, name)) {
            return true;
        }
    }
    return false;
}

const char* plural(std::size_t n) { return n == 1 ? "" : "s"; }

} // namespace

MpCostRead readMpCost(std::string_view text, std::string_view skillName) {
    MpCostRead read;
    const std::vector<std::string_view> words = wordsOf(text);
    read.name = hasName(text, words, skillName);
    // Best first: after "MP", past cost words and marks only ("MP 10", "MP: 10", "MP Consumption 10"); then "Consumes
    // 10 MP"; then "10 MP".
    for (std::size_t i = 0; i < words.size() && read.mp < 0; ++i) {
        if (!isMp(words[i])) {
            continue;
        }
        for (std::size_t j = i + 1; j < words.size() && j <= i + 3; ++j) {
            if (const int n = numberOf(words[j]); n >= 0) {
                read.mp = n;
                break;
            }
            if (!bare(words[j]).empty() && !isCostWord(words[j])) {
                break;
            }
        }
    }
    for (std::size_t i = 2; i < words.size() && read.mp < 0; ++i) {
        if (isMp(words[i]) && isCostWord(words[i - 2]) && numberOf(words[i - 1]) >= 0) {
            read.mp = numberOf(words[i - 1]);
        }
    }
    for (std::size_t i = 1; i < words.size() && read.mp < 0; ++i) {
        if (isMp(words[i]) && numberOf(words[i - 1]) >= 0) {
            read.mp = numberOf(words[i - 1]);
        }
    }
    return read;
}

StatsArea tooltipArea(const StatsArea& slot, int width, int height) {
    const int side = std::max(24, slot.x1 - slot.x0);
    const int cx = (slot.x0 + slot.x1) / 2;
    const int cy = (slot.y0 + slot.y1) / 2;
    return {std::clamp(cx - 12 * side, 0, width), std::clamp(cy - 14 * side, 0, height),
            std::clamp(cx + 12 * side, 0, width), std::clamp(cy + 5 * side, 0, height)};
}

void SkillRefresher::start(int cursorX, int cursorY) {
    *this = SkillRefresher();
    running_ = true;
    homeX_ = cursorX;
    homeY_ = cursorY;
}

void SkillRefresher::next(double now) {
    ++index_;
    since_ = now;
    placed_ = false;
    place_ = {};
    cameAt_ = -1.0;
    asked_ = false;
    lastMp_ = -2;
    lastText_.clear();
    glide_.stop();
}

SkillRefresher::Output SkillRefresher::step(const Input& in) {
    Output out;
    if (!running_) {
        return out;
    }
    total_ = in.skills.size();
    char line[512];
    if (!returning_ && index_ >= total_) {
        // After the last: the last line, then back to where the cursor was.
        returning_ = true;
        since_ = in.now;
        glide_.stop();
        std::string missed;
        for (const std::string& name : missed_) {
            missed += (missed.empty() ? "" : ", ") + name;
        }
        std::snprintf(line, sizeof(line), "Refresh done: %zu of %zu skill%s read%s%s%s", total_ - missed_.size(),
                      total_, plural(total_), missed.empty() ? "" : " (not read: ", missed.c_str(),
                      missed.empty() ? "" : ")");
        out.log = line;
    }
    if (returning_) {
        const double toX = homeX_ - in.cursorX;
        const double toY = homeY_ - in.cursorY;
        const CursorGlide::Move move = glide_.step(toX, toY, in.dt);
        out.dx = move.dx;
        out.dy = move.dy;
        if (std::hypot(toX, toY) <= 1.5 || in.now - since_ >= kReturnSeconds) {
            running_ = false;
            out.dx = 0;
            out.dy = 0;
            out.done = true;
        }
        return out;
    }

    const RefreshSkill& skill = in.skills[index_];
    if (since_ < 0.0) {
        since_ = in.now;
    }
    const auto giveUp = [&](const std::string& why) {
        std::snprintf(line, sizeof(line), "Refresh %s: %s", skill.name.c_str(), why.c_str());
        out.log = line;
        missed_.push_back(skill.name);
        next(in.now);
    };
    if (skill.seen) {
        place_ = skill;
        placed_ = true;
    }
    if (!placed_) {
        if (in.now - since_ >= kFindSeconds) {
            giveUp("not found on the hotbar, skipped");
        }
        return out;
    }

    // Onto its middle, and held there.
    const double toX = place_.x - in.cursorX;
    const double toY = place_.y - in.cursorY;
    const CursorGlide::Move move = glide_.step(toX, toY, in.dt);
    out.dx = move.dx;
    out.dy = move.dy;
    const double reach = std::max(2.0, place_.side * 0.2);
    if (std::abs(toX) > reach || std::abs(toY) > reach) {
        cameAt_ = -1.0; // Not on it (yet, or moved off): its tooltip is read only after it has been on it a while.
    } else if (cameAt_ < 0.0) {
        cameAt_ = in.now;
    }

    // An answer: read only off an image shown once the tooltip had time to come up.
    if (in.answered && asked_) {
        asked_ = false;
        if (cameAt_ >= 0.0 && in.shown >= cameAt_ + kTooltipDelay) {
            lastText_ = std::string(in.text.substr(0, kTextInLog));
            const MpCostRead read = readMpCost(in.text, skill.name);
            if (!read.name) {
                lastMp_ = -2;
            } else if (read.mp != lastMp_) {
                lastMp_ = read.mp; // Taken once the next read agrees.
            } else if (read.mp >= 0) {
                out.learned = true;
                out.name = skill.name;
                out.mp = read.mp;
                next(in.now);
                return out;
            } else {
                std::snprintf(line, sizeof(line), "Refresh %s: no MP cost on its tooltip, its cost kept",
                              skill.name.c_str());
                out.log = line;
                next(in.now);
                return out;
            }
        }
    }
    if (cameAt_ >= 0.0 && in.now - cameAt_ >= kReadSeconds) {
        if (lastText_.empty()) {
            giveUp("no tooltip read in 3 s (no text round it; is the game in front?)");
        } else {
            giveUp((lastMp_ == -2 ? "its name not on its tooltip in 3 s (read: \"" : "its MP cost not read twice alike "
                                                                                    "in 3 s (read: \"") +
                   lastText_ + "\")");
        }
        return out;
    }
    if (in.now - since_ >= kSkillSeconds) {
        giveUp("the cursor never came onto it");
        return out;
    }
    if (asked_ && in.now - askedAt_ >= kAskAgain) {
        asked_ = false; // Never answered: asked again.
    }
    if (cameAt_ >= 0.0 && in.now >= cameAt_ + kTooltipDelay && !asked_) {
        out.read = true;
        out.area = tooltipArea(place_.at, in.imageWidth, in.imageHeight);
        asked_ = true;
        askedAt_ = in.now;
    }
    return out;
}

} // namespace observex
