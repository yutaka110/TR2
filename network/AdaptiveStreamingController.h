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
        Bandwidth,
        DecodeLoad,
        DisplayLoad,
        FrameFreshness,
        RecoveryDeadline,
        PacingQueue
    };

    const char* ToString(AdaptiveDegradationCause cause);

    enum class AdaptiveControlMode {
        FixedQuality = 0,
        LossReactive = 1,
        QoeDeadlineAdaptive = 2
    };

    const char* ToString(AdaptiveControlMode mode);

    enum class CongestionControlMode {
        LossBased = 0,
        DelayBased = 1,
        Hybrid = 2
    };

    const char* ToString(CongestionControlMode mode);

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
        uint64_t deadlineNackExpiredDroppedFrames = 0;
        uint64_t deadlineNackExpiredAfterNackFrames = 0;
        uint64_t ackStaleDroppedFrames = 0;
        uint64_t ackKeyFrameRequests = 0;
        uint64_t receiveFreshnessDroppedFrames = 0;
        double receiveDecodeInputFrameAgeMs = 0.0;
        double receiveLatestDecodedFrameAgeMs = 0.0;
        double receiveFreshnessDropThresholdMs = 0.0;
        std::string lastOutputQueueDropReason;
        bool pacingEnabled = false;
        uint64_t pacingDeadlineDroppedPackets = 0;
        double pacingCurrentQueueDelayMs = 0.0;
        double pacingMaxQueueDelayMs = 0.0;
        bool networkConditionEnabled = false;
        bool networkExperimentActive = false;
        uint32_t estimatedBandwidthBps = 0;
        uint32_t deliveryRateBps = 0;
        double bandwidthQueueDelayMs = 0.0;
        double bandwidthRttTrendMs = 0.0;
        double bandwidthLossTrend = 0.0;
        double bandwidthJitterTrendMs = 0.0;
        uint64_t bandwidthFeedbackSamples = 0;
        bool fecEnabled = false;
        bool adaptiveFecEnabled = false;
        uint32_t fecGroupChunkCount = 0;
        uint64_t fecParityPackets = 0;
        uint64_t fecRecoveredFrames = 0;
        uint64_t fecRecoveredChunks = 0;
    };

    struct AdaptiveStreamingState {
        int targetJpegQuality = 85;
        int targetFps = 30;
        int targetBitrateKbps = 6000;
        int targetWidth = 640;
        int targetHeight = 360;
        int bandwidthCeilingKbps = 12000;

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
        int lastEstimatedBandwidthKbps = 0;
        int lastDeliveryRateKbps = 0;
        double lastBandwidthQueueDelayMs = 0.0;
        double lastBandwidthRttTrendMs = 0.0;
        double lastBandwidthLossTrend = 0.0;
        double lastBandwidthJitterTrendMs = 0.0;
        bool lastFecEnabled = false;
        bool lastAdaptiveFecEnabled = false;
        uint32_t lastFecGroupChunkCount = 0;
        uint64_t lastFecParityPackets = 0;
        uint64_t lastFecRecoveredFrames = 0;
        uint64_t lastFecRecoveredChunks = 0;
        uint64_t lastFecParityPacketDelta = 0;
        uint64_t lastFecRecoveredFrameDelta = 0;
        uint64_t lastFecRecoveredChunkDelta = 0;
        double lastFecRecoveryEfficiency = 0.0;
        bool lastFecRecoveryWorking = false;
        bool lastFecRecoveryGuardActive = false;
        bool lastAdaptiveFecG8ToG4Recovery = false;
        bool lastAdaptiveFecQualityHoldActive = false;
        bool lastAdaptiveFecQualityHoldCanceled = false;
        uint64_t lastDeadlineNackExpiredDroppedFrames = 0;
        uint64_t lastDeadlineNackExpiredAfterNackFrames = 0;
        uint64_t lastAckStaleDroppedFrames = 0;
        uint64_t lastAckKeyFrameRequests = 0;
        uint64_t lastReceiveFreshnessDroppedFrames = 0;
        double lastReceiveDecodeInputFrameAgeMs = 0.0;
        double lastReceiveLatestDecodedFrameAgeMs = 0.0;
        double lastReceiveFreshnessDropThresholdMs = 0.0;
        AdaptiveDegradationCause lastDegradationCause =
            AdaptiveDegradationCause::None;
        AdaptiveControlMode controlMode =
            AdaptiveControlMode::QoeDeadlineAdaptive;
        CongestionControlMode congestionControlMode =
            CongestionControlMode::Hybrid;
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
        void SetCongestionControlMode(CongestionControlMode mode);
        CongestionControlMode GetCongestionControlMode() const;

    private:
        void InitializeTargetsForMode();
        void UpdateLossReactive(
            const AdaptiveStreamingInput& input,
            double deltaTimeSec,
            uint64_t deadlineNackDelta,
            uint64_t deadlineNackMissingChunkDelta
        );
        void ApplyAimdMultiplicativeDecrease(double factor);
        void ApplyAimdDecrease(
            const AdaptiveStreamingInput& input,
            AdaptiveDegradationCause cause,
            bool hardProblem
        );
        void ApplyAimdBitrateOnlyDecrease(
            const AdaptiveStreamingInput& input,
            AdaptiveDegradationCause cause,
            bool hardProblem
        );
        void ApplyAdaptiveFecRecoveryQualityFloor(
            const AdaptiveStreamingInput& input
        );
        void ApplyAimdIncrease(int bitrateKbps);
        void DeriveTargetsFromBitrate();
        CongestionControlMode ResolveActiveCongestionControlMode() const;
        bool HasLossPressure(
            const AdaptiveStreamingInput& input,
            uint64_t deadlineNackDelta,
            uint64_t deadlineNackMissingChunkDelta
        ) const;
        double EffectiveAckMissingRate(
            const AdaptiveStreamingInput& input
        ) const;
        double EffectivePacketLossRate(
            const AdaptiveStreamingInput& input
        ) const;
        double EffectiveBandwidthLossTrend(
            const AdaptiveStreamingInput& input
        ) const;
        bool ShouldSuppressPacingDropForQuality(
            const AdaptiveStreamingInput& input
        ) const;
        bool HasPacingDropPressure(
            const AdaptiveStreamingInput& input,
            uint64_t pacingDeadlineDropDelta
        ) const;
        bool HasDelayPressure(const AdaptiveStreamingInput& input) const;
        bool HasCongestionPressure(
            const AdaptiveStreamingInput& input,
            uint64_t deadlineNackDelta,
            uint64_t deadlineNackMissingChunkDelta
        ) const;
        bool IsCongestionRecoveryAllowed(
            const AdaptiveStreamingInput& input,
            uint64_t deadlineNackDelta,
            uint64_t deadlineNackMissingChunkDelta
        ) const;
        double CalculateAimdDecreaseFactor(
            const AdaptiveStreamingInput& input,
            AdaptiveDegradationCause cause,
            bool hardProblem
        ) const;
        bool HasBandwidthEstimate(const AdaptiveStreamingInput& input) const;
        int CalculateBandwidthCeilingKbps(
            const AdaptiveStreamingInput& input
        ) const;
        bool HasBandwidthPressure(
            const AdaptiveStreamingInput& input
        ) const;
        bool HasBandwidthCongestionEvidence(
            const AdaptiveStreamingInput& input
        ) const;
        bool IsBandwidthRecoveryAllowed(
            const AdaptiveStreamingInput& input
        ) const;
        AdaptiveDegradationCause DetermineDegradationCause(
            const AdaptiveStreamingInput& input,
            uint64_t deadlineDropDelta,
            uint64_t outputQueueDropDelta,
            uint64_t deadlineNackDelta,
            uint64_t deadlineNackMissingChunkDelta,
            uint64_t recoveryDeadlineDropDelta,
            uint64_t retransmitStaleDropDelta,
            uint64_t freshnessDropDelta
        ) const;
        double CalculateQoeScore(
            const AdaptiveStreamingInput& input,
            uint64_t deadlineDropDelta,
            uint64_t outputQueueDropDelta,
            uint64_t recoveryDeadlineDropDelta,
            uint64_t retransmitStaleDropDelta,
            uint64_t freshnessDropDelta
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
        CongestionControlMode congestionControlMode_ =
            CongestionControlMode::Hybrid;

        AdaptiveStreamingState state_{};

        double stableTimeSec_ = 0.0;
        double badTimeSec_ = 0.0;
        double lossOnlyBadTimeSec_ = 0.0;
        double cooldownSec_ = 0.0;
        double observedTimeSec_ = 0.0;
        double degradationCauseHoldSec_ = 0.0;
        double qoeHoldSec_ = 0.0;
        double heldQoeScore_ = 0.0;
        AdaptiveDegradationCause heldDegradationCause_ =
            AdaptiveDegradationCause::None;

        bool hasDropCounters_ = false;
        uint64_t lastDeadlineDroppedFrames_ = 0;
        uint64_t lastOutputQueueDroppedFrames_ = 0;
        bool hasNackCounters_ = false;
        uint64_t lastDeadlineNackSentFrames_ = 0;
        uint64_t lastDeadlineNackMissingChunks_ = 0;
        uint64_t lastDeadlineNackExpiredDroppedFrames_ = 0;
        uint64_t lastDeadlineNackExpiredAfterNackFrames_ = 0;
        uint64_t lastAckStaleDroppedFrames_ = 0;
        uint64_t lastAckKeyFrameRequests_ = 0;
        bool hasFreshnessCounters_ = false;
        uint64_t lastReceiveFreshnessDroppedFrames_ = 0;
        bool hasPacingCounters_ = false;
        uint64_t lastPacingDeadlineDroppedPackets_ = 0;
        bool hasFecCounters_ = false;
        uint64_t lastFecParityPackets_ = 0;
        uint64_t lastFecRecoveredFrames_ = 0;
        uint64_t lastFecRecoveredChunks_ = 0;
        double fecRecoveryGuardSec_ = 0.0;
        double lastEffectiveFecRecoveryEfficiency_ = 0.0;
        double fecRecoveryEvidenceSec_ = 0.0;
        uint32_t recoveryDeadlineFallbackSamples_ = 0;
        uint32_t nackExpiredRisingSamples_ = 0;
        double nackExpiredGuardReleaseSec_ = 0.0;
        double postNackExpiredGuardRearmSec_ = 0.0;
        bool hasAdaptiveFecGroupSample_ = false;
        bool lastAdaptiveFecSampleEnabled_ = false;
        uint32_t lastAdaptiveFecSampleGroupChunkCount_ = 0;
        double postG8ToG4QualityHoldSec_ = 0.0;
        double fecBurstTailRecoverySec_ = 0.0;
        uint32_t nackExpiredBitrateOnlyDecreaseSamples_ = 0;
        uint32_t nackExpiredBitrateOnlyDecreaseBudget_ = 2;
        int activeBandwidthCeilingKbps_ = 12000;

        static constexpr int kMinQuality = 35;
        static constexpr int kMaxQuality = 95;

        static constexpr int kMinFps = 8;
        static constexpr int kMaxFps = 30;

        static constexpr int kMinBitrateKbps = 500;
        static constexpr int kMaxBitrateKbps = 12000;
    };

} // namespace net
