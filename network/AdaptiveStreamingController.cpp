#include "AdaptiveStreamingController.h"

#include <algorithm>
#include <cmath>

namespace net {

    const char* ToString(AdaptiveDegradationCause cause) {
        switch (cause) {
        case AdaptiveDegradationCause::PacketLoss:
            return "Loss";
        case AdaptiveDegradationCause::Jitter:
            return "Jitter";
        case AdaptiveDegradationCause::Rtt:
            return "RTT";
        case AdaptiveDegradationCause::Bandwidth:
            return "Bandwidth";
        case AdaptiveDegradationCause::DecodeLoad:
            return "DecodeLoad";
        case AdaptiveDegradationCause::DisplayLoad:
            return "DisplayLoad";
        case AdaptiveDegradationCause::None:
        default:
            return "None";
        }
    }

    const char* ToString(AdaptiveControlMode mode) {
        switch (mode) {
        case AdaptiveControlMode::FixedQuality:
            return "Fixed Quality";
        case AdaptiveControlMode::LossReactive:
            return "Loss Reactive";
        case AdaptiveControlMode::QoeDeadlineAdaptive:
        default:
            return "QoE/Deadline Adaptive";
        }
    }

    const char* ToString(CongestionControlMode mode) {
        switch (mode) {
        case CongestionControlMode::LossBased:
            return "Loss Based";
        case CongestionControlMode::DelayBased:
            return "Delay Based";
        case CongestionControlMode::Hybrid:
        default:
            return "Hybrid";
        }
    }

    AdaptiveStreamingController::AdaptiveStreamingController() {
        Reset();
    }

    void AdaptiveStreamingController::Reset() {
        state_ = AdaptiveStreamingState{};
        state_.controlMode = controlMode_;
        state_.congestionControlMode = ResolveActiveCongestionControlMode();
        InitializeTargetsForMode();

        state_.qualityChanged = false;
        state_.fpsChanged = false;
        state_.bitrateChanged = false;
        state_.resolutionChanged = false;

        stableTimeSec_ = 0.0;
        badTimeSec_ = 0.0;
        lossOnlyBadTimeSec_ = 0.0;
        cooldownSec_ = 0.0;
        observedTimeSec_ = 0.0;
        hasDropCounters_ = false;
        lastDeadlineDroppedFrames_ = 0;
        lastOutputQueueDroppedFrames_ = 0;
        hasNackCounters_ = false;
        lastDeadlineNackSentFrames_ = 0;
        lastDeadlineNackMissingChunks_ = 0;
        activeBandwidthCeilingKbps_ = kMaxBitrateKbps;
        state_.bandwidthCeilingKbps = activeBandwidthCeilingKbps_;
    }

    void AdaptiveStreamingController::SetEnabled(bool enabled) {
        enabled_ = enabled;
    }

    bool AdaptiveStreamingController::IsEnabled() const {
        return enabled_;
    }

    void AdaptiveStreamingController::SetControlMode(AdaptiveControlMode mode) {
        if (controlMode_ == mode) {
            return;
        }

        controlMode_ = mode;
        Reset();
    }

    AdaptiveControlMode AdaptiveStreamingController::GetControlMode() const {
        return controlMode_;
    }

    void AdaptiveStreamingController::SetCongestionControlMode(
        CongestionControlMode mode
    ) {
        if (congestionControlMode_ == mode) {
            return;
        }

        congestionControlMode_ = mode;
        Reset();
    }

    CongestionControlMode
    AdaptiveStreamingController::GetCongestionControlMode() const {
        return congestionControlMode_;
    }

