#pragma once

#include <atomic>
#include <cstdint>

namespace observex {

// One yes-or-no question at a time between two threads, lock-free: one asks and takes the answer, the other answers.
// Recording "only unsure" uses it: the pipeline asks whether the newest image is worth recording, and the recogniser
// answers after its next look (Recognizer::kMirrorDisagrees).
class Question {
public:
    enum class State : std::uint8_t {
        None,  // Nothing asked, or the answer was taken.
        Asked, // Waiting for the answer.
        Yes,
        No,
    };

    // Asker: asks, dropping any answer not taken yet.
    void ask() { state_.store(State::Asked, std::memory_order_relaxed); }
    // Asker: forgets the question; an answer to it that comes later is ignored.
    void drop() { state_.store(State::None, std::memory_order_relaxed); }
    // Asker: where the question is. Yes or No is the answer, taken: the state goes back to None.
    State take() {
        const State state = state_.load(std::memory_order_relaxed);
        if (state == State::Yes || state == State::No) {
            state_.store(State::None, std::memory_order_relaxed); // Only the asker changes an answer.
        }
        return state;
    }

    // Answerer: whether a question waits for an answer.
    bool asked() const { return state_.load(std::memory_order_relaxed) == State::Asked; }
    // Answerer: answers the question; ignored if it was dropped meanwhile.
    void answer(bool yes) {
        State asked = State::Asked;
        state_.compare_exchange_strong(asked, yes ? State::Yes : State::No, std::memory_order_relaxed);
    }

private:
    std::atomic<State> state_{State::None};
};

} // namespace observex
