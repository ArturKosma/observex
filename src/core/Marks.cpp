#include "core/Marks.h"

#include "core/Detection.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <system_error>
#include <utility>

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

// The index of `word` among `kinds`, or -1.
int findKind(KindIds kinds, std::string_view word) {
    const auto found = std::find(kinds.begin(), kinds.end(), word);
    return found != kinds.end() ? static_cast<int>(found - kinds.begin()) : -1;
}

// Whether `word` is a word of letters, as kinds are.
bool isWord(std::string_view word) {
    return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
}

// "monster 1012 540 80 120", "yes 1012 540 80 120", "yes player 1012 540 80 120", "no ...", "0.72 ..." or
// "0.82 monster 0.70 player 0.40 ...".
bool parseMark(std::string_view line, KindIds kinds, Mark& mark) {
    const std::string_view what = takeWord(line);
    if (const int kind = findKind(kinds, what); kind >= 0) {
        mark.answer = Answer::Yes;
        mark.kind = static_cast<std::uint8_t>(kind);
    } else if (what == "yes") {
        mark.answer = Answer::Yes;
        mark.unchecked = !kinds.empty();
        // A suggested kind may follow; a number there is already the box.
        std::string_view rest = line;
        if (const int suggested = findKind(kinds, takeWord(rest)); suggested >= 0) {
            mark.kind = static_cast<std::uint8_t>(suggested);
            line = rest;
        }
    } else if (what == "no") {
        mark.answer = Answer::No;
    } else if (!readNumber(what, mark.guess) || mark.guess < 0.0f || mark.guess > 1.0f) {
        return false;
    } else {
        // Maybe the kind it was surest of and the next, each with how sure. A first kind not among `kinds` is left at
        // the first, a second one left out.
        for (int named = 0; named < 2; ++named) {
            std::string_view rest = line;
            const std::string_view word = takeWord(rest);
            float sure = 0.0f;
            if (!isWord(word) || !readNumber(takeWord(rest), sure) || sure < 0.0f || sure > 1.0f) {
                break;
            }
            const int found = findKind(kinds, word);
            if (named == 0) {
                mark.kind = static_cast<std::uint8_t>(std::max(0, found));
                mark.kindGuess = sure;
            } else if (found >= 0) {
                mark.secondKind = static_cast<std::uint8_t>(found);
                mark.secondGuess = sure;
            }
            line = rest;
        }
    }
    Box& box = mark.box;
    return readNumber(takeWord(line), box.x) && readNumber(takeWord(line), box.y) &&
           readNumber(takeWord(line), box.width) && readNumber(takeWord(line), box.height) && trim(line).empty() &&
           box.width > 0 && box.height > 0;
}

// The mark in `previous` that overlaps `box` most, at least `minOverlap`, among those `counts` accepts; or null.
template <typename Counts>
const Mark* mostOverlapping(const std::vector<Mark>& previous, const Box& box, float minOverlap, Counts counts) {
    const Mark* best = nullptr;
    float bestOverlap = minOverlap;
    for (const Mark& before : previous) {
        const float amount = counts(before) ? overlap(box, before.box) : 0.0f;
        if (amount >= bestOverlap) {
            best = &before;
            bestOverlap = amount;
        }
    }
    return best;
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

float shareInside(const Box& a, const Box& b) {
    const int width = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
    const int height = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
    if (width <= 0 || height <= 0 || a.width <= 0 || a.height <= 0) {
        return 0.0f;
    }
    return static_cast<float>(static_cast<double>(width) * height / (static_cast<double>(a.width) * a.height));
}

ImageMarks parseMarks(std::string_view text, KindIds kinds) {
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
        if (image != nullptr && parseMark(line, kinds, mark)) {
            image->push_back(mark);
        }
    }
    return marks;
}