    void AdaptiveStreamingController::Update(
        const AdaptiveStreamingInput& input,
        double deltaTimeSec
    ) {
        state_.qualityChanged = false;
        state_.fpsChanged = false;
        state_.bitrateChanged = false;
        state_.resolutionChanged = false;

        state_.lastAckMissingRate = input.ackMissingRate;
        state_.lastPacketLossRate = input.packetLossRate;
        state_.lastRttMs = input.rttMs;
        state_.lastLatencyMs = input.latencyMs;
        state_.lastJitterMs = input.jitterMs;
        state_.lastReceiveFps = input.receiveFps;
        state_.lastDecodeFps = input.decodeFps;
        state_.lastDisplayFps = input.displayFps;
        state_.lastDeadlineDroppedFrames = input.deadlineDroppedFrames;
        state_.lastOutputQueueDroppedFrames = input.outputQueueDroppedFrames;
        state_.lastDeadlineNackSentFrames = input.deadlineNackSentFrames;
        state_.lastDeadlineNackMissingChunks = input.deadlineNackMissingChunks;
        state_.lastEstimatedBandwidthKbps =
            static_cast<int>(input.estimatedBandwidthBps / 1000u);
        state_.lastDeliveryRateKbps =
            static_cast<int>(input.deliveryRateBps / 1000u);
        state_.lastBandwidthQueueDelayMs = input.bandwidthQueueDelayMs;
        state_.lastBandwidthRttTrendMs = input.bandwidthRttTrendMs;
        state_.lastBandwidthLossTrend = input.bandwidthLossTrend;
        state_.lastBandwidthJitterTrendMs = input.bandwidthJitterTrendMs;
        activeBandwidthCeilingKbps_ = CalculateBandwidthCeilingKbps(input);
        state_.bandwidthCeilingKbps = activeBandwidthCeilingKbps_;
        state_.controlMode = controlMode_;
        state_.congestionControlMode = ResolveActiveCongestionControlMode();

        observedTimeSec_ += (std::max)(0.0, deltaTimeSec);

        uint64_t deadlineDropDelta = 0;
        uint64_t outputQueueDropDelta = 0;
        uint64_t deadlineNackDelta = 0;
        uint64_t deadlineNackMissingChunkDelta = 0;

        if (hasDropCounters_) {
            if (input.deadlineDroppedFrames >= lastDeadlineDroppedFrames_) {
                deadlineDropDelta =
                    input.deadlineDroppedFrames - lastDeadlineDroppedFrames_;
            }
            if (input.outputQueueDroppedFrames >= lastOutputQueueDroppedFrames_) {
                outputQueueDropDelta =
                    input.outputQueueDroppedFrames - lastOutputQueueDroppedFrames_;
            }
        }

        hasDropCounters_ = true;
        lastDeadlineDroppedFrames_ = input.deadlineDroppedFrames;
        lastOutputQueueDroppedFrames_ = input.outputQueueDroppedFrames;

        if (hasNackCounters_) {
            if (input.deadlineNackSentFrames >= lastDeadlineNackSentFrames_) {
                deadlineNackDelta =
                    input.deadlineNackSentFrames - lastDeadlineNackSentFrames_;
            }
            if (input.deadlineNackMissingChunks >= lastDeadlineNackMissingChunks_) {
                deadlineNackMissingChunkDelta =
                    input.deadlineNackMissingChunks - lastDeadlineNackMissingChunks_;
            }
        }

        hasNackCounters_ = true;
        lastDeadlineNackSentFrames_ = input.deadlineNackSentFrames;
        lastDeadlineNackMissingChunks_ = input.deadlineNackMissingChunks;

        const double qoeScore =
            CalculateQoeScore(input, deadlineDropDelta, outputQueueDropDelta);
        state_.lastQoeScore = qoeScore;
        state_.lastDegradationCause =
            DetermineDegradationCause(
                input,
                deadlineDropDelta,
                outputQueueDropDelta,
                deadlineNackDelta,
                deadlineNackMissingChunkDelta);

        if (!enabled_ ||
            controlMode_ == AdaptiveControlMode::FixedQuality) {
            stableTimeSec_ = 0.0;
            badTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
            return;
        }

        if (controlMode_ == AdaptiveControlMode::LossReactive) {
            UpdateLossReactive(
                input,
                deltaTimeSec,
                deadlineNackDelta,
                deadlineNackMissingChunkDelta);
            return;
        }

        if (cooldownSec_ > 0.0) {
            cooldownSec_ = (std::max)(0.0, cooldownSec_ - deltaTimeSec);
            return;
        }

        const bool hardQoeProblem = qoeScore >= 2.0;
        const bool moderateQoeProblem = qoeScore >= 1.0;
        const bool congestionPressure =
            HasCongestionPressure(
                input,
                deadlineNackDelta,
                deadlineNackMissingChunkDelta);
        const bool bandwidthPressure = HasBandwidthPressure(input);
        const bool lossOnlyPressure =
            !moderateQoeProblem &&
            congestionPressure;

        const bool displayHealthy =
            input.displayedFrames < 10 ||
            input.displayFps <= 0.0 ||
            input.displayFps >=
            static_cast<double>(state_.targetFps) * 0.85;

        const bool stableNetwork =
            qoeScore <= 0.25 &&
            input.ackMissingRate <= 0.02 &&
            input.packetLossRate <= 0.02 &&
            input.rttMs <= 100.0 &&
            input.latencyMs <= 100.0 &&
            displayHealthy &&
            IsCongestionRecoveryAllowed(
                input,
                deadlineNackDelta,
                deadlineNackMissingChunkDelta);

        if (moderateQoeProblem) {
            badTimeSec_ += deltaTimeSec;
            lossOnlyBadTimeSec_ = 0.0;
            stableTimeSec_ = 0.0;
        }
        else if (lossOnlyPressure) {
            lossOnlyBadTimeSec_ += deltaTimeSec;
            badTimeSec_ = 0.0;
            stableTimeSec_ = 0.0;
        }
        else if (stableNetwork) {
            stableTimeSec_ += deltaTimeSec;
            badTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
        }
        else {
            badTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
            stableTimeSec_ = 0.0;
        }

        if (hardQoeProblem && badTimeSec_ >= 0.6) {
            ApplyAimdDecrease(input, state_.lastDegradationCause, true);
            badTimeSec_ = 0.0;
            cooldownSec_ = 1.2;
        }
        else if (moderateQoeProblem && badTimeSec_ >= 1.8) {
            ApplyAimdDecrease(input, state_.lastDegradationCause, false);
            badTimeSec_ = 0.0;
            cooldownSec_ = 1.6;
        }
        else if (congestionPressure && lossOnlyBadTimeSec_ >= 1.0) {
            ApplyAimdDecrease(
                input,
                state_.lastDegradationCause,
                false);
            lossOnlyBadTimeSec_ = 0.0;
            cooldownSec_ = 1.4;
        }
        else if (lossOnlyPressure && lossOnlyBadTimeSec_ >= 4.0) {
            ApplyAimdDecrease(
                input,
                bandwidthPressure
                ? AdaptiveDegradationCause::Bandwidth
                : AdaptiveDegradationCause::PacketLoss,
                false);
            lossOnlyBadTimeSec_ = 0.0;
            cooldownSec_ = 2.0;
        }
        else if (stableNetwork && stableTimeSec_ >= 3.0) {
            ApplyAimdIncrease(100);
            stableTimeSec_ = 0.0;
            cooldownSec_ = 1.2;
        }
    }

