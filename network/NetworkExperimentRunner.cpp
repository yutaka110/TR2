#include "NetworkExperimentRunner.h"

#include <algorithm>

namespace net {
namespace {

    NetworkCondition MakeCondition(
        bool enabled,
        double lossRate,
        uint32_t minDelayMs,
        uint32_t maxDelayMs,
        uint32_t burstLossLength,
        double duplicateRate = 0.0,
        double reorderRate = 0.0) {
        NetworkCondition condition{};
        condition.enabled = enabled;
        condition.lossRate = lossRate;
        condition.duplicateRate = duplicateRate;
        condition.reorderRate = reorderRate;
        condition.minDelayMs = minDelayMs;
        condition.maxDelayMs = maxDelayMs;
        condition.burstLossLength = burstLossLength;
        return condition;
    }

} // namespace

    NetworkExperimentRunner::NetworkExperimentRunner()
        : scenarios_(CreateDefaultScenarios()) {
    }

    bool NetworkExperimentRunner::Update(bool enabled, double deltaTimeSec) {
        if (scenarios_.empty()) {
            return false;
        }

        if (!enabled) {
            if (!active_) {
                return false;
            }

            Reset();
            return true;
        }

        if (!active_) {
            active_ = true;
            currentIndex_ = 0;
            elapsedSec_ = 0.0;
            return true;
        }

        elapsedSec_ += (std::max)(0.0, deltaTimeSec);

        const double durationSec =
            (std::max)(0.1, scenarios_[currentIndex_].durationSec);

        if (elapsedSec_ < durationSec) {
            return false;
        }

        elapsedSec_ = 0.0;
        currentIndex_ = (currentIndex_ + 1) % scenarios_.size();
        return true;
    }

    void NetworkExperimentRunner::Reset() {
        active_ = false;
        currentIndex_ = 0;
        elapsedSec_ = 0.0;
    }

    bool NetworkExperimentRunner::IsActive() const {
        return active_;
    }

    const NetworkExperimentScenario& NetworkExperimentRunner::CurrentScenario() const {
        return scenarios_[currentIndex_];
    }

    const NetworkCondition& NetworkExperimentRunner::CurrentCondition() const {
        return CurrentScenario().condition;
    }

    const std::string& NetworkExperimentRunner::CurrentScenarioName() const {
        return CurrentScenario().name;
    }

    double NetworkExperimentRunner::RemainingSec() const {
        const double durationSec =
            (std::max)(0.1, CurrentScenario().durationSec);
        return (std::max)(0.0, durationSec - elapsedSec_);
    }

    std::vector<NetworkExperimentScenario>
        NetworkExperimentRunner::CreateDefaultScenarios() {
        return {
            { "Baseline", MakeCondition(false, 0.0, 0, 0, 0), 30.0 },
            { "10% loss", MakeCondition(true, 0.10, 0, 0, 0), 30.0 },
            { "50ms jitter", MakeCondition(true, 0.0, 0, 50, 0), 30.0 },
            { "100ms delay", MakeCondition(true, 0.0, 100, 100, 0), 30.0 },
            { "Burst loss", MakeCondition(true, 0.03, 0, 0, 8), 30.0 },
        };
    }

} // namespace net
