#include "engine/KindUpdater.h"

#include "core/Features.h"
#include "core/KindLearner.h"
#include "core/ModelInfo.h"
#include "engine/Finder.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace observex {

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string toUtf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// A recording with reviewed images.
struct Reviewed {
    fs::path folder;
    ImageMarks answers;          // Its reviewed images only: those with every kind checked.
    fs::file_time_type answered; // When its answers file was last written.
};

// The recordings in `root` with reviewed images for `category`, in name order.
std::vector<Reviewed> reviewedRecordings(const fs::path& root, const std::string& category, KindIds kinds) {
    std::vector<fs::path> folders;
    std::error_code error;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        std::error_code notFolder;
        if (it->is_directory(notFolder)) {
            folders.push_back(it->path());
        }
    }
    std::sort(folders.begin(), folders.end());
    std::vector<Reviewed> reviewed;
    for (fs::path& folder : folders) {
        const fs::path file = folder / (category + "-answers.txt");
        std::error_code missing;
        const fs::file_time_type answered = fs::last_write_time(file, missing);
        if (missing) {
            continue;
        }
        ImageMarks answers = parseMarks(readFile(file), kinds);
        // Only images the user is done with: kinds checked, and no guess added since waiting for its answer.
        std::erase_if(answers, [](const auto& image) { return !fullyAnswered(image.second); });
        if (!answers.empty()) {
            reviewed.push_back({std::move(folder), std::move(answers), answered});
        }
    }
    return reviewed;
}

// A recording's guesses and their features, if both are from the model in `modelFolder`; else no features.
struct Guessed {
    ImageMarks guesses;
    GuessFeatures features;
};

Guessed readGuesses(const fs::path& folder, const std::string& category, KindIds kinds,
                    const fs::path& modelFolder) {
    Guessed guessed;
    if (Finder::needed(folder, category.c_str(), modelFolder)) {
        return guessed; // Missing, or from an older model.
    }
    guessed.guesses = parseMarks(readFile(folder / (category + "-guesses.txt")), kinds);
    guessed.features = freshFeatures(folder, category, modelFolder, guessed.guesses);
    return guessed;
}

// Writes `text` to `file` through a temporary file, so nothing ever reads half of it.
bool save(const fs::path& file, const std::string& text) {
    fs::path temporary = file;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            return false;
        }
    }
    std::error_code error;
    fs::rename(temporary, file, error);
    return !error;
}

} // namespace

std::vector<fs::path> KindUpdater::stale(const fs::path& recordings, const std::string& category, KindIds kinds,
                                         const fs::path& modelFolder) {
    std::vector<fs::path> stale;
    for (Reviewed& recording : reviewedRecordings(recordings, category, kinds)) {
        if (readGuesses(recording.folder, category, kinds, modelFolder).features.size == 0) {
            stale.push_back(std::move(recording.folder));
        }
    }
    return stale;
}

KindUpdater::KindUpdater(fs::path recordings, std::string category, std::vector<std::string> kinds,
                         fs::path modelFolder)
    : recordings_(std::move(recordings)), category_(std::move(category)), kinds_(std::move(kinds)),
      modelFolder_(std::move(modelFolder)) {
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

KindUpdater::~KindUpdater() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void KindUpdater::run(std::stop_token stop) {
    // Below normal: capture, live recognition and the UI come first.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"observex kind update");
    const auto finish = [this](State state) { state_.store(state, std::memory_order_release); };
    const auto fail = [&](std::string why) {
        error_ = std::move(why);
        finish(State::Failed);
    };

    try {
        const std::vector<std::string_view> kinds(kinds_.begin(), kinds_.end());
        // The model's own classifier, for the category's kinds in their order, or its first ones (as the review
        // checks: answers of a kind the model does not know yet are left out).
        std::error_code error;
        const fs::file_time_type trained = fs::last_write_time(modelFolder_ / L"model.onnx", error);
        if (error) {
            return fail("No trained model (model.onnx) in " + toUtf8(modelFolder_));
        }
        const std::string info = readFile(modelFolder_ / L"model.json");
        const std::vector<std::string> modelKinds = parseModelKinds(info);
        KindHead head = parseKindHead(info);
        if (!kindsFit(modelKinds, kinds) || head.kinds() != static_cast<int>(modelKinds.size())) {
            return fail("model.json has no kind classifier for this category's kinds (export the model again)");
        }

        // Only when some answers are newer than the model and than the update there is.
        std::vector<Reviewed> reviewed = reviewedRecordings(recordings_, category_, kinds);
        fs::file_time_type newest = fs::file_time_type::min();
        for (const Reviewed& recording : reviewed) {
            newest = std::max(newest, recording.answered);
        }
        const fs::file_time_type updated = fs::last_write_time(modelFolder_ / kKindUpdateFile, error);
        if (newest <= trained || (!error && loadKindUpdate(modelFolder_).head.kinds() > 0 && updated >= newest)) {
            return finish(State::NothingNew);
        }

        // Every guess on a reviewed image, with its answer: the yes answer's kind it matches, else no.
        std::vector<float> features;
        std::vector<int> answers;
        for (Reviewed& recording : reviewed) {
            if (stop.stop_requested()) {
                return;
            }
            const Guessed guessed = readGuesses(recording.folder, category_, kinds, modelFolder_);
            if (guessed.features.size == 0) {
                stale_.push_back(std::move(recording.folder));
                continue;
            }
            if (guessed.features.size != head.features()) {
                return fail("The model's features do not fit its kind classifier (export the model again)");
            }
            const std::size_t added = addAnsweredGuesses(guessed.guesses, guessed.features, recording.answers,
                                                         head.kinds(), features, answers);
            recordingsLearned_ += added > 0 ? 1 : 0;
        }
        if (!stale_.empty()) {
            return finish(State::NeedsGuesses);
        }
        if (answers.empty()) {
            return finish(State::NothingNew);
        }

        // From the model's own classifier, never the last update's.
        const auto start = std::chrono::steady_clock::now();
        KindLearner learner(std::move(head));
        learner.fit(features, answers, KindLearner::kUpdateSteps, KindLearner::kUpdatePull);
        fitMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (stop.stop_requested()) {
            return;
        }
        KindUpdate update;
        update.head = learner.learned();
        update.answers = answers.size();
        update.recordings = recordingsLearned_;
        const auto finite = [](float value) { return std::isfinite(value); };
        if (!std::all_of(update.head.weights.begin(), update.head.weights.end(), finite) ||
            !std::all_of(update.head.biases.begin(), update.head.biases.end(), finite)) {
            return fail("The kind update did not fit (numbers out of range)");
        }
        if (!save(modelFolder_ / kKindUpdateFile, formatKindUpdate(update))) {
            return fail("Could not save " + toUtf8(modelFolder_ / kKindUpdateFile));
        }
        answers_ = update.answers;
        finish(State::Done);
    } catch (const std::exception& e) {
        if (!stop.stop_requested()) {
            fail(e.what());
        }
    }
}

} // namespace observex
