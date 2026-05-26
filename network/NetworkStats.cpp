#include "NetworkStats.h"

#include <algorithm>
#include <chrono>

namespace net {

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

    void NetworkStats::OnOutputQueueDropEvent(
        uint32_t droppedFrames,
        uint32_t queueSizeBeforeDrop,
        double oldestDroppedAgeMs,
        double newestFrameAgeMs,
        const char* reason
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

    void NetworkStats::OnDeadlineNackSent(uint32_t missingChunkCount) {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.deadlineNackSentFrames++;
        snapshot_.deadlineNackMissingChunks += missingChunkCount;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnDeadlineNackRecoveredFrame() {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.deadlineNackRecoveredFrames++;
        snapshot_.lastUpdateTimeUs = NowMicroseconds();
    }

    void NetworkStats::OnDuplicatePacket() {
        std::lock_guard<std::mutex> lock(mutex_);

        snapshot_.duplicatePackets++;
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

        const uint64_t totalObservedPackets =
            snapshot_.receivedPackets + snapshot_.missingPackets;

        if (totalObservedPackets > 0) {
            snapshot_.packetLossRate =
                static_cast<double>(snapshot_.missingPackets) /
                static_cast<double>(totalObservedPackets);
        }

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
