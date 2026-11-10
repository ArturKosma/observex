#pragma once

#include "core/Features.h"
#include "core/Marks.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace observex {

// How a box differs from a guess's: its middle moved by `x` of the guess's width and `y` of its height, its width and
// height e^`width` and e^`height` times the guess's. All 0: the guess's own box.
struct Nudge {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool operator==(const Nudge&) const = default;
};

// The most a nudge moves a box: its middle by half its width or height, its width and height to between e^-1 (0.37)
// and e^0.5 (1.65) times.
inline constexpr float kMaxNudgeMove = 0.5f;
inline constexpr float kMinNudgeSize = -1.0f;
inline constexpr float kMaxNudgeSize = 0.5f;

// The nudge from box `guess` to box `box`.
Nudge nudgeBetween(const Box& guess, const Box& box);

// `guess` nudged by `nudge` (held to the most above), in whole pixels, at least 1 x 1, and kept inside `image` (a box
// at 0, 0 the image's size) unless that is empty.
Box nudged(const Box& guess, const Nudge& nudge, const Box& image = {});

// What a box fit learns from: guesses the user answered, each with its features, the nudge from its box to the
// user's, and which image of the recording it is on (its place in name order).
struct BoxSamples {
    int size = 0;                // Numbers per feature.
    std::vector<float> features; // `size` per sample.
    std::vector<Nudge> nudges;
    std::vector<int> images;

    std::size_t count() const { return nudges.size(); }
};

// Every guess (`guesses`, with their `features`) on each reviewed image (in `answers`) that matches a yes answer
// (core/Marks' matchAnswers: also the user's tighter box around it), with the nudge from its box to that answer's:
// none for a guess kept as the finder boxed it, the user's for one they drew again (a no on it, then a new box). The
// place of each image among `images` (the recording's, in name order) is its sample's image. Guesses matching no yes
// answer teach nothing (no box to learn), nor those without features. Returns how many it added.
std::size_t addAnsweredBoxes(const ImageMarks& guesses, const GuessFeatures& features, const ImageMarks& answers,
                             std::span<const std::string> images, BoxSamples& samples);

// Learns how the user boxes things from how they answered the finder's guesses in a review, and nudges the next
// guesses' boxes to match (the user asked, 2026-11-08: "fixing every single box is absolute madness so i need better
// autoguesses"). From the same features the kind classifier reads (core/KindLearner), a straight line to the nudge,
// fitted by ridge regression: the least squared error over the samples, plus kPull times the sum of the squared
// weights (the bias only a hundredth of that), so a few answers nudge only by their average. The boxes of images near
// the one shown count most: a sample's weight halves every kHalfLife images away (the place and the user's way of
// boxing change through a recording). With no samples it does not nudge.
// Simulated in Python on the user's first review boxing far-off characters tight (2026-11-06_20-41-55, 110 images,
// 2026-11-08; each image fitted to the images before it, as the review does): of the 364 guesses the user drew again,
// those within 0.7 overlap of the user's box 1% -> 63% (within 0.8: 0% -> 34%; overlap on average 0.42 -> 0.73),
// of the 326 kept as boxed 93% stay within 0.7 (88% within 0.8). On a review that kept the boxes (2026-11-06_20-51-48,
// 658 kept, 18 drawn again) it nudged nothing out of 0.7 (overlap on average 0.98). Without the half life 52% / 96%;
// from the box's size and place alone 26% / 95%; the features with the size added no better than the features.
class BoxLearner {
public:
    static constexpr float kPull = 10.0f;
    static constexpr float kHalfLife = 10.0f;

    // Fits to `samples`, for image `at` (its place among the recording's images, as the samples'). Any samples
    // before are forgotten.
    void fit(const BoxSamples& samples, int at, float pull = kPull, float halfLife = kHalfLife);
    // How many samples the last fit learned from.
    std::size_t learnedFrom() const { return learnedFrom_; }
    // The samples' weighted average nudge in the last fit: how the user boxed near the image fitted for.
    const Nudge& average() const { return average_; }

    // The nudge for a guess with `feature`; none if it learned from nothing or `feature` is not its size.
    Nudge nudge(std::span<const float> feature) const;

private:
    int size_ = 0;
    std::vector<float> weights_; // Per number of a feature then the bias: its weight towards x, y, width, height.
    std::size_t learnedFrom_ = 0;
    Nudge average_;
};

} // namespace observex
