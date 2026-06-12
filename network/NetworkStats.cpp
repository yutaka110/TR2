#include "NetworkStats.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <ctime>

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

    std::ofstream& FrameRecoveryTraceFile() {
        static std::ofstream file;
        static bool initialized = false;
        if (initialized) {
            return file;
        }

        initialized = true;
        const std::string fileName =
            "frame_recovery_trace_" + MakeTimestamp() + ".csv";
        for (const char* directory : {"logs", "network_logs"}) {
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            const std::filesystem::path path =
                std::filesystem::path(directory) / fileName;
            file.open(path, std::ios::out | std::ios::trunc);
            if (file) {
                break;
            }
            file.clear();
        }
        if (file) {
            file
                << "eventTimeUs,"
                << "eventName,"
                << "outcome,"
                << "frameId,"
                << "streamId,"
                << "codec,"
                << "keyFrame,"
                << "largeFrame,"
                << "chunkCount,"
                << "receivedChunks,"
                << "missingChunks,"
                << "fecParityPackets,"
                << "fecRecoveredChunks,"
                << "nackCount,"
                << "postNackReceivedChunks,"
                << "nackRequestedChunks,"
                << "retransmitReceivedChunks,"
                << "retransmitDuplicatePackets,"
                << "emergencyRepairReceivedChunks,"
                << "emergencyRepairDuplicatePackets,"
                << "lastPacketSequence,"
                << "lastPacketChunkIndex,"
                << "lastRetransmitSequence,"
                << "lastRetransmitChunkIndex,"
                << "eventPacketSequence,"
                << "eventPacketChunkIndex,"
                << "eventPacketWasRetransmit,"
                << "eventPacketWasEmergencyRepair,"
                << "recoveryExpireTimeUs,"
                << "recoverySlackMs,"
                << "lastUpdateTimeUs,"
                << "lastUpdateAgeMs,"
                << "recentArrivalIntervalMs,"
                << "likelyArrivalSuppressionCount,"
                << "likelyArrivalWindowHit,"
                << "likelyArrivalRecentHit,"
                << "likelyArrivalTightCadenceHit,"
                << "likelyArrivalFecParityHit,"
                << "likelyArrivalFecRecoverableHit,"
                << "keyTinyLastChanceNackSent,"
                << "keyTinyLastChanceMissingChunks,"
                << "keyTinyLastChanceSlackMs,"
                << "sendTimeUs,"
                << "firstReceiveTimeUs,"
                << "ageMs\n";
        }
        return file;
    }

    void WriteFrameRecoveryTrace(
        uint64_t eventTimeUs,
        const char* eventName,
        const char* outcome,
        uint32_t frameId,
        uint32_t streamId,
        CodecType codecType,
        bool keyFrame,
        bool largeFrame,
        uint32_t chunkCount,
        uint32_t receivedChunks,
        uint32_t missingChunks,
        uint32_t fecParityPackets,
        uint32_t fecRecoveredChunks,
        uint32_t nackCount,
        uint32_t postNackReceivedChunks,
        uint32_t nackRequestedChunks,
        uint32_t retransmitReceivedChunks,
        uint32_t retransmitDuplicatePackets,
        uint32_t emergencyRepairReceivedChunks,
        uint32_t emergencyRepairDuplicatePackets,
        uint32_t lastPacketSequence,
        uint32_t lastPacketChunkIndex,
        uint32_t lastRetransmitSequence,
        uint32_t lastRetransmitChunkIndex,
        uint32_t eventPacketSequence,
        uint32_t eventPacketChunkIndex,
        bool eventPacketWasRetransmit,
        bool eventPacketWasEmergencyRepair,
        uint64_t recoveryExpireTimeUs,
        uint64_t lastUpdateTimeUs,
        uint64_t recentArrivalIntervalUs,
        uint32_t likelyArrivalSuppressionCount,
        bool likelyArrivalWindowHit,
        bool likelyArrivalRecentHit,
        bool likelyArrivalTightCadenceHit,
        bool likelyArrivalFecParityHit,
        bool likelyArrivalFecRecoverableHit,
        bool keyTinyLastChanceNackSent,
        uint32_t keyTinyLastChanceMissingChunks,
        uint64_t keyTinyLastChanceSlackUs,
        uint64_t sendTimeUs,
        uint64_t firstReceiveTimeUs,
        double ageMs
    ) {
        std::ofstream& file = FrameRecoveryTraceFile();
        if (!file) {
            return;
        }

        double recoverySlackMs = 0.0;
        if (recoveryExpireTimeUs != 0) {
            if (recoveryExpireTimeUs >= eventTimeUs) {
                recoverySlackMs =
                    static_cast<double>(recoveryExpireTimeUs - eventTimeUs) /
                    1000.0;
            }
            else {
                recoverySlackMs =
                    -static_cast<double>(eventTimeUs - recoveryExpireTimeUs) /
                    1000.0;
            }
        }

        double lastUpdateAgeMs = 0.0;
        if (lastUpdateTimeUs != 0 && eventTimeUs >= lastUpdateTimeUs) {
            lastUpdateAgeMs =
                static_cast<double>(eventTimeUs - lastUpdateTimeUs) /
                1000.0;
        }

        const double recentArrivalIntervalMs =
            static_cast<double>(recentArrivalIntervalUs) / 1000.0;
        const double keyTinyLastChanceSlackMs =
            static_cast<double>(keyTinyLastChanceSlackUs) / 1000.0;

        file
            << eventTimeUs << ','
            << EscapeCsv(eventName != nullptr ? eventName : "unknown") << ','
            << EscapeCsv(outcome != nullptr ? outcome : "unknown") << ','
            << frameId << ','
            << streamId << ','
            << EscapeCsv(ToString(codecType)) << ','
            << (keyFrame ? 1 : 0) << ','
            << (largeFrame ? 1 : 0) << ','
            << chunkCount << ','
            << receivedChunks << ','
            << missingChunks << ','
            << fecParityPackets << ','
            << fecRecoveredChunks << ','
            << nackCount << ','
            << postNackReceivedChunks << ','
            << nackRequestedChunks << ','
            << retransmitReceivedChunks << ','
            << retransmitDuplicatePackets << ','
            << emergencyRepairReceivedChunks << ','
            << emergencyRepairDuplicatePackets << ','
            << lastPacketSequence << ','
            << lastPacketChunkIndex << ','
            << lastRetransmitSequence << ','
            << lastRetransmitChunkIndex << ','
            << eventPacketSequence << ','
            << eventPacketChunkIndex << ','
            << (eventPacketWasRetransmit ? 1 : 0) << ','
            << (eventPacketWasEmergencyRepair ? 1 : 0) << ','
            << recoveryExpireTimeUs << ','
            << recoverySlackMs << ','
            << lastUpdateTimeUs << ','
            << lastUpdateAgeMs << ','
            << recentArrivalIntervalMs << ','
            << likelyArrivalSuppressionCount << ','
            << (likelyArrivalWindowHit ? 1 : 0) << ','
            << (likelyArrivalRecentHit ? 1 : 0) << ','
            << (likelyArrivalTightCadenceHit ? 1 : 0) << ','
            << (likelyArrivalFecParityHit ? 1 : 0) << ','
            << (likelyArrivalFecRecoverableHit ? 1 : 0) << ','
            << (keyTinyLastChanceNackSent ? 1 : 0) << ','
            << keyTinyLastChanceMissingChunks << ','
            << keyTinyLastChanceSlackMs << ','
            << sendTimeUs << ','
            << firstReceiveTimeUs << ','
            << ageMs
            << '\n';
        file.flush();
    }

} // namespace

    NetworkStats::NetworkStats() {
        Reset();
    }

    void NetworkStats::Reset() {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_ = NetworkStatsSnapshot{};

        const uint64_t nowUs = NowMicroseconds();

        startTimeUs_ = nowUs;

        lastFpsUpdateTimeUs_ = nowUs;
        lastDecodeFpsUpdateTimeUs_ = nowUs;
        lastDisplayFpsUpdateTimeUs_ = nowUs;

        framesAtLastFpsUpdate_ = 0;
        decodedFramesAtLastFpsUpdate_ = 0;
        displayedFramesAtLastFpsUpdate_ = 0;

        bytesAtLastThroughputUpdate_ = 0;
        frameBytesAtLastBitrateUpdate_ = 0;

        totalFrameBytes_ = 0;

        latencySumMs_ = 0.0;
        retransmitLateAfterCompletedDelaySumMs_ = 0.0;
        retransmitLateAfterCompletedDelaySamples_ = 0;
        retransmitLateAfterCompletedSendToCompleteSumMs_ = 0.0;
        retransmitLateAfterCompletedSendToCompleteSamples_ = 0;
        h264KeyTinyEmergencySendToArrivalSumMs_ = 0.0;
        h264KeyTinyEmergencySendToArrivalSamples_ = 0;
        h264KeyTinyLastChanceSlackSumMs_ = 0.0;
        h264KeyTinyLastChanceSlackSamples_ = 0;

        hasPreviousFrameArrival_ = false;
        previousFrameReceiveTimeUs_ = 0;
        jitterSumMs_ = 0.0;
        jitterSamples_ = 0;

        rttSumMs_ = 0.0;
    }

    void NetworkStats::OnPacketReceived(uint32_t packetBytes) {
        std::lock_guard<std::mutex> lock(mutex_);

        const uint64_t nowUs = NowMicroseconds();

        snapshot_.receivedPackets++;
        snapshot_.receivedBytes += packetBytes;
        snapshot_.lastUpdateTimeUs = nowUs;

        const uint64_t totalObservedPackets =
            snapshot_.receivedPackets + snapshot_.missingPackets;

        if (totalObservedPackets > 0) {
            snapshot_.packetLossRate =
                static_cast<double>(snapshot_.missingPackets) /
                static_cast<double>(totalObservedPackets);
        }

        UpdateThroughput(nowUs);
    }

    void NetworkStats::OnFrameCompleted(
        uint32_t frameId,
        uint64_t frameBytes,
        uint64_t sendTimeUs,
        uint64_t receiveTimeUs
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.completedFrames++;
        snapshot_.latestFrameId = frameId;
        snapshot_.lastUpdateTimeUs = receiveTimeUs;

        totalFrameBytes_ += frameBytes;

        // ============================================================
        // Latency
        // ============================================================
        if (receiveTimeUs >= sendTimeUs) {
            const double latencyMs =
                static_cast<double>(receiveTimeUs - sendTimeUs) / 1000.0;

            snapshot_.currentLatencyMs = latencyMs;
            snapshot_.maxLatencyMs = (std::max)(snapshot_.maxLatencyMs, latencyMs);

            latencySumMs_ += latencyMs;
            snapshot_.averageLatencyMs =
                latencySumMs_ / static_cast<double>(snapshot_.completedFrames);
        }

        // ============================================================
        // Jitter
        // ------------------------------------------------------------
        // 到着間隔の揺れを見る。
        // フレーム間隔が毎回ほぼ一定ならjitterは小さくなる。
        // ============================================================
        if (hasPreviousFrameArrival_) {
            const uint64_t intervalUs =
                receiveTimeUs >= previousFrameReceiveTimeUs_
                ? receiveTimeUs - previousFrameReceiveTimeUs_
                : 0;

            const double intervalMs =
                static_cast<double>(intervalUs) / 1000.0;

            // 想定フレーム間隔を receiveFps から近似。
            // FPSがまだ取れていない序盤は 30fps 相当で仮置き。
            const double expectedIntervalMs =
                snapshot_.receiveFps > 1.0
                ? 1000.0 / snapshot_.receiveFps
                : 1000.0 / 30.0;

            const double jitterMs =
                std::abs(intervalMs - expectedIntervalMs);

            snapshot_.currentJitterMs = jitterMs;
            snapshot_.maxJitterMs = (std::max)(snapshot_.maxJitterMs, jitterMs);

            jitterSumMs_ += jitterMs;
            jitterSamples_++;

            snapshot_.averageJitterMs =
                jitterSumMs_ / static_cast<double>(jitterSamples_);
        }

        previousFrameReceiveTimeUs_ = receiveTimeUs;
        hasPreviousFrameArrival_ = true;

        UpdateReceiveFps(receiveTimeUs);
        UpdateBitrate(receiveTimeUs);

        const uint64_t totalFrames =
            snapshot_.completedFrames + snapshot_.droppedFrames;

        if (totalFrames > 0) {
            snapshot_.frameDropRate =
                static_cast<double>(snapshot_.droppedFrames) /
                static_cast<double>(totalFrames);
        }
    }

    void NetworkStats::OnRttSample(double rttMs) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.currentRttMs = rttMs;
        snapshot_.maxRttMs = (std::max)(snapshot_.maxRttMs, rttMs);

        snapshot_.rttSamples++;
        rttSumMs_ += rttMs;

        snapshot_.averageRttMs =
            rttSumMs_ / static_cast<double>(snapshot_.rttSamples);

        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnDroppedFrame() {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.droppedFrames++;

        const uint64_t totalFrames =
            snapshot_.completedFrames + snapshot_.droppedFrames;

        if (totalFrames > 0) {
            snapshot_.frameDropRate =
                static_cast<double>(snapshot_.droppedFrames) /
                static_cast<double>(totalFrames);
        }

        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnDeadlineDroppedFrames(uint32_t droppedFrames) {
        if (droppedFrames == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.deadlineDroppedFrames += droppedFrames;
        snapshot_.droppedFrames += droppedFrames;

        const uint64_t totalFrames =
            snapshot_.completedFrames + snapshot_.droppedFrames;

        if (totalFrames > 0) {
            snapshot_.frameDropRate =
                static_cast<double>(snapshot_.droppedFrames) /
                static_cast<double>(totalFrames);
        }

        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnOutputQueueDroppedFrames(uint32_t droppedFrames) {
        if (droppedFrames == 0) {
            return;
        }

        OnOutputQueueDropEvent(
            droppedFrames,
            0,
            0.0,
            0.0,
            "unknown"
        );
    }

    void NetworkStats::OnCompletedQueuePush(
        uint32_t queueSizeAfterPush,
        double popAgeMs,
        double pushIntervalMs
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.completedQueuePushes++;
        snapshot_.completedQueueSize = queueSizeAfterPush;
        snapshot_.maxCompletedQueueSize =
            (std::max)(snapshot_.maxCompletedQueueSize, queueSizeAfterPush);
        snapshot_.completedQueueLastPopAgeMs = popAgeMs;
        snapshot_.completedQueueMaxPopAgeMs =
            (std::max)(snapshot_.completedQueueMaxPopAgeMs, popAgeMs);
        snapshot_.completedQueueLastPushIntervalMs = pushIntervalMs;
        snapshot_.completedQueueMaxPushIntervalMs =
            (std::max)(
                snapshot_.completedQueueMaxPushIntervalMs,
                pushIntervalMs
            );
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnCompletedQueuePop(uint32_t queueSizeAfterPop) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.completedQueuePops++;
        snapshot_.completedQueueSize = queueSizeAfterPop;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnCompletedQueueEmptyPoll() {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.completedQueueEmptyPolls++;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnOutputQueueDropEvent(
        uint32_t droppedFrames,
        uint32_t queueSizeBeforeDrop,
        double oldestDroppedAgeMs,
        double newestFrameAgeMs,
        const char* reason,
        double completedQueuePopAgeMs,
        double completedQueuePushIntervalMs
    ) {
        if (droppedFrames == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.outputQueueDroppedFrames += droppedFrames;
        snapshot_.outputQueueDropEvents++;
        snapshot_.lastOutputQueueDropFrameCount = droppedFrames;
        snapshot_.lastOutputQueueDropQueueSize = queueSizeBeforeDrop;
        snapshot_.lastOutputQueueDropOldestAgeMs = oldestDroppedAgeMs;
        snapshot_.lastOutputQueueDropNewestAgeMs = newestFrameAgeMs;
        snapshot_.maxOutputQueueDropOldestAgeMs =
            (std::max)(
                snapshot_.maxOutputQueueDropOldestAgeMs,
                oldestDroppedAgeMs
            );
        snapshot_.lastOutputQueueDropPopAgeMs = completedQueuePopAgeMs;
        snapshot_.lastOutputQueueDropPushIntervalMs =
            completedQueuePushIntervalMs;
        snapshot_.lastOutputQueueDropReason =
            reason != nullptr ? reason : "unknown";

        if (snapshot_.lastOutputQueueDropReason == "jitter-burst-release") {
            snapshot_.outputQueueDropBurstEvents++;
        }

        snapshot_.droppedFrames += droppedFrames;

        const uint64_t totalFrames =
            snapshot_.completedFrames + snapshot_.droppedFrames;

        if (totalFrames > 0) {
            snapshot_.frameDropRate =
                static_cast<double>(snapshot_.droppedFrames) /
                static_cast<double>(totalFrames);
        }

        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnDeadlineNackSent(
        uint32_t missingChunkCount,
        CodecType codecType,
        bool keyFrame,
        bool largeFrame
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.deadlineNackSentFrames++;
        snapshot_.deadlineNackMissingChunks += missingChunkCount;
        if (codecType == CodecType::H264) {
            if (keyFrame) {
                snapshot_.deadlineNackSentH264KeyFrames++;
            }
            else if (largeFrame) {
                snapshot_.deadlineNackSentH264LargeFrames++;
            }
            else {
                snapshot_.deadlineNackSentH264DeltaFrames++;
            }
        }
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnDeadlineNackRecoveredFrame() {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.deadlineNackRecoveredFrames++;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnDeadlineNackExpiredFrame(
        uint32_t missingChunkCount,
        bool nackSent,
        CodecType codecType,
        bool keyFrame,
        bool largeFrame
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.deadlineNackExpiredDroppedFrames++;
        snapshot_.deadlineNackExpiredMissingChunks += missingChunkCount;
        if (codecType == CodecType::H264) {
            if (keyFrame) {
                snapshot_.deadlineNackExpiredH264KeyFrames++;
                snapshot_.deadlineNackExpiredH264KeyMissingChunks +=
                    missingChunkCount;
                snapshot_.deadlineNackLastExpiredH264KeyMissingChunks =
                    missingChunkCount;
                if (missingChunkCount <= 1) {
                    snapshot_.deadlineNackExpiredH264KeyMissingChunks1++;
                }
                else if (missingChunkCount <= 4) {
                    snapshot_.deadlineNackExpiredH264KeyMissingChunks2To4++;
                }
                else if (missingChunkCount <= 8) {
                    snapshot_.deadlineNackExpiredH264KeyMissingChunks5To8++;
                }
                else if (missingChunkCount <= 16) {
                    snapshot_.deadlineNackExpiredH264KeyMissingChunks9To16++;
                }
                else {
                    snapshot_.deadlineNackExpiredH264KeyMissingChunks17Plus++;
                }
            }
            else if (largeFrame) {
                snapshot_.deadlineNackExpiredH264LargeFrames++;
            }
            else {
                snapshot_.deadlineNackExpiredH264DeltaFrames++;
            }
        }

        if (nackSent) {
            snapshot_.deadlineNackExpiredAfterNackFrames++;
        }

        snapshot_.droppedFrames++;

        const uint64_t totalFrames =
            snapshot_.completedFrames + snapshot_.droppedFrames;

        if (totalFrames > 0) {
            snapshot_.frameDropRate =
                static_cast<double>(snapshot_.droppedFrames) /
                static_cast<double>(totalFrames);
        }

        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnH264KeySmallMissingDeadlineRescue(
        uint32_t missingChunkCount
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.h264KeySmallMissingDeadlineRescueFrames++;
        snapshot_.h264KeySmallMissingDeadlineRescueMissingChunks +=
            missingChunkCount;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnH264KeySmallMissingDeadlineRescueOutcome(
        uint32_t missingChunkCount,
        bool completed,
        bool rejected
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (completed) {
            snapshot_.h264KeySmallMissingDeadlineRescueCompletedFrames++;
            snapshot_.h264KeySmallMissingDeadlineRescueCompletedMissingChunks +=
                missingChunkCount;
        }
        else if (rejected) {
            snapshot_.h264KeySmallMissingDeadlineRescueRejectedFrames++;
        }
        else {
            snapshot_.h264KeySmallMissingDeadlineRescueExpiredFrames++;
            snapshot_.h264KeySmallMissingDeadlineRescueExpiredMissingChunks +=
                missingChunkCount;
        }
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnFecParityPacket() {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.fecParityPackets++;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnFecRecoveredFrame(uint32_t recoveredChunkCount) {
        if (recoveredChunkCount == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.fecRecoveredFrames++;
        snapshot_.fecRecoveredChunks += recoveredChunkCount;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnH264ReassemblerAuRejected(const char* reason) {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::string reasonText =
            reason != nullptr ? reason : "unknown";

        snapshot_.h264ReassemblerAuRejectedFrames++;
        snapshot_.h264ReassemblerLastRejectReason = reasonText;
        if (reasonText == "payload-header-failure") {
            snapshot_.h264ReassemblerHeaderFailures++;
        }
        else if (reasonText == "payload-size-mismatch") {
            snapshot_.h264ReassemblerPayloadSizeMismatches++;
        }
        else if (reasonText == "frame-id-mismatch") {
            snapshot_.h264ReassemblerFrameIdMismatches++;
        }
        else if (reasonText == "crc-mismatch") {
            snapshot_.h264ReassemblerCrcMismatches++;
        }

        snapshot_.droppedFrames++;
        const uint64_t totalFrames =
            snapshot_.completedFrames + snapshot_.droppedFrames;
        if (totalFrames > 0) {
            snapshot_.frameDropRate =
                static_cast<double>(snapshot_.droppedFrames) /
                static_cast<double>(totalFrames);
        }

        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnFrameRecoveryOutcome(
        const char* eventName,
        const char* outcome,
        uint32_t frameId,
        uint32_t streamId,
        CodecType codecType,
        bool keyFrame,
        bool largeFrame,
        uint32_t chunkCount,
        uint32_t receivedChunks,
        uint32_t missingChunks,
        uint32_t fecParityPackets,
        uint32_t fecRecoveredChunks,
        uint32_t nackCount,
        uint32_t postNackReceivedChunks,
        uint32_t nackRequestedChunks,
        uint32_t retransmitReceivedChunks,
        uint32_t retransmitDuplicatePackets,
        uint32_t emergencyRepairReceivedChunks,
        uint32_t emergencyRepairDuplicatePackets,
        uint32_t lastPacketSequence,
        uint32_t lastPacketChunkIndex,
        uint32_t lastRetransmitSequence,
        uint32_t lastRetransmitChunkIndex,
        uint32_t eventPacketSequence,
        uint32_t eventPacketChunkIndex,
        bool eventPacketWasRetransmit,
        bool eventPacketWasEmergencyRepair,
        uint64_t recoveryExpireTimeUs,
        uint64_t lastUpdateTimeUs,
        uint64_t recentArrivalIntervalUs,
        uint32_t likelyArrivalSuppressionCount,
        bool likelyArrivalWindowHit,
        bool likelyArrivalRecentHit,
        bool likelyArrivalTightCadenceHit,
        bool likelyArrivalFecParityHit,
        bool likelyArrivalFecRecoverableHit,
        bool keyTinyLastChanceNackSent,
        uint32_t keyTinyLastChanceMissingChunks,
        uint64_t keyTinyLastChanceSlackUs,
        uint64_t sendTimeUs,
        uint64_t firstReceiveTimeUs,
        uint64_t eventTimeUs
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::string eventText =
            eventName != nullptr ? eventName : "unknown";
        const std::string outcomeText =
            outcome != nullptr ? outcome : "unknown";
        double ageMs = 0.0;
        if (firstReceiveTimeUs != 0 && eventTimeUs >= firstReceiveTimeUs) {
            ageMs =
                static_cast<double>(eventTimeUs - firstReceiveTimeUs) /
                1000.0;
        }

        snapshot_.frameRecoveryOutcomeEvents++;
        if (outcomeText == "completed") {
            snapshot_.frameRecoveryCompletedFrames++;
        }
        else if (outcomeText == "expired") {
            snapshot_.frameRecoveryExpiredFrames++;
        }
        else if (outcomeText == "rejected") {
            snapshot_.frameRecoveryRejectedFrames++;
        }

        if (eventText == "nack-sent" ||
            eventText == "nack-sent-key-tiny-early" ||
            eventText == "nack-sent-last-chance-key-tiny") {
            snapshot_.frameRecoveryNackSentFrames++;
        }
        else if (eventText == "fec-recovered") {
            snapshot_.frameRecoveryFecRecoveredFrames++;
        }
        else if (eventText == "retransmit-arrived") {
            snapshot_.retransmitUsefulChunks++;
        }
        else if (eventText == "retransmit-duplicate") {
            snapshot_.retransmitDuplicatePackets++;
        }
        else if (eventText == "retransmit-late-after-completed") {
            snapshot_.retransmitLateAfterCompletedPackets++;
        }
        else if (eventText == "retransmit-late-after-expired") {
            snapshot_.retransmitLateAfterExpiredPackets++;
        }
        else if (eventText == "retransmit-late-after-rejected") {
            snapshot_.retransmitLateAfterRejectedPackets++;
        }

        const bool h264KeyRepairBucket =
            codecType == CodecType::H264 &&
            keyFrame &&
            nackRequestedChunks > 0 &&
            nackRequestedChunks <= 4;
        if (h264KeyRepairBucket) {
            const bool bucket1To2 = nackRequestedChunks <= 2;
            uint64_t& completedFrames = bucket1To2
                ? snapshot_.h264KeyRepair1To2CompletedFrames
                : snapshot_.h264KeyRepair3To4CompletedFrames;
            uint64_t& expiredFrames = bucket1To2
                ? snapshot_.h264KeyRepair1To2ExpiredFrames
                : snapshot_.h264KeyRepair3To4ExpiredFrames;
            uint64_t& arrivedPackets = bucket1To2
                ? snapshot_.h264KeyRepair1To2ArrivedPackets
                : snapshot_.h264KeyRepair3To4ArrivedPackets;
            uint64_t& duplicatePackets = bucket1To2
                ? snapshot_.h264KeyRepair1To2DuplicatePackets
                : snapshot_.h264KeyRepair3To4DuplicatePackets;
            uint64_t& lateCompletedPackets = bucket1To2
                ? snapshot_.h264KeyRepair1To2LateCompletedPackets
                : snapshot_.h264KeyRepair3To4LateCompletedPackets;
            uint64_t& lateExpiredPackets = bucket1To2
                ? snapshot_.h264KeyRepair1To2LateExpiredPackets
                : snapshot_.h264KeyRepair3To4LateExpiredPackets;
            uint64_t& lateRejectedPackets = bucket1To2
                ? snapshot_.h264KeyRepair1To2LateRejectedPackets
                : snapshot_.h264KeyRepair3To4LateRejectedPackets;

            if (eventText == "completed" &&
                outcomeText == "completed" &&
                retransmitReceivedChunks > 0) {
                completedFrames++;
            }
            else if (outcomeText == "expired" &&
                eventText != "retransmit-late-after-expired" &&
                retransmitReceivedChunks > 0) {
                expiredFrames++;
            }
            else if (eventText == "retransmit-arrived") {
                arrivedPackets++;
            }
            else if (eventText == "retransmit-duplicate") {
                duplicatePackets++;
            }
            else if (eventText == "retransmit-late-after-completed") {
                lateCompletedPackets++;
            }
            else if (eventText == "retransmit-late-after-expired") {
                lateExpiredPackets++;
            }
            else if (eventText == "retransmit-late-after-rejected") {
                lateRejectedPackets++;
            }
        }

        const bool h264KeyTinyEmergencyBucket =
            codecType == CodecType::H264 &&
            keyFrame &&
            nackRequestedChunks > 0 &&
            nackRequestedChunks <= 2;
        if (h264KeyTinyEmergencyBucket) {
            if (eventText == "completed" &&
                outcomeText == "completed" &&
                emergencyRepairReceivedChunks > 0) {
                snapshot_.h264KeyTinyEmergencyCompletedFrames++;
            }
            else if (eventPacketWasEmergencyRepair &&
                eventText == "retransmit-arrived") {
                snapshot_
                    .h264KeyTinyEmergencyArrivedBeforeRetirePackets++;
            }
            else if (eventPacketWasEmergencyRepair &&
                eventText == "retransmit-duplicate") {
                snapshot_
                    .h264KeyTinyEmergencyDuplicateBeforeRetirePackets++;
            }
            else if (eventPacketWasEmergencyRepair &&
                eventText == "retransmit-late-after-completed") {
                snapshot_
                    .h264KeyTinyEmergencyArrivedAfterCompletePackets++;
            }
            else if (eventPacketWasEmergencyRepair &&
                eventText == "retransmit-late-after-expired") {
                snapshot_
                    .h264KeyTinyEmergencyArrivedAfterExpirePackets++;
            }
            else if (eventPacketWasEmergencyRepair &&
                eventText == "retransmit-late-after-rejected") {
                snapshot_
                    .h264KeyTinyEmergencyArrivedAfterRejectedPackets++;
            }

            if (eventPacketWasEmergencyRepair &&
                sendTimeUs != 0 &&
                eventTimeUs >= sendTimeUs) {
                const double sendToArrivalMs =
                    static_cast<double>(eventTimeUs - sendTimeUs) / 1000.0;
                h264KeyTinyEmergencySendToArrivalSumMs_ +=
                    sendToArrivalMs;
                h264KeyTinyEmergencySendToArrivalSamples_++;
                snapshot_.h264KeyTinyEmergencySendToArrivalAvgMs =
                    h264KeyTinyEmergencySendToArrivalSumMs_ /
                    static_cast<double>(
                        h264KeyTinyEmergencySendToArrivalSamples_);
                snapshot_.h264KeyTinyEmergencySendToArrivalMaxMs =
                    (std::max)(
                        snapshot_.h264KeyTinyEmergencySendToArrivalMaxMs,
                        sendToArrivalMs);
            }
        }

        if (eventText == "nack-sent-key-tiny-early") {
            snapshot_.h264KeyTinyEarlyNackFrames++;
            snapshot_.h264KeyTinyEarlyNackMissingChunks +=
                missingChunks;
        }

        if (keyTinyLastChanceNackSent) {
            if (eventText == "nack-sent-last-chance-key-tiny") {
                snapshot_.h264KeyTinyLastChanceNackFrames++;
                if (keyTinyLastChanceMissingChunks == 1) {
                    snapshot_.h264KeyTinyLastChanceMissing1Frames++;
                }

                const double slackMs =
                    static_cast<double>(keyTinyLastChanceSlackUs) / 1000.0;
                h264KeyTinyLastChanceSlackSumMs_ += slackMs;
                h264KeyTinyLastChanceSlackSamples_++;
                snapshot_.h264KeyTinyLastChanceSlackAvgMs =
                    h264KeyTinyLastChanceSlackSumMs_ /
                    static_cast<double>(
                        h264KeyTinyLastChanceSlackSamples_);
                if (h264KeyTinyLastChanceSlackSamples_ == 1) {
                    snapshot_.h264KeyTinyLastChanceSlackMinMs = slackMs;
                    snapshot_.h264KeyTinyLastChanceSlackMaxMs = slackMs;
                }
                else {
                    snapshot_.h264KeyTinyLastChanceSlackMinMs =
                        (std::min)(
                            snapshot_.h264KeyTinyLastChanceSlackMinMs,
                            slackMs);
                    snapshot_.h264KeyTinyLastChanceSlackMaxMs =
                        (std::max)(
                            snapshot_.h264KeyTinyLastChanceSlackMaxMs,
                            slackMs);
                }
            }
            else if (eventText == "completed" &&
                outcomeText == "completed") {
                snapshot_.h264KeyTinyLastChanceCompletedFrames++;
            }
            else if (outcomeText == "expired" &&
                eventText != "retransmit-late-after-expired") {
                snapshot_.h264KeyTinyLastChanceExpiredFrames++;
            }
            else if (eventText == "retransmit-arrived") {
                snapshot_
                    .h264KeyTinyLastChanceArrivedBeforeRetirePackets++;
            }
            else if (eventText == "retransmit-duplicate") {
                snapshot_
                    .h264KeyTinyLastChanceDuplicateBeforeRetirePackets++;
            }
            else if (eventText == "retransmit-late-after-completed") {
                snapshot_.h264KeyTinyLastChanceLateCompletedPackets++;
            }
            else if (eventText == "retransmit-late-after-expired") {
                snapshot_.h264KeyTinyLastChanceLateExpiredPackets++;
            }
            else if (eventText == "retransmit-late-after-rejected") {
                snapshot_.h264KeyTinyLastChanceLateRejectedPackets++;
            }
        }

        if (outcomeText == "completed" && retransmitReceivedChunks > 0) {
            snapshot_.retransmitCompletedFrames++;
        }
        else if (outcomeText == "expired" && retransmitReceivedChunks > 0) {
            snapshot_.retransmitExpiredFrames++;
        }

        const uint64_t retransmitArrivals =
            snapshot_.retransmitUsefulChunks +
            snapshot_.retransmitDuplicatePackets;
        if (retransmitArrivals > 0) {
            snapshot_.retransmitUsefulnessRatio =
                static_cast<double>(snapshot_.retransmitUsefulChunks) /
                static_cast<double>(retransmitArrivals);
            snapshot_.retransmitDuplicateRatio =
                static_cast<double>(snapshot_.retransmitDuplicatePackets) /
                static_cast<double>(retransmitArrivals);
        }

        snapshot_.retransmitClassifiedPackets =
            snapshot_.retransmitUsefulChunks +
            snapshot_.retransmitDuplicatePackets +
            snapshot_.retransmitLateAfterCompletedPackets +
            snapshot_.retransmitLateAfterExpiredPackets +
            snapshot_.retransmitLateAfterRejectedPackets;

        const uint64_t retransmitOutcomeFrames =
            snapshot_.retransmitCompletedFrames +
            snapshot_.retransmitExpiredFrames;
        if (retransmitOutcomeFrames > 0) {
            snapshot_.retransmitExpiredAfterUsefulRatio =
                static_cast<double>(snapshot_.retransmitExpiredFrames) /
                static_cast<double>(retransmitOutcomeFrames);
        }

        snapshot_.frameRecoveryLastFrameId = frameId;
        snapshot_.frameRecoveryLastStreamId = streamId;
        snapshot_.frameRecoveryLastEvent = eventText;
        snapshot_.frameRecoveryLastOutcome = outcomeText;
        snapshot_.frameRecoveryLastCodec = ToString(codecType);
        snapshot_.frameRecoveryLastKeyFrame = keyFrame;
        snapshot_.frameRecoveryLastLargeFrame = largeFrame;
        snapshot_.frameRecoveryLastChunkCount = chunkCount;
        snapshot_.frameRecoveryLastReceivedChunks = receivedChunks;
        snapshot_.frameRecoveryLastMissingChunks = missingChunks;
        snapshot_.frameRecoveryLastFecParityPackets = fecParityPackets;
        snapshot_.frameRecoveryLastFecRecoveredChunks = fecRecoveredChunks;
        snapshot_.frameRecoveryLastNackCount = nackCount;
        snapshot_.frameRecoveryLastPostNackReceivedChunks =
            postNackReceivedChunks;
        snapshot_.frameRecoveryLastNackRequestedChunks =
            nackRequestedChunks;
        snapshot_.frameRecoveryLastRetransmitReceivedChunks =
            retransmitReceivedChunks;
        snapshot_.frameRecoveryLastRetransmitDuplicatePackets =
            retransmitDuplicatePackets;
        snapshot_.frameRecoveryLastPacketSequence = lastPacketSequence;
        snapshot_.frameRecoveryLastPacketChunkIndex = lastPacketChunkIndex;
        snapshot_.frameRecoveryLastRetransmitSequence =
            lastRetransmitSequence;
        snapshot_.frameRecoveryLastRetransmitChunkIndex =
            lastRetransmitChunkIndex;
        snapshot_.frameRecoveryLastEventPacketSequence =
            eventPacketSequence;
        snapshot_.frameRecoveryLastEventPacketChunkIndex =
            eventPacketChunkIndex;
        snapshot_.frameRecoveryLastEventWasRetransmit =
            eventPacketWasRetransmit;
        snapshot_.frameRecoveryLastAgeMs = ageMs;
        snapshot_.lastUpdateTimeUs = eventTimeUs;

        WriteFrameRecoveryTrace(
            eventTimeUs,
            eventName,
            outcome,
            frameId,
            streamId,
            codecType,
            keyFrame,
            largeFrame,
            chunkCount,
            receivedChunks,
            missingChunks,
            fecParityPackets,
            fecRecoveredChunks,
            nackCount,
            postNackReceivedChunks,
            nackRequestedChunks,
            retransmitReceivedChunks,
            retransmitDuplicatePackets,
            emergencyRepairReceivedChunks,
            emergencyRepairDuplicatePackets,
            lastPacketSequence,
            lastPacketChunkIndex,
            lastRetransmitSequence,
            lastRetransmitChunkIndex,
            eventPacketSequence,
            eventPacketChunkIndex,
            eventPacketWasRetransmit,
            eventPacketWasEmergencyRepair,
            recoveryExpireTimeUs,
            lastUpdateTimeUs,
            recentArrivalIntervalUs,
            likelyArrivalSuppressionCount,
            likelyArrivalWindowHit,
            likelyArrivalRecentHit,
            likelyArrivalTightCadenceHit,
            likelyArrivalFecParityHit,
            likelyArrivalFecRecoverableHit,
            keyTinyLastChanceNackSent,
            keyTinyLastChanceMissingChunks,
            keyTinyLastChanceSlackUs,
            sendTimeUs,
            firstReceiveTimeUs,
            ageMs);
    }

    void NetworkStats::OnDynamicNackDeadlineUpdated(
        uint64_t deadlineUs,
        double usefulnessRatio,
        double duplicateRatio,
        double expiredAfterRetransmitRatio,
        const char* reason
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.dynamicNackDeadlineMs =
            static_cast<double>(deadlineUs) / 1000.0;
        snapshot_.dynamicNackUsefulnessRatio = usefulnessRatio;
        snapshot_.dynamicNackDuplicateRatio = duplicateRatio;
        snapshot_.dynamicNackExpiredAfterRetransmitRatio =
            expiredAfterRetransmitRatio;
        snapshot_.dynamicNackDecisionReason =
            reason != nullptr ? reason : "unknown";
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnRetransmitLateAfterCompletedTiming(
        bool largeFrame,
        uint64_t packetSendTimeUs,
        uint64_t completedTimeUs,
        uint64_t receiveTimeUs
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (largeFrame) {
            snapshot_.retransmitLateAfterCompletedLargePackets++;
        }

        if (packetSendTimeUs != 0 &&
            completedTimeUs != 0) {
            if (packetSendTimeUs <= completedTimeUs) {
                snapshot_
                    .retransmitLateAfterCompletedSentBeforeCompletePackets++;
                const double sendToCompleteMs =
                    static_cast<double>(completedTimeUs - packetSendTimeUs) /
                    1000.0;
                retransmitLateAfterCompletedSendToCompleteSumMs_ +=
                    sendToCompleteMs;
                retransmitLateAfterCompletedSendToCompleteSamples_++;
                snapshot_.retransmitLateAfterCompletedAvgSendToCompleteMs =
                    retransmitLateAfterCompletedSendToCompleteSumMs_ /
                    static_cast<double>(
                        retransmitLateAfterCompletedSendToCompleteSamples_);
            }
            else {
                snapshot_
                    .retransmitLateAfterCompletedSentAfterCompletePackets++;
            }
        }

        if (completedTimeUs != 0 && receiveTimeUs >= completedTimeUs) {
            const double delayMs =
                static_cast<double>(receiveTimeUs - completedTimeUs) /
                1000.0;
            retransmitLateAfterCompletedDelaySumMs_ += delayMs;
            retransmitLateAfterCompletedDelaySamples_++;
            snapshot_.retransmitLateAfterCompletedAvgDelayMs =
                retransmitLateAfterCompletedDelaySumMs_ /
                static_cast<double>(
                    retransmitLateAfterCompletedDelaySamples_);
            snapshot_.retransmitLateAfterCompletedMaxDelayMs =
                (std::max)(
                    snapshot_.retransmitLateAfterCompletedMaxDelayMs,
                    delayMs);
        }

        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnNackSuppressed(
        const char* reason,
        uint32_t missingChunkCount
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::string reasonText =
            reason != nullptr ? reason : "unknown";
        const bool chunkOnlySuppression =
            reasonText == "preflight-shrunk";
        if (!chunkOnlySuppression) {
            snapshot_.nackSuppressedFrames++;
        }
        snapshot_.nackSuppressedMissingChunks += missingChunkCount;
        if (reasonText.rfind("preflight", 0) == 0) {
            if (!chunkOnlySuppression) {
                snapshot_.nackPreflightSuppressedFrames++;
            }
            snapshot_.nackPreflightSuppressedChunks += missingChunkCount;
        }
        else if (reasonText == "fec-grace") {
            snapshot_.nackFecGraceSuppressedFrames++;
            snapshot_.nackFecGraceSuppressedChunks += missingChunkCount;
        }
        snapshot_.nackLastSuppressionReason = reasonText;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnNackShapingDecision(
        const char* reason,
        uint32_t missingChunkCount,
        uint32_t requestedChunkBudget,
        bool sent
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::string reasonText =
            reason != nullptr ? reason : "unknown";
        if (sent || reasonText == "predicted-useful") {
            snapshot_.nackPredictedUsefulFrames++;
            snapshot_.nackPredictedUsefulChunks += missingChunkCount;
        }
        else if (reasonText == "deferred-likely-arrival") {
            snapshot_.nackDeferredForLikelyArrivalFrames++;
            snapshot_.nackDeferredForLikelyArrivalChunks += missingChunkCount;
        }
        else if (reasonText == "skipped-too-late") {
            snapshot_.nackSkippedTooLateFrames++;
            snapshot_.nackSkippedTooLateChunks += missingChunkCount;
        }
        snapshot_.nackRequestedChunkBudget += requestedChunkBudget;
        snapshot_.nackLastShapingReason = reasonText;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnDuplicatePacket(DuplicatePacketKind kind) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.duplicatePackets++;
        switch (kind) {
        case DuplicatePacketKind::Original:
            snapshot_.duplicateOriginalPackets++;
            break;
        case DuplicatePacketKind::Retransmit:
            snapshot_.duplicateRetransmitPackets++;
            break;
        case DuplicatePacketKind::LateOriginalAfterCompleted:
            snapshot_.duplicateLateAfterCompletedPackets++;
            snapshot_.duplicateLateAfterCompletedOriginalPackets++;
            break;
        case DuplicatePacketKind::LateRetransmitAfterCompleted:
            snapshot_.duplicateLateAfterCompletedPackets++;
            snapshot_.duplicateLateAfterCompletedRetransmitPackets++;
            break;
        case DuplicatePacketKind::LateAfterExpired:
            snapshot_.duplicateLateAfterExpiredPackets++;
            break;
        case DuplicatePacketKind::LateAfterRejected:
            snapshot_.duplicateLateAfterRejectedPackets++;
            break;
        }
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnReorderedPacket() {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.reorderedPackets++;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnMissingPackets(uint64_t missingCount) {
        if (missingCount == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.missingPackets += missingCount;
        snapshot_.sequenceGapPackets += missingCount;

        const uint64_t totalObservedPackets =
            snapshot_.receivedPackets + snapshot_.missingPackets;

        if (totalObservedPackets > 0) {
            snapshot_.packetLossRate =
                static_cast<double>(snapshot_.missingPackets) /
                static_cast<double>(totalObservedPackets);
        }

        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnMissingPacketsRecovered(uint64_t recoveredCount) {
        if (recoveredCount == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        const uint64_t applied =
            (std::min)(snapshot_.missingPackets, recoveredCount);
        if (applied == 0) {
            return;
        }

        snapshot_.missingPackets -= applied;
        snapshot_.sequenceGapRecoveredPackets += applied;

        const uint64_t totalObservedPackets =
            snapshot_.receivedPackets + snapshot_.missingPackets;

        snapshot_.packetLossRate =
            totalObservedPackets > 0
            ? static_cast<double>(snapshot_.missingPackets) /
                static_cast<double>(totalObservedPackets)
            : 0.0;

        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnJitterBufferUpdated(
        uint32_t bufferedFrames,
        uint32_t targetDelayMs
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.jitterBufferBufferedFrames = bufferedFrames;
        snapshot_.jitterBufferTargetDelayMs = targetDelayMs;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnJitterBufferReleased() {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.jitterBufferReleasedFrames++;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnJitterBufferDropped(uint32_t droppedFrames) {
        if (droppedFrames == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.jitterBufferDroppedFrames += droppedFrames;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnJitterBufferAutoModeUpdated(
        bool enabled,
        uint32_t calculatedDelayMs
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.jitterBufferAutoModeEnabled = enabled;
        snapshot_.jitterBufferAutoCalculatedDelayMs = calculatedDelayMs;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnDecodeFrame() {
        std::lock_guard<std::mutex> lock(mutex_);

        const uint64_t nowUs = NowMicroseconds();

        snapshot_.decodedFrames++;
        snapshot_.lastUpdateTimeUs = nowUs;

        UpdateDecodeFps(nowUs);
    }

    void NetworkStats::OnDisplayFrame() {
        std::lock_guard<std::mutex> lock(mutex_);

        const uint64_t nowUs = NowMicroseconds();

        snapshot_.displayedFrames++;
        snapshot_.lastUpdateTimeUs = nowUs;

        UpdateDisplayFps(nowUs);
    }

    void NetworkStats::OnH264ReceiverKeyFrameRequest(
        const char* reason,
        bool sent,
        bool syncRisk
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        const uint64_t nowUs = NowMicroseconds();
        const std::string reasonText =
            reason != nullptr ? reason : "unknown";
        const bool trueSyncLoss =
            syncRisk ||
            reasonText == "payload-header-failure" ||
            reasonText == "au-invalid" ||
            reasonText == "init-wait-idr" ||
            reasonText == "waiting-for-idr" ||
            reasonText == "decode-failure";

        if (sent) {
            snapshot_.h264KeyFrameRequestReceiverRequests++;
        }
        else {
            snapshot_.h264KeyFrameRequestReceiverCooldownSuppressed++;
            if (trueSyncLoss) {
                snapshot_.h264KeyFrameRequestReceiverCooldownSyncRisk++;
            }
            else {
                snapshot_.h264KeyFrameRequestReceiverCooldownNoise++;
            }
        }

        if (trueSyncLoss) {
            snapshot_.h264KeyFrameRequestReceiverTrueSyncLoss++;
        }

        if (reasonText == "missing-ack") {
            snapshot_.h264KeyFrameRequestReceiverMissingAck++;
        }
        else if (reasonText == "deadline-expired") {
            snapshot_.h264KeyFrameRequestReceiverDeadlineExpired++;
        }
        else if (reasonText == "deadline-nack-missing") {
            snapshot_.h264KeyFrameRequestReceiverDeadlineNackMissing++;
            if (!sent) {
                if (trueSyncLoss) {
                    snapshot_
                        .h264KeyFrameRequestReceiverDeadlineNackMissingCooldownSyncRisk++;
                }
                else {
                    snapshot_
                        .h264KeyFrameRequestReceiverDeadlineNackMissingCooldownNoise++;
                }
            }
        }
        else if (reasonText == "payload-header-failure") {
            snapshot_.h264KeyFrameRequestReceiverPayloadHeaderFailure++;
        }
        else if (reasonText == "au-invalid") {
            snapshot_.h264KeyFrameRequestReceiverAuInvalid++;
        }
        else if (reasonText == "init-wait-idr") {
            snapshot_.h264KeyFrameRequestReceiverInitWaitIdr++;
        }
        else if (reasonText == "waiting-for-idr") {
            snapshot_.h264KeyFrameRequestReceiverWaitingForIdr++;
        }
        else if (reasonText == "decode-failure") {
            snapshot_.h264KeyFrameRequestReceiverDecodeFailure++;
        }
        else if (reasonText == "stale-after-decode") {
            snapshot_.h264KeyFrameRequestReceiverStaleAfterDecode++;
        }

        snapshot_.h264KeyFrameRequestReceiverLastReason =
            sent
            ? reasonText
            : reasonText +
                (trueSyncLoss
                    ? "-cooldown-sync-risk"
                    : "-cooldown-noise");
        snapshot_.lastUpdateTimeUs = nowUs;
    }

    NetworkStatsSnapshot NetworkStats::GetSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    uint64_t NetworkStats::NowMicroseconds() const {
        using namespace std::chrono;

        return static_cast<uint64_t>(
            duration_cast<microseconds>(
                steady_clock::now().time_since_epoch()
            ).count()
            );
    }

    void NetworkStats::UpdateReceiveFps(uint64_t nowUs) {
        const uint64_t elapsedUs = nowUs - lastFpsUpdateTimeUs_;

        if (elapsedUs < 500000) {
            return;
        }

        const uint64_t frameDelta =
            snapshot_.completedFrames - framesAtLastFpsUpdate_;

        const double elapsedSec =
            static_cast<double>(elapsedUs) / 1000000.0;

        snapshot_.receiveFps =
            static_cast<double>(frameDelta) / elapsedSec;

        framesAtLastFpsUpdate_ = snapshot_.completedFrames;
        lastFpsUpdateTimeUs_ = nowUs;
    }

    void NetworkStats::UpdateDecodeFps(uint64_t nowUs) {
        const uint64_t elapsedUs = nowUs - lastDecodeFpsUpdateTimeUs_;

        if (elapsedUs < 500000) {
            return;
        }

        const uint64_t frameDelta =
            snapshot_.decodedFrames - decodedFramesAtLastFpsUpdate_;

        const double elapsedSec =
            static_cast<double>(elapsedUs) / 1000000.0;

        snapshot_.decodeFps =
            static_cast<double>(frameDelta) / elapsedSec;

        decodedFramesAtLastFpsUpdate_ = snapshot_.decodedFrames;
        lastDecodeFpsUpdateTimeUs_ = nowUs;
    }

    void NetworkStats::UpdateDisplayFps(uint64_t nowUs) {
        const uint64_t elapsedUs = nowUs - lastDisplayFpsUpdateTimeUs_;

        if (elapsedUs < 500000) {
            return;
        }

        const uint64_t frameDelta =
            snapshot_.displayedFrames - displayedFramesAtLastFpsUpdate_;

        const double elapsedSec =
            static_cast<double>(elapsedUs) / 1000000.0;

        snapshot_.displayFps =
            static_cast<double>(frameDelta) / elapsedSec;

        displayedFramesAtLastFpsUpdate_ = snapshot_.displayedFrames;
        lastDisplayFpsUpdateTimeUs_ = nowUs;
    }

    void NetworkStats::UpdateThroughput(uint64_t nowUs) {
        const uint64_t elapsedUs = nowUs - startTimeUs_;

        if (elapsedUs == 0) {
            return;
        }

        const double elapsedSec =
            static_cast<double>(elapsedUs) / 1000000.0;

        const double bits =
            static_cast<double>(snapshot_.receivedBytes) * 8.0;

        snapshot_.throughputMbps =
            bits / elapsedSec / 1000.0 / 1000.0;

        bytesAtLastThroughputUpdate_ = snapshot_.receivedBytes;
    }

    void NetworkStats::UpdateBitrate(uint64_t nowUs) {
        const uint64_t elapsedUs = nowUs - startTimeUs_;

        if (elapsedUs == 0) {
            return;
        }

        const double elapsedSec =
            static_cast<double>(elapsedUs) / 1000000.0;

        const double bits =
            static_cast<double>(totalFrameBytes_) * 8.0;

        snapshot_.bitrateMbps =
            bits / elapsedSec / 1000.0 / 1000.0;

        frameBytesAtLastBitrateUpdate_ = totalFrameBytes_;
    }

} // namespace net
