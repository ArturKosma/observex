#include "core/Features.h"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace observex;

namespace {

GuessFeatures someFeatures() {
    GuessFeatures features;
    features.size = 3;
    features.images["000001.jpg"] = {0.5f, -1.25f, 3.0f, 1e-7f, 0.0f, -65504.0f}; // Two guesses.
    features.images["000002.jpg"] = {};                                          // None.
    features.images["\xC3\xA9t\xC3\xA9.jpg"] = {1.0f, 2.0f, 3.0f};               // A UTF-8 name, one guess.
    return features;
}

ImageMarks guessesOf(const GuessFeatures& features) {
    ImageMarks guesses;
    for (const auto& [name, numbers] : features.images) {
        guesses[name].resize(numbers.size() / static_cast<std::size_t>(features.size));
    }
    return guesses;
}

} // namespace

TEST_CASE("the features file reads back what was written") {
    const GuessFeatures features = someFeatures();
    const std::string bytes = formatFeatures(features);
    CHECK(bytes.substr(0, 4) == "oxf1");
    const GuessFeatures read = parseFeatures(bytes);
    CHECK(read == features);
    REQUIRE(read.of("000001.jpg", 1).size() == 3u);
    CHECK(read.of("000001.jpg", 1)[0] == 1e-7f);
    CHECK(read.of("000001.jpg", 1)[2] == -65504.0f);
    CHECK(read.of("000001.jpg", 2).empty());
    CHECK(read.of("000002.jpg", 0).empty());
    CHECK(read.of("000003.jpg", 0).empty());
}

TEST_CASE("the features file is empty when it is not one or is cut short") {
    const std::string bytes = formatFeatures(someFeatures());
    CHECK(parseFeatures("").size == 0);
    CHECK(parseFeatures("oxf2" + bytes.substr(4)).size == 0);
    for (std::size_t length = 0; length < bytes.size(); length += 7) {
        CHECK(parseFeatures(bytes.substr(0, length)).images.empty());
    }
    CHECK(parseFeatures(bytes + "x").size == 0); // Something after it.
}

TEST_CASE("features match only guesses with the same images and as many guesses on each") {
    const GuessFeatures features = someFeatures();
    ImageMarks guesses = guessesOf(features);
    CHECK(featuresMatch(features, guesses));

    ImageMarks more = guesses;
    more["000001.jpg"].emplace_back(); // A guess more.
    CHECK_FALSE(featuresMatch(features, more));
    ImageMarks fewer = guesses;
    fewer.erase("000002.jpg"); // An image less.
    CHECK_FALSE(featuresMatch(features, fewer));
    ImageMarks other = fewer;
    other["000009.jpg"]; // Another image instead.
    CHECK_FALSE(featuresMatch(features, other));
    CHECK_FALSE(featuresMatch(GuessFeatures{}, ImageMarks{})); // No features at all.
}

TEST_CASE("fresh features are those written after their guesses and the model, and matching them") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
                          ("observex-tests-fresh-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path recording = root / "2026-11-08_10-00-00";
    const fs::path model = root / "model";
    fs::create_directories(recording);
    fs::create_directories(model);
    const GuessFeatures features = someFeatures();
    const ImageMarks guesses = guessesOf(features);
    std::ofstream(recording / "character-guesses.txt", std::ios::binary) << formatMarks(guesses);
    std::ofstream(recording / "character-features.bin", std::ios::binary) << formatFeatures(features);
    std::ofstream(model / "model.onnx", std::ios::binary) << "onnx";
    const fs::file_time_type now = fs::last_write_time(recording / "character-features.bin");
    const auto setTimes = [&](int modelHours, int guessesHours) { // Hours before the features were written.
        fs::last_write_time(model / "model.onnx", now - std::chrono::hours(modelHours));
        fs::last_write_time(recording / "character-guesses.txt", now - std::chrono::hours(guessesHours));
    };

    setTimes(2, 1);
    CHECK(freshFeatures(recording, "character", model, guesses) == features);
    setTimes(0, 0); // Written in the same instant counts as after.
    CHECK(freshFeatures(recording, "character", model, guesses) == features);
    setTimes(-1, 1); // A newer model.
    CHECK(freshFeatures(recording, "character", model, guesses).size == 0);
    setTimes(2, -1); // Newer guesses: another finder run.
    CHECK(freshFeatures(recording, "character", model, guesses).size == 0);
    setTimes(2, 1);
    ImageMarks more = guesses;
    more["000001.jpg"].emplace_back();
    CHECK(freshFeatures(recording, "character", model, more).size == 0); // Not these guesses.
    CHECK(freshFeatures(recording, "monster", model, guesses).size == 0); // No such files.
    fs::remove(model / "model.onnx");
    CHECK(freshFeatures(recording, "character", model, guesses).size == 0); // No model.
    fs::remove_all(root);
}