std::string formatMarks(const ImageMarks& marks, std::string_view comment, KindIds kinds) {
    std::string text;
    while (!comment.empty()) {
        const std::size_t end = std::min(comment.find('\n'), comment.size());
        text += "# ";
        text += comment.substr(0, end);
        text += '\n';
        comment.remove_prefix(std::min(end + 1, comment.size()));
    }
    char line[128];
    for (const auto& [name, imageMarks] : marks) {
        text += "image ";
        text += name;
        text += '\n';
        for (const Mark& mark : imageMarks) {
            const Box& box = mark.box;
            if (mark.answer == Answer::Yes && mark.kind < kinds.size()) {
                const std::string_view kind = kinds[mark.kind];
                std::snprintf(line, sizeof(line), "%s%.*s %d %d %d %d\n", mark.unchecked ? "yes " : "",
                              static_cast<int>(kind.size()), kind.data(), box.x, box.y, box.width, box.height);
            } else if (mark.answer != Answer::None) {
                std::snprintf(line, sizeof(line), "%s %d %d %d %d\n", mark.answer == Answer::Yes ? "yes" : "no", box.x,
                              box.y, box.width, box.height);
            } else if (mark.guess >= 0.0f && mark.kind < kinds.size()) {
                // With its kinds: "0.820 monster 0.700 player 0.400 ...", without the second if it has none.
                const std::string_view kind = kinds[mark.kind];
                int length = std::snprintf(line, sizeof(line), "%.3f %.*s %.3f ", static_cast<double>(mark.guess),
                                           static_cast<int>(kind.size()), kind.data(),
                                           static_cast<double>(kindGuessOf(mark)));
                if (mark.secondGuess >= 0.0f && mark.secondKind < kinds.size()) {
                    const std::string_view second = kinds[mark.secondKind];
                    length +=
                        std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length), "%.*s %.3f ",
                                      static_cast<int>(second.size()), second.data(),
                                      static_cast<double>(mark.secondGuess));
                }
                std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length), "%d %d %d %d\n", box.x,
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

bool kindsChecked(const std::vector<Mark>& marks) {
    return std::none_of(marks.begin(), marks.end(), [](const Mark& mark) { return mark.unchecked; });
}

bool fullyAnswered(const std::vector<Mark>& marks) {
    return kindsChecked(marks) &&
           std::none_of(marks.begin(), marks.end(), [](const Mark& mark) { return mark.answer == Answer::None; });
}

int carryAnswers(const std::vector<Mark>& previous, std::vector<Mark>& marks, std::span<int> presumed,
                 float minOverlap) {
    int carried = 0;
    for (std::size_t i = 0; i < marks.size(); ++i) {
        Mark& mark = marks[i];
        if (mark.answer != Answer::None) {
            continue;
        }
        const Mark* best = mostOverlapping(previous, mark.box, minOverlap, [&](const Mark& before) {
            return before.answer != Answer::None && !boxedAgain(before, previous);
        });
        const bool sure = i < presumed.size() && presumed[i] >= 0;
        if (best != nullptr && sure && (best->answer == Answer::No || best->kind != presumed[i])) {
            presumed[i] = -1; // Sure it is something else than the answer before: the user answers it.
        } else if (best != nullptr) {
            mark.answer = best->answer;
            mark.kind = best->kind;
            mark.carried = true;
            ++carried;
        }
    }
    return carried;
}

bool boxedAgain(const Mark& mark, const std::vector<Mark>& marks) {
    if (mark.answer != Answer::No) {
        return false;
    }
    int inside = 0; // Yes marks inside it as a group's,
    int tighter = 0; // and as a tighter box.
    for (const Mark& other : marks) {
        if (other.answer != Answer::Yes) {
            continue;
        }
        const float share = shareInside(other.box, mark.box);
        inside += share >= kGroupInside ? 1 : 0;
        tighter += share >= kTightInside && overlap(other.box, mark.box) >= kTightOverlap ? 1 : 0;
    }
    return inside < 2 && tighter == 1;
}

int carryKinds(const std::vector<Mark>& previous, std::vector<Mark>& marks, float minOverlap) {
    int carried = 0;
    for (Mark& mark : marks) {
        if (mark.answer != Answer::Yes || !mark.unchecked) {
            continue;
        }
        const Mark* best = mostOverlapping(previous, mark.box, minOverlap, [](const Mark& before) {
            return before.answer == Answer::Yes && !before.unchecked;
        });
        if (best != nullptr) {
            mark.kind = best->kind;
            mark.carried = true;
            ++carried;
        }
    }
    return carried;
}

std::vector<int> matchAnswers(const std::vector<Mark>& guesses, const std::vector<Mark>& answers, float minOverlap) {
    std::vector<std::size_t> order(guesses.size()); // Surest first; equally sure ones in their order.
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return guesses[a].guess > guesses[b].guess; });
    std::vector<int> matched(guesses.size(), -1);
    std::vector<bool> taken(answers.size());
    for (const std::size_t i : order) {
        const Box& guess = guesses[i].box;
        const auto group = std::count_if(answers.begin(), answers.end(), [&](const Mark& answer) {
                               return answer.answer == Answer::Yes && shareInside(answer.box, guess) >= kGroupInside;
                           }) >= 2;
        int best = -1; // The first of those it overlaps most.
        float bestOverlap = 0.0f;
        for (std::size_t j = 0; j < answers.size(); ++j) {
            if (answers[j].answer != Answer::Yes || taken[j]) {
                continue;
            }
            const float amount = overlap(guess, answers[j].box);
            const bool tighter =
                !group && amount >= kTightOverlap && shareInside(answers[j].box, guess) >= kTightInside;
            if ((amount >= minOverlap || tighter) && amount > bestOverlap) {
                best = static_cast<int>(j);
                bestOverlap = amount;
            }
        }
        if (best >= 0) {
            taken[static_cast<std::size_t>(best)] = true;
            matched[i] = best;
        }
    }
    return matched;
}

