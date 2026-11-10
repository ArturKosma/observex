#pragma once

#include "core/Marks.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace observex {

// The kind update (the user asked, 2026-11-08, whether the review's learning could make the whole model better
// between trainings): after a review, re-fits the model's kind classifier to every reviewed answer of every
// recording at once (core/KindLearner at its update pace) and saves it next to the model as kinds-update.json
// (core/ModelInfo's KindUpdate). Live recognition and the finder then pick kinds with it, keeping how sure the model
// is of a character (core/KindLearner's pickKinds), and the review learns on from it, until the next training.
// It learns from each reviewed image's guesses, with the features the finder kept of them: a guess matched to a yes
// answer is that kind, any other is no (core/Marks' matchGuesses). Always from model.json's own classifier and all
// the answers, never on top of the last update: in Python, fitting only the newest answers forgot older places (the
// right kind there 97% -> 77%). Right after a training it gained nothing there and pre-filled slightly worse in a
// replayed review (26 -> 32 wrong), so it is made only when some answers are newer than the model.
// CPU only, on its own thread (below normal priority); talks through atomics.
class KindUpdater {
public:
    enum class State : std::uint8_t {
        Running,
        Done,         // Wrote a new kinds-update.json: answers(), recordings(), fitMs().
        NothingNew,   // No answers newer than the model, or than the kind update already there.
        NeedsGuesses, // Some reviewed recordings' guesses or features are not from the current model: stale().
        Failed,       // See error().
    };

    // The reviewed recordings in `recordings` (its folders whose <category>-answers.txt holds an image with every kind
    // checked) whose guesses or features are not from the model in `modelFolder` (Finder::needed, core/Features'
    // freshFeatures), in name order: the finder must look at them again before the update can learn from them.
    static std::vector<std::filesystem::path> stale(const std::filesystem::path& recordings, const std::string& category,
                                                    KindIds kinds, const std::filesystem::path& modelFolder);

    // Starts on the recordings in `recordings` for `category` (e.g. "character"; `kinds` are its kinds' ids in order,
    // which must be model.json's), with the model in `modelFolder`. Returns at once.
    KindUpdater(std::filesystem::path recordings, std::string category, std::vector<std::string> kinds,
                std::filesystem::path modelFolder);
    // Stops it if it is still running; then nothing is written.
    ~KindUpdater();

    KindUpdater(const KindUpdater&) = delete;
    KindUpdater& operator=(const KindUpdater&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    // Once Done: how many answers it learned from, in how many recordings, and how long the fit took.
    std::size_t answers() const { return answers_; }
    std::size_t recordings() const { return recordingsLearned_; }
    double fitMs() const { return fitMs_; }
    // Once NeedsGuesses: the recordings whose guesses are not from the current model (see stale()).
    const std::vector<std::filesystem::path>& staleRecordings() const { return stale_; }
    const std::string& error() const { return error_; } // Once Failed.

private:
    void run(std::stop_token stop);

    std::filesystem::path recordings_;
    std::string category_;
    std::vector<std::string> kinds_;
    std::filesystem::path modelFolder_;
    std::atomic<State> state_{State::Running};
    std::size_t answers_ = 0;
    std::size_t recordingsLearned_ = 0;
    double fitMs_ = 0.0;
    std::vector<std::filesystem::path> stale_;
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