    AdaptiveStreamingState AdaptiveStreamingController::GetState() const {
        return state_;
    }

    void AdaptiveStreamingController::ReportEncodedFrame(
        size_t rawBytes,
        size_t encodedBytes
    ) {
        state_.lastRawFrameBytes = rawBytes;
        state_.lastEncodedFrameBytes = encodedBytes;

        if (rawBytes > 0 && encodedBytes > 0) {
            state_.lastCompressionRatio =
                static_cast<double>(encodedBytes) /
                static_cast<double>(rawBytes);
        }
        else {
            state_.lastCompressionRatio = 0.0;
        }
    }

    void AdaptiveStreamingController::InitializeTargetsForMode() {
        if (controlMode_ == AdaptiveControlMode::FixedQuality) {
            state_.targetJpegQuality = 85;
            state_.targetFps = 30;
            state_.targetBitrateKbps = 9500;
            state_.targetWidth = 320;
            state_.targetHeight = 180;
            return;
        }

        state_.targetBitrateKbps = 6000;
        DeriveTargetsFromBitrate();
    }

    void AdaptiveStreamingController::UpdateLossReactive(
        const AdaptiveStreamingInput& input,
        double deltaTimeSec,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta
    ) {
        if (cooldownSec_ > 0.0) {
            cooldownSec_ = (std::max)(0.0, cooldownSec_ - deltaTimeSec);
            return;
        }

        const bool lossPressure =
            HasLossPressure(input, deadlineNackDelta, deadlineNackMissingChunkDelta);

        const double lossRate = (std::max)(
            (std::max)(input.ackMissingRate, input.packetLossRate),
            input.bandwidthLossTrend
        );
        const bool hardLossPressure =
            lossRate >= 0.10 ||
            deadlineNackMissingChunkDelta >= 3;

        const bool stableLoss =
            input.ackMissingRate <= 0.02 &&
            input.packetLossRate <= 0.02 &&
            deadlineNackDelta == 0 &&
            deadlineNackMissingChunkDelta == 0 &&
            IsCongestionRecoveryAllowed(
                input,
                deadlineNackDelta,
                deadlineNackMissingChunkDelta);

        state_.lastDegradationCause =
            lossPressure
            ? AdaptiveDegradationCause::PacketLoss
            : AdaptiveDegradationCause::None;

        if (lossPressure) {
            lossOnlyBadTimeSec_ += deltaTimeSec;
            stableTimeSec_ = 0.0;
            badTimeSec_ = 0.0;
        }
        else if (stableLoss) {
            stableTimeSec_ += deltaTimeSec;
            lossOnlyBadTimeSec_ = 0.0;
            badTimeSec_ = 0.0;
        }
        else {
            stableTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
            badTimeSec_ = 0.0;
        }

        if (hardLossPressure && lossOnlyBadTimeSec_ >= 0.8) {
            ApplyAimdDecrease(
                input,
                AdaptiveDegradationCause::PacketLoss,
                true);
            lossOnlyBadTimeSec_ = 0.0;
            cooldownSec_ = 1.2;
        }
        else if (lossPressure && lossOnlyBadTimeSec_ >= 3.0) {
            ApplyAimdDecrease(
                input,
                AdaptiveDegradationCause::PacketLoss,
                false);
            lossOnlyBadTimeSec_ = 0.0;
            cooldownSec_ = 2.0;
        }
        else if (stableLoss && stableTimeSec_ >= 4.0) {
            ApplyAimdIncrease(100);
            stableTimeSec_ = 0.0;
            cooldownSec_ = 1.5;
        }
    }

