#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace net {

    enum class AdaptiveDegradationCause {
        None,
        PacketLoss,
        Jitter,
        Rtt,
        DecodeLoad,
        DisplayLoad
    };

    const char* ToString(AdaptiveDegradationCause cause);

    enum class AdaptiveControlMode {
        FixedQuality = 0,
        LossReactive = 1,
        QoeDeadlineAdaptive = 2
    };

    const char* ToString(AdaptiveControlMode mode);

    struct AdaptiveStreamingInput {
        double ackMissingRate = 0.0;
        double packetLossRate = 0.0;
        double rttMs = 0.0;
        double latencyMs = 0.0;
        double jitterMs = 0.0;
        double receiveFps = 0.0;
        double decodeFps = 0.0;
        double displayFps = 0.0;
        uint64_t displayedFrames = 0;
        uint64_t deadlineDroppedFrames = 0;
        uint64_t outputQueueDroppedFrames = 0;
        uint64_t deadlineNackSentFrames = 0;
        uint64_t deadlineNackMissingChunks = 0;
        std::string lastOutputQueueDropReason;
    };

    struct AdaptiveStreamingState {
        int targetJpegQuality = 85;
        int targetFps = 30;
        int targetBitrateKbps = 6000;
        int targetWidth = 320;
        int targetHeight = 180;

        bool qualityChanged = false;
        bool fpsChanged = false;
        bool bitrateChanged = false;
        bool resolutionChanged = false;

        size_t lastRawFrameBytes = 0;
        size_t lastEncodedFrameBytes = 0;
        double lastCompressionRatio = 0.0;

        double lastAckMissingRate = 0.0;
        double lastPacketLossRate = 0.0;
        double lastRttMs = 0.0;
        double lastLatencyMs = 0.0;
        double lastJitterMs = 0.0;
        double lastReceiveFps = 0.0;
        double lastDecodeFps = 0.0;
        double lastDisplayFps = 0.0;
        double lastQoeScore = 0.0;
        AdaptiveDegradationCause lastDegradationCause =
            AdaptiveDegradationCause::None;
        AdaptiveControlMode controlMode =
            AdaptiveControlMode::QoeDeadlineAdaptive;
        uint64_t lastDeadlineDroppedFrames = 0;
        uint64_t lastOutputQueueDroppedFrames = 0;
        uint64_t lastDeadlineNackSentFrames = 0;
        uint64_t lastDeadlineNackMissingChunks = 0;
    };

    class AdaptiveStreamingController {
    public:
        AdaptiveStreamingController();

        void Reset();

        void Update(const AdaptiveStreamingInput& input, double deltaTimeSec);

        AdaptiveStreamingState GetState() const;

        void ReportEncodedFrame(size_t rawBytes, size_t encodedBytes);

        void SetEnabled(bool enabled);
        bool IsEnabled() const;
        void SetControlMode(AdaptiveControlMode mode);
        AdaptiveControlMode GetControlMode() const;

    private:
        void InitializeTargetsForMode();
        void UpdateLossReactive(
            const AdaptiveStreamingInput& input,
            double deltaTimeSec,
            uint64_t deadlineNackDelta,
            uint64_t deadlineNackMissingChunkDelta
        );
        void ApplyMultiplicativeDecrease(double factor);
        void ApplyCauseSpecificDecrease(
            AdaptiveDegradationCause cause,
            bool hardProblem
        );
        void ApplyAdditiveIncrease(int bitrateKbps);
        void DeriveTargetsFromBitrate();
        AdaptiveDegradationCause DetermineDegradationCause(
            const AdaptiveStreamingInput& input,
            uint64_t deadlineDropDelta,
            uint64_t outputQueueDropDelta,
            uint64_t deadlineNackDelta,
            uint64_t deadlineNackMissingChunkDelta
        ) const;
        double CalculateQoeScore(
            const AdaptiveStreamingInput& input,
            uint64_t deadlineDropDelta,
            uint64_t outputQueueDropDelta
        ) const;

        int ClampQuality(int value) const;
        int ClampFps(int value) const;
        int ClampBitrate(int value) const;
        int ClampWidth(int value) const;
        int ClampHeight(int value) const;

    private:
        bool enabled_ = true;
        AdaptiveControlMode controlMode_ =
            AdaptiveControlMode::QoeDeadlineAdaptive;

        AdaptiveStreamingState state_{};

        double stableTimeSec_ = 0.0;
        double badTimeSec_ = 0.0;
        double lossOnlyBadTimeSec_ = 0.0;
        double cooldownSec_ = 0.0;
        double observedTimeSec_ = 0.0;

        bool hasDropCounters_ = false;
        uint64_t lastDeadlineDroppedFrames_ = 0;
        uint64_t lastOutputQueueDroppedFrames_ = 0;
        bool hasNackCounters_ = false;
        uint64_t lastDeadlineNackSentFrames_ = 0;
        uint64_t lastDeadlineNackMissingChunks_ = 0;

        static constexpr int kMinQuality = 35;
        static constexpr int kMaxQuality = 95;

        static constexpr int kMinFps = 8;
        static constexpr int kMaxFps = 30;

        static constexpr int kMinBitrateKbps = 500;
        static constexpr int kMaxBitrateKbps = 12000;
    };

} // namespace net
