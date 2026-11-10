#include "core/ReviewLines.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

namespace observex {

namespace {

std::string kindName(std::size_t kind, KindIds kinds) {
    if (kinds.empty()) {
        return "yes";
    }
    return kind < kinds.size() ? std::string(kinds[kind]) : "kind " + std::to_string(kind);
}

std::string percent(float value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f%%", static_cast<double>(value * 100.0f));
    return text;
}

long long area(const Box& box) {
    return static_cast<long long>(box.width) * box.height;
}

long long intersection(const Box& a, const Box& b) {
    const long long width = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
    const long long height = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
    return width > 0 && height > 0 ? width * height : 0;
}

bool sameAnswer(const Mark& a, const Mark& b) {
    return a.answer == b.answer && (a.answer != Answer::Yes || a.kind == b.kind);
}

} // namespace

std::string describeBox(const Box& box) {
    char text[64];
    std::snprintf(text, sizeof(text), "[%d %d %d %d]", box.x, box.y, box.width, box.height);
    return text;
}

std::string describeMark(const Mark& mark, KindIds kinds) {
    if (mark.answer == Answer::None) {
        return "unanswered";
    }
    const std::string what = mark.answer == Answer::Yes ? kindName(mark.kind, kinds) : "no";
    if (mark.unchecked) {
        return what + " (kind not checked)";
    }
    if (mark.presumed) {
        return what + " (pre-filled)";
    }
    if (mark.carried) {
        return what + " (copied from the image before)";
    }
    return what + " (yours)";
}

std::string describeWhere(const Mark& mark) {
    return describeBox(mark.box) + (mark.guess >= 0.0f ? " guess " + percent(mark.guess) : "");
}

std::string describeSure(std::span<const float> sure, KindIds kinds) {
    std::vector<std::size_t> order(sure.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return sure[a] > sure[b]; });
    std::string text;
    for (const std::size_t kind : order) {
        if (sure[kind] >= 0.005f) {
            text += (text.empty() ? "" : ", ") + kindName(kind, kinds) + " " + percent(sure[kind]);
        }
    }
    return text.empty() ? "nothing" : text;
}

std::string describePrefill(const Mark& mark, std::span<const float> own, std::span<const float> learned) {
    if (mark.answer == Answer::None) {
        // Sure enough to start as yes: only another answer at its place on the image before (a no or another kind)
        // held it back (carryAnswers).
        const int kind = own.empty() ? presumedKind(mark) : presumedKind(own, learned);
        return kind >= 0 ? "asked, though sure enough to pre-fill: the image before had another answer here" : "asked";
    }
    if (mark.carried) {
        return "copied from the image before";
    }
    if (!mark.presumed) {
        return "answered";
    }
    if (own.empty()) {
        return "the model is " + percent(kPresumeYes) + "+ sure";
    }
    if (sureKind(own, kPresumeYes) == mark.kind && sureKind(learned, kPresumeYes) == mark.kind) {
        return "the model and the re-fit agree, both " + percent(kPresumeYes) + "+ sure";
    }
    return "the re-fit is " + percent(kPresumeLearned) + "+ sure";
}

std::vector<std::string> describeOverlaps(const std::vector<Mark>& marks, std::size_t index, KindIds kinds) {
    std::vector<std::string> lines;
    if (index >= marks.size()) {
        return lines;
    }
    const Box& box = marks[index].box;
    for (std::size_t i = 0; i < marks.size(); ++i) {
        const Box& other = marks[i].box;
        const long long shared = intersection(box, other);
        if (i == index || shared == 0) {
            continue;
        }
        // Worth saying: a real overlap, or one box mostly inside the other (one box over two characters, a box
        // drawn inside a guess).
        const float iou = overlap(box, other);
        const float thisInside = area(box) > 0 ? static_cast<float>(shared) / static_cast<float>(area(box)) : 0.0f;
        const float itInside = area(other) > 0 ? static_cast<float>(shared) / static_cast<float>(area(other)) : 0.0f;
        if (iou < 0.1f && thisInside < 0.5f && itInside < 0.5f) {
            continue;
        }
        lines.push_back("mark " + std::to_string(i) + " " + describeWhere(marks[i]) + " " +
                        describeMark(marks[i], kinds) + ": overlap " + percent(iou) + ", " + percent(thisInside) +
                        " of this box inside it, " + percent(itInside) + " of it inside this box");
    }
    return lines;
}

