#pragma once

#include "AdaptiveStreamingController.h"
#include "NetworkConditionSimulator.h"

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
        double durationSec = 30.0;
    };

    class NetworkExperimentRunner {
    public:
        NetworkExperimentRunner();

        bool Update(bool enabled, double deltaTimeSec);
        void Reset();

        bool IsActive() const;
        const NetworkExperimentScenario& CurrentScenario() const;
        const NetworkCondition& CurrentCondition() const;
        AdaptiveControlMode CurrentAdaptiveControlMode() const;
        CongestionControlMode CurrentCongestionControlMode() const;
        const std::string& CurrentScenarioName() const;
        const std::string& CurrentNetworkScenarioName() const;
        double RemainingSec() const;
        size_t CurrentIndex() const;
        size_t ScenarioCount() const;

    private:
        static std::vector<NetworkExperimentScenario> CreateDefaultScenarios();

        std::vector<NetworkExperimentScenario> scenarios_;
        size_t currentIndex_ = 0;
        double elapsedSec_ = 0.0;
        bool active_ = false;
    };

} // namespace net
