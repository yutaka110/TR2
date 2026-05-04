#include "AdaptiveStreamingController.h"

#include <algorithm>

namespace net {

    AdaptiveStreamingController::AdaptiveStreamingController() {
        Reset();
    }

    void AdaptiveStreamingController::Reset() {
        enabled_ = true;

        state_ = AdaptiveStreamingState{};
        state_.targetJpegQuality = 85;
        state_.targetFps = 30;
        state_.targetBitrateKbps = 6000;

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

        state_.lastAckMissingRate = input.ackMissingRate;
        state_.lastRttMs = input.rttMs;
        state_.lastLatencyMs = input.latencyMs;

        if (!enabled_) {
            return;
        }

        if (cooldownSec_ > 0.0) {
            cooldownSec_ -= deltaTimeSec;
            if (cooldownSec_ < 0.0) {
                cooldownSec_ = 0.0;
            }
            return;
        }

        const bool badNetwork =
            input.ackMissingRate >= 0.05 ||
            input.packetLossRate >= 0.05 ||
            input.rttMs >= 120.0 ||
            input.latencyMs >= 120.0;

        const bool goodNetwork =
            input.ackMissingRate <= 0.01 &&
            input.packetLossRate <= 0.01 &&
            input.rttMs <= 60.0 &&
            input.latencyMs <= 60.0;

        if (badNetwork) {
            badTimeSec_ += deltaTimeSec;
            stableTimeSec_ = 0.0;
        }
        else if (goodNetwork) {
            stableTimeSec_ += deltaTimeSec;
            badTimeSec_ = 0.0;
        }
        else {
            stableTimeSec_ = 0.0;
            badTimeSec_ = 0.0;
        }

        if (badTimeSec_ >= 1.0) {
            ApplyDegradePolicy(input);
            badTimeSec_ = 0.0;
            cooldownSec_ = 1.5;
        }
        else if (stableTimeSec_ >= 5.0) {
            ApplyRecoveryPolicy(input);
            stableTimeSec_ = 0.0;
            cooldownSec_ = 2.0;
        }
    }

    AdaptiveStreamingState AdaptiveStreamingController::GetState() const {
        return state_;
    }

    void AdaptiveStreamingController::ApplyDegradePolicy(
        const AdaptiveStreamingInput& input
    ) {
        const int oldQuality = state_.targetJpegQuality;
        const int oldFps = state_.targetFps;
        const int oldBitrate = state_.targetBitrateKbps;

        if (input.ackMissingRate >= 0.15 || input.packetLossRate >= 0.15) {
            state_.targetJpegQuality = ClampQuality(state_.targetJpegQuality - 15);
            state_.targetFps = ClampFps(state_.targetFps - 10);
            state_.targetBitrateKbps = ClampBitrate(state_.targetBitrateKbps - 2000);
        }
        else if (input.ackMissingRate >= 0.05 || input.packetLossRate >= 0.05) {
            state_.targetJpegQuality = ClampQuality(state_.targetJpegQuality - 8);
            state_.targetFps = ClampFps(state_.targetFps - 5);
            state_.targetBitrateKbps = ClampBitrate(state_.targetBitrateKbps - 1000);
        }
        else if (input.rttMs >= 120.0 || input.latencyMs >= 120.0) {
            state_.targetFps = ClampFps(state_.targetFps - 5);
            state_.targetBitrateKbps = ClampBitrate(state_.targetBitrateKbps - 800);
        }

        state_.qualityChanged = oldQuality != state_.targetJpegQuality;
        state_.fpsChanged = oldFps != state_.targetFps;
        state_.bitrateChanged = oldBitrate != state_.targetBitrateKbps;
    }

    void AdaptiveStreamingController::ApplyRecoveryPolicy(
        const AdaptiveStreamingInput& input
    ) {
        (void)input;

        const int oldQuality = state_.targetJpegQuality;
        const int oldFps = state_.targetFps;
        const int oldBitrate = state_.targetBitrateKbps;

        state_.targetJpegQuality = ClampQuality(state_.targetJpegQuality + 3);
        state_.targetFps = ClampFps(state_.targetFps + 2);
        state_.targetBitrateKbps = ClampBitrate(state_.targetBitrateKbps + 500);

        state_.qualityChanged = oldQuality != state_.targetJpegQuality;
        state_.fpsChanged = oldFps != state_.targetFps;
        state_.bitrateChanged = oldBitrate != state_.targetBitrateKbps;
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

} // namespace net