#include "core/KindLearner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace observex {

namespace {

float sigmoid(float score) {
    return 1.0f / (1.0f + std::exp(-score));
}

// a . b over `count` numbers, in eight running sums: the processor adds those side by side, one sum it must add in
// order, which takes several times longer. Written just so (eight named sums, not an array, and the loop's end worked
// out first) the compiler (MSVC) keeps the sums in registers side by side: a fit of 2,000 answers took 44 ms with an
// array, 18 like this (2026-11-08).
float dot(const float* a, const float* b, std::size_t count) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f, s5 = 0.0f, s6 = 0.0f, s7 = 0.0f;
    const std::size_t whole = count - count % 8;
    std::size_t i = 0;
    for (; i < whole; i += 8) {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
        s4 += a[i + 4] * b[i + 4];
        s5 += a[i + 5] * b[i + 5];
        s6 += a[i + 6] * b[i + 6];
        s7 += a[i + 7] * b[i + 7];
    }
    float total = ((s0 + s1) + (s2 + s3)) + ((s4 + s5) + (s6 + s7));
    for (; i < count; ++i) {
        total += a[i] * b[i];
    }
    return total;
}

// How sure `head` is of each kind of `feature`.
void sureOf(const KindHead& head, std::span<const float> feature, std::span<float> sure) {
    const std::size_t size = static_cast<std::size_t>(head.features());
    for (std::size_t kind = 0; kind < head.biases.size(); ++kind) {
        sure[kind] = sigmoid(dot(feature.data(), head.weights.data() + kind * size, size) + head.biases[kind]);
    }
}

// One step of Adam (Kingma and Ba, 2015), as PyTorch takes it with its default settings: moves each of `values`
// against its `gradient`, scaled by the running averages of its gradient (`mean`) and squared gradient (`square`),
// which it keeps up to date. `step` counts from 1.
void adamStep(std::span<float> values, std::span<const float> gradient, std::span<float> mean, std::span<float> square,
              int step) {
    constexpr float kMeanRate = 0.9f;
    constexpr float kSquareRate = 0.999f;
    constexpr float kTiny = 1e-8f;
    // The averages start at 0, so early on they are too small: by these shares.
    const double meanShare = 1.0 - std::pow(0.9, step);
    const double squareShare = 1.0 - std::pow(0.999, step);
    const float stepSize = static_cast<float>(KindLearner::kStepSize / meanShare);
    const float squareShareRoot = static_cast<float>(std::sqrt(squareShare));
    for (std::size_t i = 0; i < values.size(); ++i) {
        mean[i] += (1.0f - kMeanRate) * (gradient[i] - mean[i]);
        square[i] = square[i] * kSquareRate + (1.0f - kSquareRate) * gradient[i] * gradient[i];
        values[i] -= stepSize * (mean[i] / (std::sqrt(square[i]) / squareShareRoot + kTiny));
    }
}

} // namespace

KindLearner::KindLearner(KindHead own) : own_(std::move(own)), learned_(own_) {}

