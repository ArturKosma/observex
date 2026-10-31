#pragma once

#include <algorithm>
#include <cstdint>

namespace observex {

// Collects samples (e.g. milliseconds per frame) and summarises them on request, then starts over.
// Cheap enough to call on every frame: no allocation, no locking.
class StatWindow {
public:
    struct Summary {
        std::uint32_t count = 0;
        float average = 0.0f;
        float max = 0.0f;
    };

    void add(float value) {
        ++count_;
        sum_ += value;
        max_ = std::max(max_, value);
    }

    // Returns the summary of everything added since the last take(), and clears it.
    Summary take() {
        Summary summary;
        summary.count = count_;
        summary.average = count_ > 0 ? static_cast<float>(sum_ / count_) : 0.0f;
        summary.max = max_;
        *this = StatWindow{};
        return summary;
    }

private:
    std::uint32_t count_ = 0;
    double sum_ = 0.0;
    float max_ = 0.0f;
};

} // namespace observex
