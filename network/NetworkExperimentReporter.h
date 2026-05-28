#pragma once

#include "NetworkStats.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace net {

    class NetworkExperimentReporter {
    public:
        bool Start(const std::string& directory);
        void Stop();

        void RecordSample(
            const std::string& scenarioName,
            const NetworkStatsSnapshot& stats,
            double appTimeSec
        );

        bool IsRunning() const;
        const std::string& CsvFilePath() const;
        const std::string& TextFilePath() const;
        const std::string& MarkdownFilePath() const;
        const std::string& BeforeAfterFilePath() const;

    private:
        struct TimeSeriesSample {
            double relativeTimeSec = 0.0;
            double displayFps = 0.0;
            double currentLatencyMs = 0.0;
            double rollingP95LatencyMs = 0.0;
            double adaptiveQoeScore = 0.0;
            std::string adaptiveDegradationCause;
            double adaptiveCauseScore = 0.0;
            int targetJpegQuality = 0;
            int targetFps = 0;
            int targetBitrateKbps = 0;
            int bandwidthCeilingKbps = 0;
            uint32_t estimatedBandwidthBps = 0;
            uint32_t deliveryRateBps = 0;
            double bandwidthQueueDelayMs = 0.0;
            double bandwidthLossTrend = 0.0;
            double bandwidthJitterTrendMs = 0.0;
            uint64_t deadlineNackSentFrames = 0;
            uint64_t deadlineNackRecoveredFrames = 0;
            uint64_t deadlineNackExpiredDroppedFrames = 0;
            uint64_t ackRetransmittedChunks = 0;
            double packetLossRate = 0.0;
            double currentJitterMs = 0.0;
        };

        struct ScenarioAccumulator {
            std::string name;
            double startTimeSec = 0.0;
            double endTimeSec = 0.0;
            uint32_t sampleCount = 0;

            double latencySumMs = 0.0;
            double maxLatencyMs = 0.0;
            std::vector<double> latencySamplesMs;

            double displayFpsSum = 0.0;
            double minDisplayFps = 0.0;
            double receiveFpsSum = 0.0;
            double decodeFpsSum = 0.0;

            double packetLossRateSum = 0.0;
            double maxPacketLossRate = 0.0;

            double jitterSumMs = 0.0;
            double maxJitterMs = 0.0;

            int minTargetFps = 0;
            int minTargetJpegQuality = 0;
            int minTargetBitrateKbps = 0;
            std::string networkRuntimeMode;
            std::string adaptiveControlMode;
            std::string adaptiveCongestionControlMode;
            std::array<uint32_t, 7> adaptiveCauseSamples{};
            std::vector<TimeSeriesSample> timeSeriesSamples;

            NetworkStatsSnapshot lastStats{};
        };

        struct ScenarioSummary {
            std::string name;
            uint32_t sampleCount = 0;
            double startTimeSec = 0.0;
            double endTimeSec = 0.0;
            double durationSec = 0.0;

            double avgLatencyMs = 0.0;
            double p95LatencyMs = 0.0;
            double maxLatencyMs = 0.0;

            double avgDisplayFps = 0.0;
            double minDisplayFps = 0.0;
            double avgReceiveFps = 0.0;
            double avgDecodeFps = 0.0;

            double avgPacketLossRate = 0.0;
            double maxPacketLossRate = 0.0;
            double avgJitterMs = 0.0;
            double maxJitterMs = 0.0;

            uint64_t completedFrames = 0;
            uint64_t displayedFrames = 0;
            uint64_t droppedFrames = 0;
            uint64_t deadlineDroppedFrames = 0;
            uint64_t outputQueueDroppedFrames = 0;
            uint64_t outputQueueDropEvents = 0;
            uint64_t outputQueueDropBurstEvents = 0;
            double maxOutputQueueDropOldestAgeMs = 0.0;
            std::string lastOutputQueueDropReason;
            uint64_t ackCount = 0;
            uint64_t ackRetransmittedFrames = 0;
            uint64_t deadlineNackSentFrames = 0;
            uint64_t deadlineNackRecoveredFrames = 0;
            uint64_t deadlineNackMissingChunks = 0;
            uint64_t deadlineNackExpiredDroppedFrames = 0;
            uint64_t deadlineNackExpiredAfterNackFrames = 0;
            uint64_t deadlineNackExpiredMissingChunks = 0;
            uint64_t simDroppedPackets = 0;

            int minTargetFps = 0;
            int minTargetJpegQuality = 0;
            int minTargetBitrateKbps = 0;
            std::string networkRuntimeMode;
            std::string adaptiveControlMode;
            std::string adaptiveCongestionControlMode;
            std::string dominantAdaptiveDegradationCause;
            std::vector<TimeSeriesSample> timeSeriesSamples;

            std::string verdict;
            std::string notes;
        };

        void ResetCurrent();
        void FinalizeCurrent();
        void WriteHeader();
        void WriteSummary(const ScenarioSummary& summary);
        void WriteMarkdownReport() const;
        void WriteBeforeAfterReport() const;

        static ScenarioSummary BuildSummary(const ScenarioAccumulator& current);
        static double Percentile(std::vector<double> values, double percentile);
        static std::string BuildVerdictNotes(const ScenarioSummary& summary);
        static std::string FindPreviousSummaryCsv(
            const std::string& directory,
            const std::string& currentCsvPath
        );
        static std::vector<ScenarioSummary> LoadSummaryCsv(
            const std::string& path
        );
        static std::vector<std::string> ParseCsvLine(const std::string& line);

        std::ofstream csvFile_;
        std::ofstream textFile_;
        std::string csvFilePath_;
        std::string textFilePath_;
        std::string markdownFilePath_;
        std::string beforeAfterFilePath_;
        std::string previousSummaryCsvPath_;
        std::string generatedTimestamp_;
        std::vector<ScenarioSummary> summaries_;
        ScenarioAccumulator current_;
        bool hasCurrent_ = false;
        bool headerWritten_ = false;
    };

} // namespace net