void KindLearner::fit(std::span<const float> features, std::span<const int> answers, int steps, float pull) {
    learned_ = own_;
    learnedFrom_ = answers.size();
    if (answers.empty()) {
        return;
    }
    const std::size_t kinds = own_.biases.size();
    const std::size_t size = static_cast<std::size_t>(own_.features());
    // Each answer's each kind counts the same: the loss is their mean.
    const float share = 1.0f / static_cast<float>(answers.size() * kinds);
    std::vector<float> weightGradient(own_.weights.size());
    std::vector<float> weightMean(own_.weights.size());
    std::vector<float> weightSquare(own_.weights.size());
    std::vector<float> biasGradient(kinds);
    std::vector<float> biasMean(kinds);
    std::vector<float> biasSquare(kinds);
    for (int step = 1; step <= steps; ++step) {
        // The gradient of the mean binary cross-entropy: per kind, how much surer it is than the answer says.
        std::fill(weightGradient.begin(), weightGradient.end(), 0.0f);
        std::fill(biasGradient.begin(), biasGradient.end(), 0.0f);
        for (std::size_t answer = 0; answer < answers.size(); ++answer) {
            const float* feature = features.data() + answer * size;
            for (std::size_t kind = 0; kind < kinds; ++kind) {
                const float* weights = learned_.weights.data() + kind * size;
                const float target = answers[answer] == static_cast<int>(kind) ? 1.0f : 0.0f;
                const float error = (sigmoid(dot(feature, weights, size) + learned_.biases[kind]) - target) * share;
                biasGradient[kind] += error;
                float* gradient = weightGradient.data() + kind * size;
                for (std::size_t i = 0; i < size; ++i) {
                    gradient[i] += error * feature[i];
                }
            }
        }
        // And of the pull back towards the model's own weights.
        for (std::size_t i = 0; i < weightGradient.size(); ++i) {
            weightGradient[i] += pull * (2.0f * (learned_.weights[i] - own_.weights[i]));
        }
        adamStep(learned_.weights, weightGradient, weightMean, weightSquare, step);
        adamStep(learned_.biases, biasGradient, biasMean, biasSquare, step);
    }
}

void KindLearner::sure(std::span<const float> feature, std::span<float> own, std::span<float> learned) const {
    sureOf(own_, feature, own);
    sureOf(learned_, feature, learned);
}

void pickKinds(const KindHead& head, std::span<const float> feature, Detection& thing) {
    const int kinds = head.kinds();
    if (kinds == 0 || kinds > kMaxKinds || feature.size() != static_cast<std::size_t>(head.features())) {
        return;
    }
    std::array<float, kMaxKinds> sure;
    sureOf(head, feature, sure);
    float total = 0.0f;
    int first = 0; // The surest kind and the next.
    int second = -1;
    for (int kind = 0; kind < kinds; ++kind) {
        total += sure[static_cast<std::size_t>(kind)];
        if (kind == 0) {
            continue;
        }
        if (sure[static_cast<std::size_t>(kind)] > sure[static_cast<std::size_t>(first)]) {
            second = first;
            first = kind;
        } else if (second < 0 || sure[static_cast<std::size_t>(kind)] > sure[static_cast<std::size_t>(second)]) {
            second = kind;
        }
    }
    if (!(total > 0.0f)) {
        return; // Every kind's sigmoid came out 0: no shares to take.
    }
    const float perShare = thing.sure / total;
    thing.kind = static_cast<std::uint8_t>(first);
    thing.kindSure = sure[static_cast<std::size_t>(first)] * perShare;
    if (second >= 0) {
        thing.secondKind = static_cast<std::uint8_t>(second);
        thing.second = sure[static_cast<std::size_t>(second)] * perShare;
    }
}

std::size_t addAnsweredGuesses(const ImageMarks& guesses, const GuessFeatures& features, const ImageMarks& answers,
                               int kinds, std::vector<float>& sampleFeatures, std::vector<int>& sampleAnswers) {
    const std::size_t before = sampleAnswers.size();
    for (const auto& [name, answered] : answers) {
        const auto guessed = guesses.find(name);
        if (guessed == guesses.end() || !fullyAnswered(answered)) {
            continue;
        }
        const std::vector<int> matched = matchGuesses(guessed->second, answered);
        for (std::size_t i = 0; i < matched.size(); ++i) {
            const std::span<const float> feature = features.of(name, i);
            if (matched[i] >= kinds || feature.empty()) {
                continue;
            }
            sampleFeatures.insert(sampleFeatures.end(), feature.begin(), feature.end());
            sampleAnswers.push_back(matched[i] >= 0 ? matched[i] : KindLearner::kNo);
        }
    }
    return sampleAnswers.size() - before;
}

} // namespace observex
