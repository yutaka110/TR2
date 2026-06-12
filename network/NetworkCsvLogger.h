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
        void WriteEventHeader();
        void WriteEventSample(
            const NetworkStatsSnapshot& stats,
            double appTimeSec,
            bool startupWarmupActive,
            uint64_t outputQueueDroppedFramesDelta,
            uint64_t outputQueueDropEventsDelta,
            uint64_t outputQueueDropBurstEventsDelta,
            uint64_t receiveFreshnessDroppedFramesDelta,
            const std::string& freshnessDropClass,
            const std::string& freshnessDropEvidence
        );
        void WriteRunSummary();
        std::string ResolveScenarioName(const NetworkStatsSnapshot& stats) const;

        std::ofstream file_;
        std::ofstream eventFile_;
        std::string filePath_;
        std::string eventFilePath_;
        std::string scenarioName_ = "Auto";
        bool headerWritten_ = false;
        bool eventHeaderWritten_ = false;
        double startupWarmupSec_ = 5.0;
        uint64_t summaryTotalRows_ = 0;
        uint64_t summaryEventRows_ = 0;
        uint64_t summaryQoe2Rows_ = 0;
        uint64_t summaryQoe3Rows_ = 0;
        uint64_t summaryRecoveryDeadlineRawRows_ = 0;
        uint64_t summaryRecoveryDeadlineEffectiveRows_ = 0;
        uint64_t summaryRetransmitStaleEffectiveRows_ = 0;
        uint64_t summarySyncRiskRows_ = 0;
        uint64_t summaryHardSyncLossRows_ = 0;
        uint64_t summaryOutputDropRows_ = 0;
        uint64_t summaryFreshnessDropRows_ = 0;
        uint64_t summaryArrivalGapJitterRows_ = 0;
        uint64_t previousOutputQueueDroppedFrames_ = 0;
        uint64_t previousOutputQueueDropEvents_ = 0;
        uint64_t previousOutputQueueDropBurstEvents_ = 0;
        uint64_t previousReceiveFreshnessDroppedFrames_ = 0;
        uint64_t startupOutputQueueDroppedFrames_ = 0;
        uint64_t startupOutputQueueDropEvents_ = 0;
        uint64_t startupOutputQueueDropBurstEvents_ = 0;
        uint64_t steadyOutputQueueDroppedFrames_ = 0;
        uint64_t steadyOutputQueueDropEvents_ = 0;
        uint64_t steadyOutputQueueDropBurstEvents_ = 0;
    };

} // namespace net