    void AdaptiveStreamingController::ApplyAimdMultiplicativeDecrease(double factor) {
        const int oldQuality = state_.targetJpegQuality;
        const int oldFps = state_.targetFps;
        const int oldBitrate = state_.targetBitrateKbps;
        const int oldWidth = state_.targetWidth;
        const int oldHeight = state_.targetHeight;

        const int nextBitrate = static_cast<int>(
            std::lround(static_cast<double>(state_.targetBitrateKbps) * factor)
        );
        state_.targetBitrateKbps = ClampBitrate(nextBitrate);
        DeriveTargetsFromBitrate();

        state_.qualityChanged = oldQuality != state_.targetJpegQuality;
        state_.fpsChanged = oldFps != state_.targetFps;
        state_.bitrateChanged = oldBitrate != state_.targetBitrateKbps;
        state_.resolutionChanged =
            oldWidth != state_.targetWidth ||
            oldHeight != state_.targetHeight;
    }

    void AdaptiveStreamingController::ApplyAimdDecrease(
        const AdaptiveStreamingInput& input,
        AdaptiveDegradationCause cause,
        bool hardProblem
    ) {
        const double factor =
            CalculateAimdDecreaseFactor(input, cause, hardProblem);

        ApplyAimdMultiplicativeDecrease(factor);

        if (cause == AdaptiveDegradationCause::DecodeLoad ||
            cause == AdaptiveDegradationCause::DisplayLoad ||
            cause == AdaptiveDegradationCause::Rtt) {
            const int oldFps = state_.targetFps;
            const int fpsStep = hardProblem ? 4 : 2;
            state_.targetFps = ClampFps(state_.targetFps - fpsStep);
            state_.fpsChanged = state_.fpsChanged || oldFps != state_.targetFps;
        }
    }

    void AdaptiveStreamingController::ApplyAimdIncrease(int bitrateKbps) {
        const int oldQuality = state_.targetJpegQuality;
        const int oldFps = state_.targetFps;
        const int oldBitrate = state_.targetBitrateKbps;
        const int oldWidth = state_.targetWidth;
        const int oldHeight = state_.targetHeight;

        state_.targetBitrateKbps =
            ClampBitrate(state_.targetBitrateKbps + bitrateKbps);
        DeriveTargetsFromBitrate();

        state_.qualityChanged = oldQuality != state_.targetJpegQuality;
        state_.fpsChanged = oldFps != state_.targetFps;
        state_.bitrateChanged = oldBitrate != state_.targetBitrateKbps;
        state_.resolutionChanged =
            oldWidth != state_.targetWidth ||
            oldHeight != state_.targetHeight;
    }

