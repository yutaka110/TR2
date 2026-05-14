#include "AdaptiveStreamingController.h"

#include <algorithm>
#include <cmath>

namespace net {

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
        cooldownSec_ = 0.0;
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
        state_.lastRttMs = input.rttMs;
        state_.lastLatencyMs = input.latencyMs;

        if (!enabled_) {
            return;
        }

        if (cooldownSec_ > 0.0) {
            cooldownSec_ = (std::max)(0.0, cooldownSec_ - deltaTimeSec);
            return;
        }

        const bool lossDetected =
            input.ackMissingRate >= 0.02 ||
            input.packetLossRate >= 0.02;

        const bool delayCongestion =
            input.rttMs >= 150.0 ||
            input.latencyMs >= 150.0;

        const bool stableNetwork =
            input.ackMissingRate <= 0.005 &&
            input.packetLossRate <= 0.005 &&
            input.rttMs <= 80.0 &&
            input.latencyMs <= 80.0;

        if (lossDetected || delayCongestion) {
            badTimeSec_ += deltaTimeSec;
            stableTimeSec_ = 0.0;
        }
        else if (stableNetwork) {
            stableTimeSec_ += deltaTimeSec;
            badTimeSec_ = 0.0;
        }
        else {
            badTimeSec_ = 0.0;
            stableTimeSec_ = 0.0;
        }

        if (lossDetected && badTimeSec_ >= 0.3) {
            ApplyMultiplicativeDecrease(0.7);
            badTimeSec_ = 0.0;
            cooldownSec_ = 0.8;
        }
        else if (delayCongestion && badTimeSec_ >= 1.0) {
            ApplyMultiplicativeDecrease(0.85);
            badTimeSec_ = 0.0;
            cooldownSec_ = 1.0;
        }
        else if (stableNetwork && stableTimeSec_ >= 2.5) {
            ApplyAdditiveIncrease(300);
            stableTimeSec_ = 0.0;
            cooldownSec_ = 1.0;
        }
    }

    AdaptiveStreamingState AdaptiveStreamingController::GetState() const {
        return state_;
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
