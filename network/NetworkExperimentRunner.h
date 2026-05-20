#pragma once

#include "NetworkConditionSimulator.h"

#include <cstddef>
#include <string>
#include <vector>

namespace net {

    struct NetworkExperimentScenario {
        std::string name;
        NetworkCondition condition{};
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
        const std::string& CurrentScenarioName() const;
        double RemainingSec() const;

    private:
        static std::vector<NetworkExperimentScenario> CreateDefaultScenarios();

        std::vector<NetworkExperimentScenario> scenarios_;
        size_t currentIndex_ = 0;
        double elapsedSec_ = 0.0;
        bool active_ = false;
    };

} // namespace net
