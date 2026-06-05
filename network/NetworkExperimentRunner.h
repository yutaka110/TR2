#pragma once

#include "AdaptiveStreamingController.h"
#include "NetworkConditionSimulator.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace net {

    struct NetworkExperimentScenario {
        std::string name;
        std::string networkScenarioName;
        NetworkCondition condition{};
        AdaptiveControlMode adaptiveControlMode =
            AdaptiveControlMode::QoeDeadlineAdaptive;
        CongestionControlMode congestionControlMode =
            CongestionControlMode::Hybrid;
        bool fecEnabled = true;
        bool adaptiveFecEnabled = false;
        uint16_t fecGroupChunkCount = 4;
        double durationSec = 30.0;
    };

    class NetworkExperimentRunner {
    public:
        NetworkExperimentRunner();

        bool Update(bool enabled, double deltaTimeSec);
        void Reset();
        void SetStopAfterOnePass(bool enabled);

        bool IsActive() const;
        bool IsCompleted() const;
        const NetworkExperimentScenario& CurrentScenario() const;
        const NetworkCondition& CurrentCondition() const;
        AdaptiveControlMode CurrentAdaptiveControlMode() const;
        CongestionControlMode CurrentCongestionControlMode() const;
        bool CurrentFecEnabled() const;
        bool CurrentAdaptiveFecEnabled() const;
        uint16_t CurrentFecGroupChunkCount() const;
        const std::string& CurrentScenarioName() const;
        const std::string& CurrentNetworkScenarioName() const;
        double ElapsedSec() const;
        double WarmupSec() const;
        double RemainingSec() const;
        size_t CurrentIndex() const;
        size_t ScenarioCount() const;
        const std::vector<NetworkExperimentScenario>& Scenarios() const;

    private:
        static std::vector<NetworkExperimentScenario> CreateDefaultScenarios();

        std::vector<NetworkExperimentScenario> scenarios_;
        size_t currentIndex_ = 0;
        double elapsedSec_ = 0.0;
        double warmupSec_ = 5.0;
        bool active_ = false;
        bool completed_ = false;
        bool stopAfterOnePass_ = false;
    };

} // namespace net
