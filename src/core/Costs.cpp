#include "core/Costs.h"

#include <algorithm>

namespace observex {

void nameCost(Cost& cost, std::string_view name) {
    cost.name.fill('\0');
    std::copy_n(name.begin(), std::min(name.size(), cost.name.size() - 1), cost.name.begin());
}

float costTotalMs(std::span<const CostPart> parts) {
    float total = 0.0f;
    for (const CostPart& part : parts) {
        total += part.run.eachMs;
    }
    return total;
}

CostMeter::CostMeter(std::initializer_list<const char*> steps) {
    for (const char* step : steps) {
        if (names_.stepCount < CostPart::kMaxSteps) {
            nameCost(names_.steps[static_cast<std::size_t>(names_.stepCount++)], step);
        }
    }
}

void CostMeter::lap(int step) {
    const Clock::time_point now = Clock::now();
    if (step >= 0 && step < names_.stepCount) {
        steps_[static_cast<std::size_t>(step)].add(msBetween(mark_, now));
    }
    mark_ = now;
}

void CostMeter::end() {
    const Clock::time_point now = Clock::now();
    run_.add(msBetween(start_, now));
    mark_ = now;
}

CostPart CostMeter::take(double seconds) {
    CostPart part = names_;
    const auto fill = [seconds](Cost& cost, StatWindow& window) {
        const StatWindow::Summary summary = window.take();
        cost.eachMs = summary.average;
        cost.slowestMs = summary.max;
        cost.perSecond = seconds > 0.0 ? static_cast<float>(summary.count / seconds) : 0.0f;
    };
    fill(part.run, run_);
    for (int i = 0; i < part.stepCount; ++i) {
        fill(part.steps[static_cast<std::size_t>(i)], steps_[static_cast<std::size_t>(i)]);
    }
    return part;
}

} // namespace observex
