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

    NetworkExperimentScenario MakeScenario(
        const char* networkName,
        const NetworkCondition& condition,
        AdaptiveControlMode mode,
        CongestionControlMode congestionMode,
        double durationSec,
        bool fecEnabled = true,
        uint16_t fecGroupChunkCount = 4,
        bool adaptiveFecEnabled = false
    ) {
        NetworkExperimentScenario scenario{};
        scenario.networkScenarioName = networkName;
        scenario.condition = condition;
        scenario.adaptiveControlMode = mode;
        scenario.congestionControlMode = congestionMode;
        scenario.fecEnabled = fecEnabled;
        scenario.adaptiveFecEnabled = adaptiveFecEnabled;
        scenario.fecGroupChunkCount = static_cast<uint16_t>(
            (std::max)(2u, (std::min)(32u,
                static_cast<unsigned>(fecGroupChunkCount)))
        );
        scenario.durationSec = durationSec;
        scenario.name =
            scenario.networkScenarioName + " / " + ToString(mode);
        if (mode == AdaptiveControlMode::QoeDeadlineAdaptive) {
            scenario.name += " / ";
            scenario.name += ToString(congestionMode);
        }
        scenario.name += " / ";
        if (!scenario.fecEnabled && !scenario.adaptiveFecEnabled) {
            scenario.name += "FEC off";
        }
        else if (scenario.adaptiveFecEnabled) {
            scenario.name += "Adaptive FEC";
        }
        else {
            scenario.name += "FEC g";
            scenario.name += std::to_string(scenario.fecGroupChunkCount);
        }
        return scenario;
    }

    NetworkExperimentScenario MakeScenario(
        const char* networkName,
        const NetworkCondition& condition,
        AdaptiveControlMode mode,
        double durationSec
    ) {
        const CongestionControlMode congestionMode =
            mode == AdaptiveControlMode::LossReactive
            ? CongestionControlMode::LossBased
            : CongestionControlMode::Hybrid;
        return MakeScenario(
            networkName,
            condition,
            mode,
            congestionMode,
            durationSec
        );
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
                if (completed_) {
                    Reset();
                    return true;
                }
                return false;
            }

            Reset();
            return true;
        }

        if (completed_) {
            return false;
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

        if (currentIndex_ + 1 >= scenarios_.size()) {
            if (stopAfterOnePass_) {
                active_ = false;
                completed_ = true;
                elapsedSec_ = 0.0;
                return true;
            }

            currentIndex_ = 0;
        }
        else {
            currentIndex_++;
        }

        elapsedSec_ = 0.0;
        return true;
    }

    void NetworkExperimentRunner::Reset() {
        active_ = false;
        currentIndex_ = 0;
        elapsedSec_ = 0.0;
        completed_ = false;
    }

    void NetworkExperimentRunner::SetStopAfterOnePass(bool enabled) {
        stopAfterOnePass_ = enabled;
    }

    bool NetworkExperimentRunner::IsActive() const {
        return active_;
    }

    bool NetworkExperimentRunner::IsCompleted() const {
        return completed_;
    }

    const NetworkExperimentScenario& NetworkExperimentRunner::CurrentScenario() const {
        return scenarios_[currentIndex_];
    }

    const NetworkCondition& NetworkExperimentRunner::CurrentCondition() const {
        return CurrentScenario().condition;
    }

    AdaptiveControlMode NetworkExperimentRunner::CurrentAdaptiveControlMode() const {
        return CurrentScenario().adaptiveControlMode;
    }

    CongestionControlMode NetworkExperimentRunner::CurrentCongestionControlMode() const {
        return CurrentScenario().congestionControlMode;
    }

    bool NetworkExperimentRunner::CurrentFecEnabled() const {
        return CurrentScenario().fecEnabled;
    }

    bool NetworkExperimentRunner::CurrentAdaptiveFecEnabled() const {
        return CurrentScenario().adaptiveFecEnabled;
    }

    uint16_t NetworkExperimentRunner::CurrentFecGroupChunkCount() const {
        return CurrentScenario().fecGroupChunkCount;
    }

    const std::string& NetworkExperimentRunner::CurrentScenarioName() const {
        return CurrentScenario().name;
    }

    const std::string& NetworkExperimentRunner::CurrentNetworkScenarioName() const {
        return CurrentScenario().networkScenarioName;
    }

    double NetworkExperimentRunner::ElapsedSec() const {
        return elapsedSec_;
    }

    double NetworkExperimentRunner::WarmupSec() const {
        return warmupSec_;
    }

    double NetworkExperimentRunner::RemainingSec() const {
        const double durationSec =
            (std::max)(0.1, CurrentScenario().durationSec);
        return (std::max)(0.0, durationSec - elapsedSec_);
    }

    size_t NetworkExperimentRunner::CurrentIndex() const {
        return currentIndex_;
    }

    size_t NetworkExperimentRunner::ScenarioCount() const {
        return scenarios_.size();
    }

    const std::vector<NetworkExperimentScenario>&
        NetworkExperimentRunner::Scenarios() const {
        return scenarios_;
    }

    std::vector<NetworkExperimentScenario>
        NetworkExperimentRunner::CreateDefaultScenarios() {
        const NetworkCondition baseline =
            MakeCondition(false, 0.0, 0, 0, 0);
        const NetworkCondition loss10 =
            MakeCondition(true, 0.10, 0, 0, 0);
        const NetworkCondition jitter50 =
            MakeCondition(true, 0.0, 0, 50, 0);
        const NetworkCondition burstLoss =
            MakeCondition(true, 0.03, 0, 0, 8);
        const double durationSec = 20.0;

        return {
            MakeScenario("Baseline", baseline,
                AdaptiveControlMode::FixedQuality, durationSec),
            MakeScenario("Baseline", baseline,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::Hybrid, durationSec),

            MakeScenario("10% loss", loss10,
                AdaptiveControlMode::FixedQuality, durationSec),
            MakeScenario("10% loss", loss10,
                AdaptiveControlMode::LossReactive, durationSec),
            MakeScenario("10% loss", loss10,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::LossBased, durationSec),
            MakeScenario("10% loss", loss10,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::DelayBased, durationSec),
            MakeScenario("10% loss", loss10,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::Hybrid, durationSec),

            MakeScenario("50ms jitter", jitter50,
                AdaptiveControlMode::FixedQuality, durationSec),
            MakeScenario("50ms jitter", jitter50,
                AdaptiveControlMode::LossReactive, durationSec),
            MakeScenario("50ms jitter", jitter50,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::LossBased, durationSec),
            MakeScenario("50ms jitter", jitter50,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::DelayBased, durationSec),
            MakeScenario("50ms jitter", jitter50,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::Hybrid, durationSec),

            MakeScenario("Burst loss", burstLoss,
                AdaptiveControlMode::FixedQuality, durationSec),
            MakeScenario("Burst loss", burstLoss,
                AdaptiveControlMode::LossReactive, durationSec),
            MakeScenario("Burst loss", burstLoss,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::LossBased, durationSec),
            MakeScenario("Burst loss", burstLoss,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::DelayBased, durationSec),
            MakeScenario("Burst loss", burstLoss,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::Hybrid, durationSec),
            MakeScenario("Burst loss", burstLoss,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::Hybrid, durationSec,
                false, 4, false),
            MakeScenario("Burst loss", burstLoss,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::Hybrid, durationSec,
                true, 8, false),
            MakeScenario("Burst loss", burstLoss,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::Hybrid, durationSec,
                true, 2, false),
            MakeScenario("Burst loss", burstLoss,
                AdaptiveControlMode::QoeDeadlineAdaptive,
                CongestionControlMode::Hybrid, durationSec,
                true, 4, true),
        };
    }

} // namespace net