std::vector<std::string> describeChanges(const std::vector<Mark>& before, const std::vector<Mark>& after,
                                         KindIds kinds) {
    std::vector<std::string> lines;
    std::vector<bool> matched(before.size());
    int kept = 0;
    for (std::size_t i = 0; i < after.size(); ++i) {
        const Mark& now = after[i];
        std::size_t was = 0;
        while (was < before.size() && (matched[was] || !(before[was].box == now.box))) {
            ++was;
        }
        const std::string where = "mark " + std::to_string(i) + " " + describeWhere(now) + ": ";
        if (was == before.size()) {
            lines.push_back("drew " + where + describeMark(now, kinds));
            continue;
        }
        matched[was] = true;
        const Mark& then = before[was];
        if (!sameAnswer(then, now)) {
            lines.push_back("changed " + where + describeMark(then, kinds) + " -> " + describeMark(now, kinds));
        } else if ((then.presumed || then.carried) && !now.presumed && !now.carried) {
            // Answered again the same: the pre-fill was right, the click was not needed.
            lines.push_back("confirmed " + where + describeMark(then, kinds) + " -> " + describeMark(now, kinds));
        } else {
            ++kept;
        }
    }
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (!matched[i]) {
            lines.push_back("removed mark " + std::to_string(i) + " " + describeWhere(before[i]) + ": " +
                            describeMark(before[i], kinds));
        }
    }
    if (kept > 0 && !lines.empty()) {
        lines.push_back("kept " + std::to_string(kept) + (kept == 1 ? " mark" : " marks") + " as shown");
    } else if (kept > 0) {
        lines.push_back("kept all " + std::to_string(kept) + (kept == 1 ? " mark" : " marks") + " as shown");
    }
    return lines;
}

std::string describeCounts(const std::vector<Mark>& marks, KindIds kinds) {
    const std::size_t yesKinds = std::max<std::size_t>(kinds.size(), 1);
    std::vector<int> yes(yesKinds);
    int no = 0;
    int unanswered = 0;
    for (const Mark& mark : marks) {
        if (mark.answer == Answer::Yes) {
            ++yes[std::min<std::size_t>(mark.kind, yesKinds - 1)];
        } else {
            ++(mark.answer == Answer::No ? no : unanswered);
        }
    }
    std::string text;
    const auto add = [&](const std::string& name, int count) {
        if (count > 0) {
            text += (text.empty() ? "" : ", ") + name + " " + std::to_string(count);
        }
    };
    for (std::size_t kind = 0; kind < yesKinds; ++kind) {
        add(kindName(kind, kinds), yes[kind]);
    }
    add("no", no);
    add("unanswered", unanswered);
    return text.empty() ? "no marks" : text;
}

std::string describeBoxFit(const BoxLearner& learner, double ms) {
    if (learner.learnedFrom() == 0) {
        return "learned from no answered guesses yet: the finder's boxes";
    }
    const Nudge& average = learner.average();
    char text[256];
    std::snprintf(text, sizeof(text),
                  "learned from %zu answered guesses in %.1f ms; the user's boxes near this image on average: %.2f of "
                  "the guess's width, %.2f of its height, the middle %.0f%% of it %s, %.0f%% %s",
                  learner.learnedFrom(), ms, std::exp(static_cast<double>(average.width)),
                  std::exp(static_cast<double>(average.height)), std::abs(average.x) * 100.0,
                  average.x < 0.0f ? "left" : "right", std::abs(average.y) * 100.0, average.y < 0.0f ? "up" : "down");
    return text;
}

std::string describeFit(const KindLearner& learner, std::span<const int> answers, KindIds kinds, double ms) {
    if (answers.empty()) {
        return "learned from no answers yet: the model's own kinds";
    }
    const std::size_t count = static_cast<std::size_t>(std::max(learner.kinds(), 0));
    std::vector<int> perKind(count);
    int no = 0;
    for (const int answer : answers) {
        if (answer >= 0 && static_cast<std::size_t>(answer) < count) {
            ++perKind[static_cast<std::size_t>(answer)];
        } else {
            ++no;
        }
    }
    std::string text = "learned from " + std::to_string(answers.size()) + " answers (";
    std::string split;
    for (std::size_t kind = 0; kind < count; ++kind) {
        if (perKind[kind] > 0) {
            split += (split.empty() ? "" : ", ") + kindName(kind, kinds) + " " + std::to_string(perKind[kind]);
        }
    }
    if (no > 0) {
        split += (split.empty() ? "" : ", ") + std::string("no ") + std::to_string(no);
    }
    char numbers[64];
    std::snprintf(numbers, sizeof(numbers), ") in %.1f ms; ", ms);
    text += split + numbers;

    // How far the nudge moved the classifier from the model's: the weights' change against their size, each bias's.
    const KindHead& own = learner.own();
    const KindHead& learned = learner.learned();
    double moved = 0.0;
    double size = 0.0;
    for (std::size_t i = 0; i < own.weights.size() && i < learned.weights.size(); ++i) {
        const double change = learned.weights[i] - own.weights[i];
        moved += change * change;
        size += static_cast<double>(own.weights[i]) * own.weights[i];
    }
    std::snprintf(numbers, sizeof(numbers), "weights moved %.1f%% from the model's; biases",
                  size > 0.0 ? 100.0 * std::sqrt(moved / size) : 0.0);
    text += numbers;
    for (std::size_t kind = 0; kind < own.biases.size() && kind < learned.biases.size(); ++kind) {
        std::snprintf(numbers, sizeof(numbers), " %+.2f", static_cast<double>(learned.biases[kind] - own.biases[kind]));
        text += (kind == 0 ? " " : ", ") + kindName(kind, kinds) + numbers;
    }
    return text;
}

} // namespace observex
