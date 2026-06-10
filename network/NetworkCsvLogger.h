#pragma once

#include "NetworkStats.h"

#include <cstdint>
#include <fstream>
#include <string>

namespace net {

    class NetworkCsvLogger {
    public:
        bool Start(const std::string& directory);
        void Stop();

        void SetScenarioName(std::string name);
        void WriteSample(const NetworkStatsSnapshot& stats, double appTimeSec);

        bool IsRunning() const;
        const std::string& FilePath() const;

    private:
        void WriteHeader();
        std::string ResolveScenarioName(const NetworkStatsSnapshot& stats) const;

        std::ofstream file_;
        std::string filePath_;
        std::string scenarioName_ = "Auto";
        bool headerWritten_ = false;
        double startupWarmupSec_ = 5.0;
        uint64_t previousOutputQueueDroppedFrames_ = 0;
        uint64_t previousOutputQueueDropEvents_ = 0;
        uint64_t previousOutputQueueDropBurstEvents_ = 0;
        uint64_t startupOutputQueueDroppedFrames_ = 0;
        uint64_t startupOutputQueueDropEvents_ = 0;
        uint64_t startupOutputQueueDropBurstEvents_ = 0;
        uint64_t steadyOutputQueueDroppedFrames_ = 0;
        uint64_t steadyOutputQueueDropEvents_ = 0;
        uint64_t steadyOutputQueueDropBurstEvents_ = 0;
    };

} // namespace net
