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
        case AdaptiveDegradationCause::DecodeLoad:
            return "DecodeLoad";
        case AdaptiveDegradationCause::DisplayLoad:
            return "DisplayLoad";
        case AdaptiveDegradationCause::None:
        default:
            return "None";
        }
    }

    AdaptiveStreamingController::AdaptiveStreamingController() {
        Reset();
    }

    void AdaptiveStreamingController::Reset() {
        enabled_ = true;

        state_ = AdaptiveStreamingState{};
        state_.targetBitrateKbps = 6000;
        DeriveTargetsFromBitrate();

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
    }

    void AdaptiveStreamingController::SetEnabled(bool enabled) {
        enabled_ = enabled;
    }

    bool AdaptiveStreamingController::IsEnabled() const {
        return enabled_;
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

        if (!enabled_) {
            return;
        }

        if (cooldownSec_ > 0.0) {
            cooldownSec_ = (std::max)(0.0, cooldownSec_ - deltaTimeSec);
            return;
        }

        const bool hardQoeProblem = qoeScore >= 2.0;
        const bool moderateQoeProblem = qoeScore >= 1.0;
        const bool lossOnlyPressure =
            !moderateQoeProblem &&
            (input.ackMissingRate >= 0.03 ||
                input.packetLossRate >= 0.03 ||
                deadlineNackDelta > 0);

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
            displayHealthy;

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
            ApplyCauseSpecificDecrease(state_.lastDegradationCause, true);
            badTimeSec_ = 0.0;
            cooldownSec_ = 1.2;
        }
        else if (moderateQoeProblem && badTimeSec_ >= 1.8) {
            ApplyCauseSpecificDecrease(state_.lastDegradationCause, false);
            badTimeSec_ = 0.0;
            cooldownSec_ = 1.6;
        }
        else if (lossOnlyPressure && lossOnlyBadTimeSec_ >= 4.0) {
            ApplyCauseSpecificDecrease(
                AdaptiveDegradationCause::PacketLoss,
                false);
            lossOnlyBadTimeSec_ = 0.0;
            cooldownSec_ = 2.0;
        }
        else if (stableNetwork && stableTimeSec_ >= 3.0) {
            ApplyAdditiveIncrease(300);
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

    void AdaptiveStreamingController::ApplyMultiplicativeDecrease(double factor) {
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

    void AdaptiveStreamingController::ApplyCauseSpecificDecrease(
        AdaptiveDegradationCause cause,
        bool hardProblem
    ) {
        double factor = hardProblem ? 0.82 : 0.90;

        switch (cause) {
        case AdaptiveDegradationCause::PacketLoss:
            factor = hardProblem ? 0.84 : 0.90;
            break;
        case AdaptiveDegradationCause::Jitter:
            factor = hardProblem ? 0.90 : 0.95;
            break;
        case AdaptiveDegradationCause::Rtt:
            factor = hardProblem ? 0.82 : 0.88;
            break;
        case AdaptiveDegradationCause::DecodeLoad:
        case AdaptiveDegradationCause::DisplayLoad:
            factor = hardProblem ? 0.78 : 0.86;
            break;
        case AdaptiveDegradationCause::None:
        default:
            break;
        }

        ApplyMultiplicativeDecrease(factor);

        if (cause == AdaptiveDegradationCause::DecodeLoad ||
            cause == AdaptiveDegradationCause::DisplayLoad ||
            cause == AdaptiveDegradationCause::Rtt) {
            const int oldFps = state_.targetFps;
            const int fpsStep = hardProblem ? 4 : 2;
            state_.targetFps = ClampFps(state_.targetFps - fpsStep);
            state_.fpsChanged = state_.fpsChanged || oldFps != state_.targetFps;
        }
    }

    void AdaptiveStreamingController::ApplyAdditiveIncrease(int bitrateKbps) {
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

    AdaptiveDegradationCause AdaptiveStreamingController::DetermineDegradationCause(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineDropDelta,
        uint64_t outputQueueDropDelta,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta
    ) const {
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
        return (std::max)(kMinBitrateKbps, (std::min)(kMaxBitrateKbps, value));
    }

    int AdaptiveStreamingController::ClampWidth(int value) const {
        return (std::max)(160, (std::min)(320, value));
    }

    int AdaptiveStreamingController::ClampHeight(int value) const {
        return (std::max)(90, (std::min)(180, value));
    }

} // namespace net
