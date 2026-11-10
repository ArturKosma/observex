#include "core/BoxLearner.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace observex {

namespace {

constexpr std::size_t kParts = 4; // x, y, width, height.

float part(const Nudge& nudge, std::size_t index) {
    const float parts[kParts] = {nudge.x, nudge.y, nudge.width, nudge.height};
    return parts[index];
}

// Keeps [`start`, `start` + `length`) inside [`from`, `from` + `room`), at least 1 long.
void keepInside(int& start, int& length, int from, int room) {
    const int end = std::min(start + length, from + room);
    start = std::clamp(start, from, from + room - 1);
    length = std::max(end - start, 1);
}

// Solves `a` x = `b` for x into `b` (`n` x `columns`, row by row), `a` (`n` x `n`, row by row) symmetric and
// positive definite, by its Cholesky factor (written over `a`). False if `a` is not positive definite.
bool solve(std::vector<double>& a, std::vector<double>& b, std::size_t n, std::size_t columns) {
    for (std::size_t j = 0; j < n; ++j) {
        double diagonal = a[j * n + j];
        for (std::size_t k = 0; k < j; ++k) {
            diagonal -= a[j * n + k] * a[j * n + k];
        }
        if (!(diagonal > 0.0)) {
            return false;
        }
        const double root = std::sqrt(diagonal);
        a[j * n + j] = root;
        for (std::size_t i = j + 1; i < n; ++i) {
            double value = a[i * n + j];
            for (std::size_t k = 0; k < j; ++k) {
                value -= a[i * n + k] * a[j * n + k];
            }
            a[i * n + j] = value / root;
        }
    }
    for (std::size_t c = 0; c < columns; ++c) {
        for (std::size_t i = 0; i < n; ++i) { // L y = b.
            double value = b[i * columns + c];
            for (std::size_t k = 0; k < i; ++k) {
                value -= a[i * n + k] * b[k * columns + c];
            }
            b[i * columns + c] = value / a[i * n + i];
        }
        for (std::size_t i = n; i-- > 0;) { // L^T x = y.
            double value = b[i * columns + c];
            for (std::size_t k = i + 1; k < n; ++k) {
                value -= a[k * n + i] * b[k * columns + c];
            }
            b[i * columns + c] = value / a[i * n + i];
        }
    }
    return true;
}

} // namespace

Nudge nudgeBetween(const Box& guess, const Box& box) {
    const float width = static_cast<float>(std::max(guess.width, 1));
    const float height = static_cast<float>(std::max(guess.height, 1));
    return Nudge{(box.x + box.width * 0.5f - guess.x - guess.width * 0.5f) / width,
                 (box.y + box.height * 0.5f - guess.y - guess.height * 0.5f) / height,
                 std::log(static_cast<float>(std::max(box.width, 1)) / width),
                 std::log(static_cast<float>(std::max(box.height, 1)) / height)};
}

Box nudged(const Box& guess, const Nudge& nudge, const Box& image) {
    const float width = guess.width * std::exp(std::clamp(nudge.width, kMinNudgeSize, kMaxNudgeSize));
    const float height = guess.height * std::exp(std::clamp(nudge.height, kMinNudgeSize, kMaxNudgeSize));
    const float middleX = guess.x + guess.width * (0.5f + std::clamp(nudge.x, -kMaxNudgeMove, kMaxNudgeMove));
    const float middleY = guess.y + guess.height * (0.5f + std::clamp(nudge.y, -kMaxNudgeMove, kMaxNudgeMove));
    Box box{static_cast<int>(std::lround(middleX - width * 0.5f)), static_cast<int>(std::lround(middleY - height * 0.5f)),
            std::max(static_cast<int>(std::lround(width)), 1), std::max(static_cast<int>(std::lround(height)), 1)};
    if (image.width > 0 && image.height > 0) {
        keepInside(box.x, box.width, image.x, image.width);
        keepInside(box.y, box.height, image.y, image.height);
    }
    return box;
}

