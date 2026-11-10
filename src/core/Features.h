#pragma once

#include "core/Marks.h"

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace observex {

// What the model saw of each of its guesses on a recording: per guess, the numbers its kind classifier reads
// (Detector::features; 256 for RF-DETR Medium), image by image, in the order of the guesses file. The finder writes
// them next to the guesses (<category>-features.bin); the review re-fits the kinds to the user's answers with them
// (core/KindLearner).
struct GuessFeatures {
    int size = 0; // Numbers per guess; 0: none.
    std::map<std::string, std::vector<float>> images; // By image file name: `size` numbers per guess, guess by guess.

    // The numbers of guess `index` on `image`; empty if there is no such guess.
    std::span<const float> of(const std::string& image, std::size_t index) const;
    bool operator==(const GuessFeatures&) const = default;
};

// The features file: "oxf1", then little-endian 32-bit counts: numbers per guess and images; then per image (in name
// order) the length of its file name in bytes, the name (UTF-8), its guesses, and their numbers as 32-bit floats.
std::string formatFeatures(const GuessFeatures& features);
// Reads what formatFeatures wrote; empty (size 0) for anything else, also a file cut short.
GuessFeatures parseFeatures(std::string_view bytes);

// Whether `features` are those of `guesses`: the same images, with as many guesses on each. Features that are not
// (written by another finder run) belong to other guesses and must not be used.
bool featuresMatch(const GuessFeatures& features, const ImageMarks& guesses);

// The features of a recording's guesses (`guesses`, as read from <category>-guesses.txt in `folder`), from its
// <category>-features.bin, if they are from the same finder run and from the model in `modelFolder`: written after
// the guesses file and after the model's model.onnx, and matching the guesses (featuresMatch). Else empty: features of
// other guesses, or of an older model, must not be used. The review and the kind update both read them so.
GuessFeatures freshFeatures(const std::filesystem::path& folder, std::string_view category,
                            const std::filesystem::path& modelFolder, const ImageMarks& guesses);

} // namespace observex