    void AdaptiveStreamingController::DeriveTargetsFromBitrate() {
        const int bitrate = state_.targetBitrateKbps;

        if (bitrate <= 900) {
            state_.targetWidth = 160;
            state_.targetHeight = 90;
            state_.targetFps = 8;
            state_.targetJpegQuality = 40;
        }
        else if (bitrate <= 1400) {
            state_.targetWidth = 160;
            state_.targetHeight = 90;
            state_.targetFps = 10;
            state_.targetJpegQuality = 45;
        }
        else if (bitrate <= 2200) {
            state_.targetWidth = 214;
            state_.targetHeight = 120;
            state_.targetFps = 12;
            state_.targetJpegQuality = 55;
        }
        else if (bitrate <= 3500) {
            state_.targetWidth = 256;
            state_.targetHeight = 144;
            state_.targetFps = 15;
            state_.targetJpegQuality = 65;
        }
        else if (bitrate <= 5000) {
            state_.targetWidth = 320;
            state_.targetHeight = 180;
            state_.targetFps = 15;
            state_.targetJpegQuality = 70;
        }
        else if (bitrate <= 7500) {
            state_.targetWidth = 320;
            state_.targetHeight = 180;
            state_.targetFps = 20;
            state_.targetJpegQuality = 78;
        }
        else if (bitrate <= 9500) {
            state_.targetWidth = 320;
            state_.targetHeight = 180;
            state_.targetFps = 24;
            state_.targetJpegQuality = 85;
        }
        else {
            state_.targetWidth = 320;
            state_.targetHeight = 180;
            state_.targetFps = 30;
            state_.targetJpegQuality = 90;
        }

        state_.targetWidth = ClampWidth(state_.targetWidth);
        state_.targetHeight = ClampHeight(state_.targetHeight);
        state_.targetFps = ClampFps(state_.targetFps);
        state_.targetJpegQuality = ClampQuality(state_.targetJpegQuality);
    }

    CongestionControlMode
    AdaptiveStreamingController::ResolveActiveCongestionControlMode() const {
        if (controlMode_ == AdaptiveControlMode::LossReactive) {
            return CongestionControlMode::LossBased;
        }

        return congestionControlMode_;
    }

    bool AdaptiveStreamingController::HasLossPressure(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta
    ) const {
        const double lossRate = (std::max)(
            (std::max)(input.ackMissingRate, input.packetLossRate),
            input.bandwidthLossTrend
        );

        return lossRate >= 0.03 ||
            deadlineNackDelta > 0 ||
            deadlineNackMissingChunkDelta > 0;
    }

    bool AdaptiveStreamingController::HasDelayPressure(
        const AdaptiveStreamingInput& input
    ) const {
        return input.bandwidthQueueDelayMs >= 30.0 ||
            input.bandwidthRttTrendMs >= 10.0 ||
            input.bandwidthJitterTrendMs >= 20.0 ||
            input.rttMs >= 140.0 ||
            input.jitterMs >= 35.0 ||
            input.latencyMs >= 130.0;
    }

    bool AdaptiveStreamingController::HasCongestionPressure(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta
    ) const {
        const CongestionControlMode mode =
            ResolveActiveCongestionControlMode();
        const bool lossPressure =
            HasLossPressure(input, deadlineNackDelta, deadlineNackMissingChunkDelta);
        const bool delayPressure = HasDelayPressure(input);
        const bool bandwidthPressure = HasBandwidthPressure(input);

        switch (mode) {
        case CongestionControlMode::LossBased:
            return lossPressure;
        case CongestionControlMode::DelayBased:
            return delayPressure || bandwidthPressure;
        case CongestionControlMode::Hybrid:
        default:
            return lossPressure || delayPressure || bandwidthPressure;
        }
    }