std::size_t addAnsweredBoxes(const ImageMarks& guesses, const GuessFeatures& features, const ImageMarks& answers,
                             std::span<const std::string> images, BoxSamples& samples) {
    const std::size_t before = samples.count();
    if (samples.count() == 0) {
        samples.size = features.size;
    }
    if (features.size <= 0 || samples.size != features.size) {
        return 0;
    }
    for (const auto& [name, answered] : answers) {
        const auto guessed = guesses.find(name);
        const auto place = std::lower_bound(images.begin(), images.end(), name);
        if (guessed == guesses.end() || place == images.end() || *place != name) {
            continue;
        }
        const std::vector<int> matched = matchAnswers(guessed->second, answered);
        for (std::size_t i = 0; i < matched.size(); ++i) {
            const std::span<const float> feature = features.of(name, i);
            if (matched[i] < 0 || feature.empty()) {
                continue;
            }
            samples.features.insert(samples.features.end(), feature.begin(), feature.end());
            samples.nudges.push_back(
                nudgeBetween(guessed->second[i].box, answered[static_cast<std::size_t>(matched[i])].box));
            samples.images.push_back(static_cast<int>(place - images.begin()));
        }
    }
    return samples.count() - before;
}

void BoxLearner::fit(const BoxSamples& samples, int at, float pull, float halfLife) {
    size_ = samples.size;
    learnedFrom_ = 0;
    average_ = {};
    weights_.clear();
    if (samples.count() == 0 || size_ <= 0) {
        return;
    }
    // The normal equations: (X^T W X + pull) weights = X^T W nudges, X a sample's features and a 1 for the bias.
    const std::size_t n = static_cast<std::size_t>(size_) + 1;
    std::vector<double> a(n * n);
    std::vector<double> b(n * kParts);
    std::vector<double> input(n);
    double total = 0.0;
    double averages[kParts] = {};
    for (std::size_t s = 0; s < samples.count(); ++s) {
        const double weight =
            halfLife > 0.0f ? std::exp2(-std::abs(at - samples.images[s]) / static_cast<double>(halfLife)) : 1.0;
        const float* feature = samples.features.data() + s * static_cast<std::size_t>(size_);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            input[i] = feature[i];
        }
        input[n - 1] = 1.0;
        for (std::size_t i = 0; i < n; ++i) { // The upper half; the lower is the same.
            const double weighted = weight * input[i];
            double* row = a.data() + i * n;
            for (std::size_t j = i; j < n; ++j) {
                row[j] += weighted * input[j];
            }
            for (std::size_t k = 0; k < kParts; ++k) {
                b[i * kParts + k] += weighted * part(samples.nudges[s], k);
            }
        }
        total += weight;
        for (std::size_t k = 0; k < kParts; ++k) {
            averages[k] += weight * part(samples.nudges[s], k);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            a[i * n + j] = a[j * n + i];
        }
        a[i * n + i] += i + 1 < n ? pull : pull * 0.01;
    }
    if (!solve(a, b, n, kParts)) {
        return; // Only with no pull and too few samples: it does not nudge.
    }
    weights_.resize(b.size());
    for (std::size_t i = 0; i < b.size(); ++i) {
        weights_[i] = static_cast<float>(b[i]);
    }
    learnedFrom_ = samples.count();
    average_ = Nudge{static_cast<float>(averages[0] / total), static_cast<float>(averages[1] / total),
                     static_cast<float>(averages[2] / total), static_cast<float>(averages[3] / total)};
}

Nudge BoxLearner::nudge(std::span<const float> feature) const {
    if (learnedFrom_ == 0 || feature.size() != static_cast<std::size_t>(size_)) {
        return {};
    }
    float parts[kParts] = {};
    for (std::size_t k = 0; k < kParts; ++k) {
        float value = weights_[feature.size() * kParts + k]; // The bias.
        for (std::size_t i = 0; i < feature.size(); ++i) {
            value += feature[i] * weights_[i * kParts + k];
        }
        parts[k] = value;
    }
    return Nudge{parts[0], parts[1], parts[2], parts[3]};
}

} // namespace observex
