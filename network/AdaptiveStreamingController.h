#pragma once

#include <cstdint>

namespace net {

    struct AdaptiveStreamingInput {
        double ackMissingRate = 0.0;   // 0.0 ～ 1.0
        double packetLossRate = 0.0;   // 0.0 ～ 1.0
        double rttMs = 0.0;
        double latencyMs = 0.0;
    };

    struct AdaptiveStreamingState {
        int targetJpegQuality = 85;    // 1 ～ 100
        int targetFps = 30;            // 5 ～ 60
        int targetBitrateKbps = 6000;  // 将来H.264用

        bool qualityChanged = false;
        bool fpsChanged = false;
        bool bitrateChanged = false;

        double lastAckMissingRate = 0.0;
        double lastRttMs = 0.0;
        double lastLatencyMs = 0.0;
    };

    class AdaptiveStreamingController {
    public:
        AdaptiveStreamingController();

        void Reset();

        void Update(const AdaptiveStreamingInput& input, double deltaTimeSec);

        AdaptiveStreamingState GetState() const;

        void SetEnabled(bool enabled);
        bool IsEnabled() const;

    private:
        void ApplyDegradePolicy(const AdaptiveStreamingInput& input);
        void ApplyRecoveryPolicy(const AdaptiveStreamingInput& input);

        int ClampQuality(int value) const;
        int ClampFps(int value) const;
        int ClampBitrate(int value) const;

    private:
        bool enabled_ = true;

        AdaptiveStreamingState state_{};

        double stableTimeSec_ = 0.0;
        double badTimeSec_ = 0.0;

        // 調整しすぎ防止用
        double cooldownSec_ = 0.0;

        static constexpr int kMinQuality = 35;
        static constexpr int kMaxQuality = 95;

        static constexpr int kMinFps = 10;
        static constexpr int kMaxFps = 60;

        static constexpr int kMinBitrateKbps = 1000;
        static constexpr int kMaxBitrateKbps = 12000;
    };

} // namespace net