    bool AdaptiveStreamingController::IsCongestionRecoveryAllowed(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta
    ) const {
        const bool lossStable =
            input.ackMissingRate <= 0.02 &&
            input.packetLossRate <= 0.02 &&
            input.bandwidthLossTrend <= 0.015 &&
            deadlineNackDelta == 0 &&
            deadlineNackMissingChunkDelta == 0;
        const bool delayStable =
            input.rttMs <= 100.0 &&
            input.latencyMs <= 100.0 &&
            input.jitterMs <= 20.0 &&
            input.bandwidthQueueDelayMs <= 10.0 &&
            input.bandwidthRttTrendMs <= 4.0 &&
            input.bandwidthJitterTrendMs <= 8.0;

        switch (ResolveActiveCongestionControlMode()) {
        case CongestionControlMode::LossBased:
            return lossStable && IsBandwidthRecoveryAllowed(input);
        case CongestionControlMode::DelayBased:
            return delayStable && IsBandwidthRecoveryAllowed(input);
        case CongestionControlMode::Hybrid:
        default:
            return lossStable && delayStable && IsBandwidthRecoveryAllowed(input);
        }
    }

    double AdaptiveStreamingController::CalculateAimdDecreaseFactor(
        const AdaptiveStreamingInput& input,
        AdaptiveDegradationCause cause,
        bool hardProblem
    ) const {
        const double lossRate = (std::max)(
            (std::max)(input.ackMissingRate, input.packetLossRate),
            input.bandwidthLossTrend
        );
        const double queueDelayMs = input.bandwidthQueueDelayMs;

        switch (cause) {
        case AdaptiveDegradationCause::PacketLoss:
            if (lossRate > 0.20) {
                return 0.50;
            }
            if (lossRate > 0.10) {
                return 0.70;
            }
            if (lossRate > 0.05) {
                return 0.85;
            }
            return hardProblem ? 0.84 : 0.90;
        case AdaptiveDegradationCause::Bandwidth:
        case AdaptiveDegradationCause::Rtt:
        case AdaptiveDegradationCause::Jitter:
            if (queueDelayMs > 80.0 ||
                input.bandwidthRttTrendMs > 30.0) {
                return 0.70;
            }
            if (queueDelayMs > 30.0 ||
                input.bandwidthRttTrendMs > 10.0 ||
                input.bandwidthJitterTrendMs > 20.0) {
                return 0.85;
            }
            return hardProblem ? 0.82 : 0.90;
        case AdaptiveDegradationCause::DecodeLoad:
        case AdaptiveDegradationCause::DisplayLoad:
            return hardProblem ? 0.78 : 0.86;
        case AdaptiveDegradationCause::None:
        default:
            return hardProblem ? 0.82 : 0.90;
        }
    }

    bool AdaptiveStreamingController::HasBandwidthEstimate(
        const AdaptiveStreamingInput& input
    ) const {
        return input.bandwidthFeedbackSamples >= 128 &&
            input.estimatedBandwidthBps >=
            static_cast<uint32_t>(kMinBitrateKbps * 1000) &&
            input.deliveryRateBps > 0;
    }

    int AdaptiveStreamingController::CalculateBandwidthCeilingKbps(
        const AdaptiveStreamingInput& input
    ) const {
        if (controlMode_ == AdaptiveControlMode::FixedQuality ||
            !HasBandwidthEstimate(input)) {
            return kMaxBitrateKbps;
        }

        const double estimatedKbps =
            static_cast<double>(input.estimatedBandwidthBps) / 1000.0;
        const double safetyMargin =
            input.bandwidthQueueDelayMs >= 8.0 ||
            input.bandwidthLossTrend >= 0.04 ||
            input.bandwidthRttTrendMs >= 10.0
            ? 0.80
            : 0.90;

        const int ceilingKbps = static_cast<int>(
            std::lround(estimatedKbps * safetyMargin)
        );

        return (std::max)(
            kMinBitrateKbps,
            (std::min)(kMaxBitrateKbps, ceilingKbps)
        );
    }

