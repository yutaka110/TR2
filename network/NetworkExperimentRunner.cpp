#include "NetworkExperimentRunner.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>

namespace net {
namespace {

    std::string ToLowerAscii(std::string value) {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        return value;
    }

    std::string ReadEnvString(const char* name) {
        char* buffer = nullptr;
        size_t size = 0;
        if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
            return {};
        }

        std::string value(buffer);
        std::free(buffer);
        return value;
    }

    double ReadEnvDouble(const char* name, double fallback) {
        const std::string value = ReadEnvString(name);
        if (value.empty()) {
            return fallback;
        }

        char* end = nullptr;
        const double parsed = std::strtod(value.c_str(), &end);
        return end != value.c_str() ? parsed : fallback;
    }

    bool ContainsLower(const std::string& haystack, const std::string& needle) {
        return ToLowerAscii(haystack).find(needle) != std::string::npos;
    }

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

    bool MatchesSlice(
        const NetworkExperimentScenario& scenario,
        const std::string& slice) {
        if (slice.empty() || slice == "all") {
            return true;
        }

        if (slice == "burst") {
            return scenario.networkScenarioName == "Burst loss";
        }

        if (slice == "adaptive_fec" || slice == "adaptive-fec") {
            return scenario.adaptiveFecEnabled;
        }

        if (slice == "burst_adaptive" || slice == "burst-adaptive") {
            return scenario.networkScenarioName == "Burst loss" &&
                scenario.adaptiveFecEnabled;
        }

        if (slice == "burst_fec" || slice == "burst-fec") {
            return scenario.networkScenarioName == "Burst loss" &&
                (scenario.fecEnabled || scenario.adaptiveFecEnabled);
        }

        return ContainsLower(scenario.name, slice) ||
            ContainsLower(scenario.networkScenarioName, slice);
    }

    void ApplyExperimentEnvironmentOverrides(
        std::vector<NetworkExperimentScenario>& scenarios) {
        const std::string slice =
            ToLowerAscii(ReadEnvString("TR2_NETWORK_EXPERIMENT_SLICE"));
        if (!slice.empty() && slice != "all") {
            std::vector<NetworkExperimentScenario> filtered;
            filtered.reserve(scenarios.size());

            for (const NetworkExperimentScenario& scenario : scenarios) {
                if (MatchesSlice(scenario, slice)) {
                    filtered.push_back(scenario);
                }
            }

            if (!filtered.empty()) {
                scenarios = std::move(filtered);
            }
        }

        const std::string scenarioMatch =
            ToLowerAscii(ReadEnvString("TR2_NETWORK_EXPERIMENT_SCENARIO"));
        if (!scenarioMatch.empty()) {
            std::vector<NetworkExperimentScenario> filtered;
            filtered.reserve(scenarios.size());

            for (const NetworkExperimentScenario& scenario : scenarios) {
                if (ContainsLower(scenario.name, scenarioMatch) ||
                    ContainsLower(scenario.networkScenarioName, scenarioMatch)) {
                    filtered.push_back(scenario);
                }
            }

            if (!filtered.empty()) {
                scenarios = std::move(filtered);
            }
        }

        const double durationOverride =
            ReadEnvDouble("TR2_NETWORK_EXPERIMENT_DURATION_SEC", 0.0);
        if (durationOverride > 0.0) {
            for (NetworkExperimentScenario& scenario : scenarios) {
                scenario.durationSec = (std::max)(0.5, durationOverride);
            }
        }
    }

} // namespace

    NetworkExperimentRunner::NetworkExperimentRunner()
        : scenarios_(CreateDefaultScenarios()) {
        ApplyExperimentEnvironmentOverrides(scenarios_);

        const double warmupOverride =
            ReadEnvDouble("TR2_NETWORK_EXPERIMENT_WARMUP_SEC", warmupSec_);
        warmupSec_ = (std::max)(0.0, warmupOverride);
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
