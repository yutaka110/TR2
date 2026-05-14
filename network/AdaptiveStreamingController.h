#pragma once

#include <cstddef>
#include <cstdint>

namespace net {

    struct AdaptiveStreamingInput {
        double ackMissingRate = 0.0;
        double packetLossRate = 0.0;
        double rttMs = 0.0;
        double latencyMs = 0.0;
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
        double lastRttMs = 0.0;
        double lastLatencyMs = 0.0;
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

    private:
        void ApplyMultiplicativeDecrease(double factor);
        void ApplyAdditiveIncrease(int bitrateKbps);
        void DeriveTargetsFromBitrate();

        int ClampQuality(int value) const;
        int ClampFps(int value) const;
        int ClampBitrate(int value) const;
        int ClampWidth(int value) const;
        int ClampHeight(int value) const;

    private:
        bool enabled_ = true;

        AdaptiveStreamingState state_{};

        double stableTimeSec_ = 0.0;
        double badTimeSec_ = 0.0;
        double cooldownSec_ = 0.0;

        static constexpr int kMinQuality = 35;
        static constexpr int kMaxQuality = 95;

        static constexpr int kMinFps = 8;
        static constexpr int kMaxFps = 30;

        static constexpr int kMinBitrateKbps = 500;
        static constexpr int kMaxBitrateKbps = 12000;
    };

} // namespace net