    bool AdaptiveStreamingController::HasBandwidthPressure(
        const AdaptiveStreamingInput& input
    ) const {
        if (controlMode_ == AdaptiveControlMode::FixedQuality ||
            !HasBandwidthEstimate(input)) {
            return false;
        }

        const int ceilingKbps = CalculateBandwidthCeilingKbps(input);
        if (state_.targetBitrateKbps <= ceilingKbps + 300) {
            return false;
        }

        const double deliveryKbps =
            static_cast<double>(input.deliveryRateBps) / 1000.0;
        const bool deliveryBelowTarget =
            deliveryKbps > 0.0 &&
            deliveryKbps <
            static_cast<double>(state_.targetBitrateKbps) * 0.75;

        return deliveryBelowTarget ||
            input.bandwidthQueueDelayMs >= 3.0 ||
            input.bandwidthLossTrend >= 0.02 ||
            input.bandwidthRttTrendMs >= 6.0 ||
            input.bandwidthJitterTrendMs >= 6.0;
    }

    bool AdaptiveStreamingController::IsBandwidthRecoveryAllowed(
        const AdaptiveStreamingInput& input
    ) const {
        if (controlMode_ == AdaptiveControlMode::FixedQuality ||
            !HasBandwidthEstimate(input)) {
            return true;
        }

        const int ceilingKbps = CalculateBandwidthCeilingKbps(input);
        const bool hasHeadroom =
            ceilingKbps >= state_.targetBitrateKbps + 300;
        const bool estimatorStable =
            input.bandwidthQueueDelayMs <= 2.0 &&
            input.bandwidthLossTrend <= 0.015 &&
            input.bandwidthRttTrendMs <= 4.0 &&
            input.bandwidthJitterTrendMs <= 5.0;

        return hasHeadroom && estimatorStable;
    }

    AdaptiveDegradationCause AdaptiveStreamingController::DetermineDegradationCause(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineDropDelta,
        uint64_t outputQueueDropDelta,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta
    ) const {
        const CongestionControlMode congestionMode =
            ResolveActiveCongestionControlMode();

        if (congestionMode == CongestionControlMode::LossBased) {
            return HasLossPressure(
                input,
                deadlineNackDelta,
                deadlineNackMissingChunkDelta)
                ? AdaptiveDegradationCause::PacketLoss
                : AdaptiveDegradationCause::None;
        }

        if (congestionMode == CongestionControlMode::DelayBased) {
            if (HasBandwidthPressure(input)) {
                return AdaptiveDegradationCause::Bandwidth;
            }
            if (input.rttMs >= 140.0 ||
                input.latencyMs >= 130.0 ||
                input.bandwidthRttTrendMs >= 10.0) {
                return AdaptiveDegradationCause::Rtt;
            }
            if (input.jitterMs >= 35.0 ||
                input.bandwidthJitterTrendMs >= 20.0 ||
                input.bandwidthQueueDelayMs >= 30.0) {
                return AdaptiveDegradationCause::Jitter;
            }

            return AdaptiveDegradationCause::None;
        }

        const bool enoughDecodeData =
            observedTimeSec_ >= 2.0 &&
            input.receiveFps >= 5.0 &&
            input.decodeFps > 0.0;

        const bool enoughDisplayData =
            observedTimeSec_ >= 2.0 &&
            input.displayedFrames >= 10 &&
            input.decodeFps >= 5.0 &&
            input.displayFps > 0.0;

        const bool rendererLag =
            input.lastOutputQueueDropReason == "renderer-lag";
        const bool jitterBurst =
            input.lastOutputQueueDropReason == "jitter-burst-release";

        if (rendererLag ||
            (enoughDisplayData &&
                input.displayFps < input.decodeFps * 0.75)) {
            return AdaptiveDegradationCause::DisplayLoad;
        }

        if (enoughDecodeData &&
            input.decodeFps < input.receiveFps * 0.75) {
            return AdaptiveDegradationCause::DecodeLoad;
        }

        if (input.rttMs >= 140.0 ||
            (input.latencyMs >= 130.0 && input.rttMs >= 100.0)) {
            return AdaptiveDegradationCause::Rtt;
        }

        if (HasBandwidthPressure(input)) {
            return AdaptiveDegradationCause::Bandwidth;
        }

        if (deadlineNackDelta >= 3 ||
            deadlineNackMissingChunkDelta >= 3 ||
            ((input.ackMissingRate >= 0.03 || input.packetLossRate >= 0.08) &&
                input.jitterMs < 35.0)) {
            return AdaptiveDegradationCause::PacketLoss;
        }

        if (input.jitterMs >= 35.0 ||
            jitterBurst ||
            (outputQueueDropDelta > 0 && input.jitterMs >= 15.0)) {
            return AdaptiveDegradationCause::Jitter;
        }

        if (input.ackMissingRate >= 0.03 ||
            input.packetLossRate >= 0.03 ||
            deadlineNackDelta > 0 ||
            deadlineNackMissingChunkDelta > 0 ||
            deadlineDropDelta > 0) {
            return AdaptiveDegradationCause::PacketLoss;
        }

        return AdaptiveDegradationCause::None;
    }