std::vector<int> matchGuesses(const std::vector<Mark>& guesses, const std::vector<Mark>& answers, float minOverlap) {
    std::vector<int> kinds = matchAnswers(guesses, answers, minOverlap);
    for (int& kind : kinds) {
        kind = kind >= 0 ? answers[static_cast<std::size_t>(kind)].kind : -1;
    }
    return kinds;
}

namespace {

// The surest kind in `sure` and the next (-1 if there is none).
std::pair<int, int> surestTwo(std::span<const float> sure) {
    int first = -1;
    int second = -1;
    for (std::size_t kind = 0; kind < sure.size(); ++kind) {
        if (first < 0 || sure[kind] > sure[static_cast<std::size_t>(first)]) {
            second = first;
            first = static_cast<int>(kind);
        } else if (second < 0 || sure[kind] > sure[static_cast<std::size_t>(second)]) {
            second = static_cast<int>(kind);
        }
    }
    return {first, second};
}

} // namespace

int sureKind(std::span<const float> sure, float minSure) {
    const auto [first, second] = surestTwo(sure);
    if (first < 0 || sure[static_cast<std::size_t>(first)] < minSure) {
        return -1;
    }
    const float next = second >= 0 ? sure[static_cast<std::size_t>(second)] : -1.0f;
    return kindsClose(sure[static_cast<std::size_t>(first)], next) ? -1 : first;
}

int presumedKind(std::span<const float> own, std::span<const float> learned) {
    const int both = sureKind(own, kPresumeYes);
    if (both >= 0 && both == sureKind(learned, kPresumeYes)) {
        return both;
    }
    return sureKind(learned, kPresumeLearned);
}

int presumedKind(const Mark& guess) {
    const float sure = kindGuessOf(guess);
    return sure >= kPresumeYes && !kindsClose(sure, guess.secondGuess) ? guess.kind : -1;
}

void showKinds(Mark& mark, std::span<const float> sure) {
    const auto [first, second] = surestTwo(sure);
    if (first < 0) {
        return;
    }
    mark.kind = static_cast<std::uint8_t>(first);
    mark.kindGuess = sure[static_cast<std::size_t>(first)];
    mark.secondKind = static_cast<std::uint8_t>(std::max(second, 0));
    mark.secondGuess = second >= 0 ? sure[static_cast<std::size_t>(second)] : -1.0f;
}

int presumeKinds(std::vector<Mark>& marks, std::span<const int> kinds) {
    int presumed = 0;
    for (std::size_t i = 0; i < marks.size() && i < kinds.size(); ++i) {
        Mark& mark = marks[i];
        if (mark.answer == Answer::None && kinds[i] >= 0) {
            mark.answer = Answer::Yes;
            mark.kind = static_cast<std::uint8_t>(kinds[i]);
            mark.presumed = true;
            ++presumed;
        }
    }
    return presumed;
}

} // namespace observex
