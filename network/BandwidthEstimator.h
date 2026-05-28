#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace net {

    struct BandwidthFeedbackPacket {
        uint32_t sequence = 0;
        uint64_t sendTimeUs = 0;
        uint64_t receiveTimeUs = 0;
        uint32_t packetBytes = 0;
        bool received = false;
    };

    struct BandwidthEstimatorStats {
        uint32_t estimatedBandwidthBps = 6000000;
        uint32_t deliveryRateBps = 0;
        double queueDelayMs = 0.0;
        double rttTrendMs = 0.0;
        double lossTrend = 0.0;
        double jitterTrendMs = 0.0;
        uint64_t feedbackSamples = 0;
    };

    class BandwidthEstimator {
    public:
        BandwidthEstimator();

        void Reset();
        void OnTransportFeedback(
            const std::vector<BandwidthFeedbackPacket>& packets
        );
        void OnRttSample(double rttMs);

        BandwidthEstimatorStats GetStats() const;

    private:
        void UpdateEstimatedBandwidthLocked(double measuredDeliveryRateBps);

    private:
        mutable std::mutex mutex_;
        BandwidthEstimatorStats stats_{};

        double smoothedDeliveryRateBps_ = 0.0;
        double smoothedQueueDelayMs_ = 0.0;
        double smoothedLossRate_ = 0.0;
        double smoothedJitterMs_ = 0.0;
        double smoothedRttTrendMs_ = 0.0;

        bool hasLastRttSample_ = false;
        double lastRttMs_ = 0.0;
    };

} // namespace net