    double AdaptiveStreamingController::CalculateQoeScore(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineDropDelta,
        uint64_t outputQueueDropDelta
    ) const {
        double score = 0.0;

        if (deadlineDropDelta > 0 || outputQueueDropDelta > 0) {
            score = (std::max)(score, 3.0);
        }

        if (input.latencyMs >= 150.0 || input.rttMs >= 220.0) {
            score = (std::max)(score, 3.0);
        }
        else if (input.latencyMs >= 130.0 || input.rttMs >= 180.0) {
            score = (std::max)(score, 2.0);
        }
        else if (input.latencyMs >= 110.0 || input.rttMs >= 140.0) {
            score = (std::max)(score, 1.0);
        }

        const bool displayFpsReady =
            observedTimeSec_ >= 2.0 &&
            input.displayedFrames >= 10 &&
            input.displayFps > 0.0 &&
            state_.targetFps > 0;

        if (displayFpsReady) {
            const double displayRatio =
                input.displayFps / static_cast<double>(state_.targetFps);

            if (displayRatio < 0.60) {
                score = (std::max)(score, 2.0);
            }
            else if (displayRatio < 0.75) {
                score = (std::max)(score, 1.0);
            }
        }

        const bool decodeFpsReady =
            observedTimeSec_ >= 2.0 &&
            input.receiveFps >= 5.0 &&
            input.decodeFps > 0.0;

        if (decodeFpsReady) {
            const double decodeRatio = input.decodeFps / input.receiveFps;
            if (decodeRatio < 0.60) {
                score = (std::max)(score, 2.0);
            }
            else if (decodeRatio < 0.75) {
                score = (std::max)(score, 1.0);
            }
        }

        const bool displayVsDecodeReady =
            observedTimeSec_ >= 2.0 &&
            input.displayedFrames >= 10 &&
            input.decodeFps >= 5.0 &&
            input.displayFps > 0.0;

        if (displayVsDecodeReady) {
            const double displayDecodeRatio =
                input.displayFps / input.decodeFps;
            if (displayDecodeRatio < 0.60) {
                score = (std::max)(score, 2.0);
            }
            else if (displayDecodeRatio < 0.75) {
                score = (std::max)(score, 1.0);
            }
        }

        if (input.jitterMs >= 50.0) {
            score = (std::max)(score, 2.0);
        }
        else if (input.jitterMs >= 30.0) {
            score = (std::max)(score, 1.0);
        }

        if (input.ackMissingRate >= 0.08 || input.packetLossRate >= 0.08) {
            score = (std::max)(score, 0.75);
        }
        else if (input.ackMissingRate >= 0.03 || input.packetLossRate >= 0.03) {
            score = (std::max)(score, 0.5);
        }

        return score;
    }

    int AdaptiveStreamingController::ClampQuality(int value) const {
        return (std::max)(kMinQuality, (std::min)(kMaxQuality, value));
    }

    int AdaptiveStreamingController::ClampFps(int value) const {
        return (std::max)(kMinFps, (std::min)(kMaxFps, value));
    }

    int AdaptiveStreamingController::ClampBitrate(int value) const {
        const int ceiling = (std::max)(
            kMinBitrateKbps,
            (std::min)(kMaxBitrateKbps, activeBandwidthCeilingKbps_)
        );
        return (std::max)(kMinBitrateKbps, (std::min)(ceiling, value));
    }

    int AdaptiveStreamingController::ClampWidth(int value) const {
        return (std::max)(160, (std::min)(320, value));
    }

    int AdaptiveStreamingController::ClampHeight(int value) const {
        return (std::max)(90, (std::min)(180, value));
    }

} // namespace net
