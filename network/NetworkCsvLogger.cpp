#include "NetworkCsvLogger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <utility>

namespace net {
namespace {

    std::string MakeTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);

        std::tm localTime{};
        localtime_s(&localTime, &time);

        std::ostringstream oss;
        oss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
        return oss.str();
    }

    bool NearlyEqual(double lhs, double rhs, double epsilon = 0.0001) {
        return std::fabs(lhs - rhs) <= epsilon;
    }

    bool MatchesCondition(
        const NetworkCondition& condition,
        bool enabled,
        double lossRate,
        uint32_t minDelayMs,
        uint32_t maxDelayMs,
        uint32_t burstLossLength,
        double duplicateRate = 0.0,
        double reorderRate = 0.0) {
        return condition.enabled == enabled &&
            NearlyEqual(condition.lossRate, lossRate) &&
            NearlyEqual(condition.duplicateRate, duplicateRate) &&
            NearlyEqual(condition.reorderRate, reorderRate) &&
            condition.minDelayMs == minDelayMs &&
            condition.maxDelayMs == maxDelayMs &&
            condition.burstLossLength == burstLossLength;
    }

    std::string EscapeCsv(std::string value) {
        const bool needsQuote =
            value.find_first_of(",\"\r\n") != std::string::npos;

        if (!needsQuote) {
            return value;
        }

        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back('"');

        for (char ch : value) {
            if (ch == '"') {
                escaped.push_back('"');
            }
            escaped.push_back(ch);
        }

        escaped.push_back('"');
        return escaped;
    }

    int Percent(double ratio) {
        return static_cast<int>(std::lround(ratio * 100.0));
    }

} // namespace

    bool NetworkCsvLogger::Start(const std::string& directory) {
        Stop();

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return false;
        }

        const std::filesystem::path path =
            std::filesystem::path(directory) /
            ("network_" + MakeTimestamp() + ".csv");

        file_.open(path, std::ios::out | std::ios::trunc);
        if (!file_) {
            filePath_.clear();
            return false;
        }

        filePath_ = path.string();
        headerWritten_ = false;
        WriteHeader();
        return true;
    }

    void NetworkCsvLogger::Stop() {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }

        headerWritten_ = false;
    }

    void NetworkCsvLogger::SetScenarioName(std::string name) {
        if (name.empty()) {
            scenarioName_ = "Auto";
            return;
        }

        scenarioName_ = std::move(name);
    }

    void NetworkCsvLogger::WriteSample(
        const NetworkStatsSnapshot& stats,
        double appTimeSec) {
        if (!file_.is_open()) {
            return;
        }

        if (!headerWritten_) {
            WriteHeader();
        }

        file_ << std::fixed << std::setprecision(3)
            << appTimeSec << ','
            << EscapeCsv(ResolveScenarioName(stats)) << ','
            << (stats.networkExperimentActive ? 1 : 0) << ','
            << EscapeCsv(stats.networkExperimentScenarioName) << ','
            << EscapeCsv(stats.networkExperimentAdaptiveMode) << ','
            << stats.networkExperimentRemainingSec << ','
            << stats.networkExperimentStepIndex << ','
            << stats.networkExperimentStepCount << ','
            << stats.latestFrameId << ','
            << stats.receiveFps << ','
            << stats.decodeFps << ','
            << stats.displayFps << ','
            << stats.currentLatencyMs << ','
            << stats.averageLatencyMs << ','
            << stats.currentRttMs << ','
            << stats.averageRttMs << ','
            << stats.currentJitterMs << ','
            << stats.packetLossRate << ','
            << stats.frameDropRate << ','
            << stats.receivedPackets << ','
            << stats.missingPackets << ','
            << stats.duplicatePackets << ','
            << stats.reorderedPackets << ','
            << stats.completedFrames << ','
            << stats.droppedFrames << ','
            << stats.deadlineDroppedFrames << ','
            << stats.outputQueueDroppedFrames << ','
            << stats.outputQueueDropEvents << ','
            << stats.outputQueueDropBurstEvents << ','
            << stats.lastOutputQueueDropFrameCount << ','
            << stats.lastOutputQueueDropQueueSize << ','
            << stats.lastOutputQueueDropOldestAgeMs << ','
            << stats.lastOutputQueueDropNewestAgeMs << ','
            << stats.maxOutputQueueDropOldestAgeMs << ','
            << EscapeCsv(stats.lastOutputQueueDropReason) << ','
            << stats.decodedFrames << ','
            << stats.displayedFrames << ','
            << stats.ackCount << ','
            << stats.lastAckMissingRate << ','
            << stats.ackRetransmittedFrames << ','
            << stats.deadlineNackSentFrames << ','
            << stats.deadlineNackRecoveredFrames << ','
            << stats.deadlineNackMissingChunks << ','
            << stats.deadlineNackExpiredDroppedFrames << ','
            << stats.deadlineNackExpiredAfterNackFrames << ','
            << stats.deadlineNackExpiredMissingChunks << ','
            << (stats.adaptiveEnabled ? 1 : 0) << ','
            << EscapeCsv(stats.adaptiveControlMode) << ','
            << stats.adaptiveTargetJpegQuality << ','
            << stats.adaptiveTargetFps << ','
            << stats.adaptiveTargetBitrateKbps << ','
            << stats.adaptiveTargetWidth << ','
            << stats.adaptiveTargetHeight << ','
            << stats.adaptiveRawFrameBytes << ','
            << stats.adaptiveEncodedFrameBytes << ','
            << stats.adaptiveCompressionRatio << ','
            << stats.adaptiveLastPacketLossRate << ','
            << stats.adaptiveLastReceiveFps << ','
            << stats.adaptiveLastDecodeFps << ','
            << stats.adaptiveLastJitterMs << ','
            << stats.adaptiveLastDisplayFps << ','
            << stats.adaptiveLastQoeScore << ','
            << EscapeCsv(stats.adaptiveDegradationCause) << ','
            << (stats.networkCondition.enabled ? 1 : 0) << ','
            << stats.networkCondition.lossRate << ','
            << stats.networkCondition.duplicateRate << ','
            << stats.networkCondition.reorderRate << ','
            << stats.networkCondition.minDelayMs << ','
            << stats.networkCondition.maxDelayMs << ','
            << stats.networkCondition.burstLossLength << ','
            << stats.networkSimulation.submittedPackets << ','
            << stats.networkSimulation.sentPackets << ','
            << stats.networkSimulation.droppedPackets << ','
            << stats.networkSimulation.duplicatedPackets << ','
            << stats.networkSimulation.reorderedPackets
            << '\n';

        file_.flush();
    }

    bool NetworkCsvLogger::IsRunning() const {
        return file_.is_open();
    }

    const std::string& NetworkCsvLogger::FilePath() const {
        return filePath_;
    }

    void NetworkCsvLogger::WriteHeader() {
        if (!file_.is_open() || headerWritten_) {
            return;
        }

        file_
            << "timeSec,"
            << "scenarioName,"
            << "networkExperimentActive,"
            << "networkExperimentScenarioName,"
            << "networkExperimentAdaptiveMode,"
            << "networkExperimentRemainingSec,"
            << "networkExperimentStepIndex,"
            << "networkExperimentStepCount,"
            << "frameId,"
            << "receiveFps,"
            << "decodeFps,"
            << "displayFps,"
            << "currentLatencyMs,"
            << "averageLatencyMs,"
            << "currentRttMs,"
            << "averageRttMs,"
            << "currentJitterMs,"
            << "packetLossRate,"
            << "frameDropRate,"
            << "receivedPackets,"
            << "missingPackets,"
            << "duplicatePackets,"
            << "reorderedPackets,"
            << "completedFrames,"
            << "droppedFrames,"
            << "deadlineDroppedFrames,"
            << "outputQueueDroppedFrames,"
            << "outputQueueDropEvents,"
            << "outputQueueDropBurstEvents,"
            << "lastOutputQueueDropFrameCount,"
            << "lastOutputQueueDropQueueSize,"
            << "lastOutputQueueDropOldestAgeMs,"
            << "lastOutputQueueDropNewestAgeMs,"
            << "maxOutputQueueDropOldestAgeMs,"
            << "lastOutputQueueDropReason,"
            << "decodedFrames,"
            << "displayedFrames,"
            << "ackCount,"
            << "lastAckMissingRate,"
            << "ackRetransmittedFrames,"
            << "deadlineNackSentFrames,"
            << "deadlineNackRecoveredFrames,"
            << "deadlineNackMissingChunks,"
            << "deadlineNackExpiredDroppedFrames,"
            << "deadlineNackExpiredAfterNackFrames,"
            << "deadlineNackExpiredMissingChunks,"
            << "adaptiveEnabled,"
            << "adaptiveControlMode,"
            << "targetJpegQuality,"
            << "targetFps,"
            << "targetBitrateKbps,"
            << "targetWidth,"
            << "targetHeight,"
            << "rawFrameBytes,"
            << "encodedFrameBytes,"
            << "compressionRatio,"
            << "adaptiveInputPacketLossRate,"
            << "adaptiveInputReceiveFps,"
            << "adaptiveInputDecodeFps,"
            << "adaptiveInputJitterMs,"
            << "adaptiveInputDisplayFps,"
            << "adaptiveQoeScore,"
            << "adaptiveDegradationCause,"
            << "simEnabled,"
            << "simLossRate,"
            << "simDuplicateRate,"
            << "simReorderRate,"
            << "simMinDelayMs,"
            << "simMaxDelayMs,"
            << "simBurstLossLength,"
            << "simSubmittedPackets,"
            << "simSentPackets,"
            << "simDroppedPackets,"
            << "simDuplicatedPackets,"
            << "simReorderedPackets"
            << '\n';

        headerWritten_ = true;
    }

    std::string NetworkCsvLogger::ResolveScenarioName(
        const NetworkStatsSnapshot& stats) const {
        if (!scenarioName_.empty() && scenarioName_ != "Auto") {
            return scenarioName_;
        }

        const NetworkCondition& condition = stats.networkCondition;

        if (!condition.enabled) {
            return "Baseline";
        }
        if (MatchesCondition(condition, true, 0.10, 0, 0, 0)) {
            return "10% loss";
        }
        if (MatchesCondition(condition, true, 0.0, 0, 50, 0)) {
            return "50ms jitter";
        }
        if (MatchesCondition(condition, true, 0.0, 100, 100, 0)) {
            return "100ms delay";
        }
        if (MatchesCondition(condition, true, 0.03, 0, 0, 8)) {
            return "Burst loss";
        }

        std::ostringstream oss;
        oss << "Custom"
            << "_loss" << Percent(condition.lossRate)
            << "_delay" << condition.minDelayMs
            << "-" << condition.maxDelayMs
            << "_dup" << Percent(condition.duplicateRate)
            << "_reorder" << Percent(condition.reorderRate)
            << "_burst" << condition.burstLossLength;

        return oss.str();
    }

} // namespace net
