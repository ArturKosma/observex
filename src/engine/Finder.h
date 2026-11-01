#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace observex {

// Runs a trained model over a recording and writes its guesses (<category>-guesses.txt in the recording's folder)
// for the review: on each image, everything at least Recognizer::kMinSure sure. It uses the same Detector as live
// recognition (TensorRT on the GPU), on its own thread and GPU device, so it starts in under a second once the
// model is prepared for the GPU, and the UI never waits for it. Talks to the UI only through atomics.
class Finder {
public:
    enum class State : std::uint8_t { Running, Done, Failed };

    // Whether the finder should look at the recording in `folder`: the model in `modelFolder` (model.onnx) exists,
    // and the recording's guesses for `category` are missing or older than it (it was trained again since).
    static bool needed(const std::filesystem::path& folder, const char* category,
                       const std::filesystem::path& modelFolder);

    // Starts on `images` (file names in `folder`, UTF-8, in order) with the model in `modelFolder`. Returns at once.
    Finder(std::filesystem::path folder, std::vector<std::string> images, const char* category,
           std::filesystem::path modelFolder);
    // Stops it if it is still running; then nothing is written.
    ~Finder();

    Finder(const Finder&) = delete;
    Finder& operator=(const Finder&) = delete;

    State state() const { return state_.load(std::memory_order_acquire); }
    // How far preparing a new model for the GPU is, 0 to 1 (see Detector); stays 0 when a prepared one is loaded.
    float preparing() const { return progress_.load(std::memory_order_relaxed); }
    // Images looked at so far, of `total()`.
    int done() const { return done_.load(std::memory_order_relaxed); }
    int total() const { return static_cast<int>(images_.size()); }
    const std::string& error() const { return error_; } // Valid once state() is Failed.

private:
    void run(std::stop_token stop);

    std::filesystem::path folder_;
    std::vector<std::string> images_;
    std::string category_;
    std::filesystem::path modelFolder_;
    std::atomic<State> state_{State::Running};
    std::atomic<float> progress_{0.0f};
    std::atomic<int> done_{0};
    std::string error_;
    std::jthread thread_; // Last, so it stops before the members it uses are destroyed.
};

} // namespace observex
