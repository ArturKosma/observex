#pragma once

#include <filesystem>
#include <string>

namespace observex {

// Runs the trained model over a recording in the background and writes its guesses (<category>-guesses.txt) for
// the review. For now the model runs in the training tool's Python (tools/train/guess.py), as a separate process
// with no window; later observex will run it itself on the GPU (docs/training-loop.md, step 5).
class Finder {
public:
    enum class State { Running, Done, Failed };

    // Whether the finder should look at `folder`: a trained model for `category` exists, and the recording's
    // guesses are missing or older than the model (it was trained again since).
    static bool needed(const std::filesystem::path& folder, const char* category);

    // Starts it. Throws if it cannot start.
    Finder(const std::filesystem::path& folder, const char* category);
    ~Finder(); // Stops it if it is still running.

    Finder(const Finder&) = delete;
    Finder& operator=(const Finder&) = delete;

    // Reads what it reported so far, without waiting. Call often: this also keeps its output from backing up.
    State poll();

    // Images looked at so far, of `total()`. The total is 0 until the model has loaded.
    int done() const { return done_; }
    int total() const { return total_; }
    // Why it failed: its last words.
    const std::string& error() const { return error_; }

private:
    void read();

    void* job_ = nullptr; // Closing it ends the process and every process it started.
    void* process_ = nullptr;
    void* output_ = nullptr; // Our end of a pipe that has its output and error output.
    std::string pending_;    // Output that is not a whole line yet.
    std::string lastLine_;   // The last line that was not progress.
    State state_ = State::Running;
    int done_ = 0;
    int total_ = 0;
    std::string error_;
};

} // namespace observex
