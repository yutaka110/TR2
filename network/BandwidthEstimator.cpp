#include "BandwidthEstimator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace net {
namespace {

    constexpr uint32_t kInitialEstimatedBandwidthBps = 6000000;
    constexpr uint32_t kMinEstimatedBandwidthBps = 300000;
    constexpr uint32_t kMaxEstimatedBandwidthBps = 12000000;
    constexpr double kMaxObservedDeliveryRateBps = 100000000.0;

    double Ewma(double current, double sample, double alpha) {
        if (current <= 0.0) {
            return sample;
        }

        return current + (sample - current) * alpha;
    }

    uint32_t ClampBandwidth(double bandwidthBps) {
        const double clamped = std::clamp(
            bandwidthBps,
            static_cast<double>(kMinEstimatedBandwidthBps),
            static_cast<double>(kMaxEstimatedBandwidthBps)
        );

        return static_cast<uint32_t>(std::llround(clamped));
    }

    uint32_t ClampObservedRate(double bandwidthBps) {
        const double clamped = std::clamp(
            bandwidthBps,
            0.0,
            kMaxObservedDeliveryRateBps
        );

        return static_cast<uint32_t>(std::llround(clamped));
    }

} // namespace

    BandwidthEstimator::BandwidthEstimator() {
        Reset();
    }

    void BandwidthEstimator::Reset() {
        std::lock_guard<std::mutex> lock(mutex_);

        stats_ = BandwidthEstimatorStats{};
        stats_.estimatedBandwidthBps = kInitialEstimatedBandwidthBps;

        smoothedDeliveryRateBps_ = 0.0;
        smoothedQueueDelayMs_ = 0.0;
        smoothedLossRate_ = 0.0;
        smoothedJitterMs_ = 0.0;
        smoothedRttTrendMs_ = 0.0;
        hasLastRttSample_ = false;
        lastRttMs_ = 0.0;
    }

    void BandwidthEstimator::OnTransportFeedback(
        const std::vector<BandwidthFeedbackPacket>& packets
    ) {
        if (packets.empty()) {
            return;
        }

        uint64_t receivedPackets = 0;
        uint64_t missingPackets = 0;
        uint64_t validReceivedPackets = 0;
        uint64_t receivedBytes = 0;

        uint64_t firstReceiveTimeUs = 0;
        uint64_t lastReceiveTimeUs = 0;

        bool hasPreviousReceived = false;
        uint64_t previousSendTimeUs = 0;
        uint64_t previousReceiveTimeUs = 0;
        double jitterSumMs = 0.0;
        uint32_t jitterSamples = 0;
        double queueDelaySumMs = 0.0;
        uint32_t queueDelaySamples = 0;

        for (const BandwidthFeedbackPacket& packet : packets) {
            if (!packet.received) {
                missingPackets++;
                hasPreviousReceived = false;
                continue;
            }

            receivedPackets++;

            if (packet.sendTimeUs == 0 ||
                packet.receiveTimeUs == 0 ||
                packet.packetBytes == 0) {
                hasPreviousReceived = false;
                continue;
            }

            validReceivedPackets++;
            receivedBytes += packet.packetBytes;

            if (firstReceiveTimeUs == 0 ||
                packet.receiveTimeUs < firstReceiveTimeUs) {
                firstReceiveTimeUs = packet.receiveTimeUs;
            }
            if (packet.receiveTimeUs > lastReceiveTimeUs) {
                lastReceiveTimeUs = packet.receiveTimeUs;
            }

            if (hasPreviousReceived &&
                packet.sendTimeUs >= previousSendTimeUs &&
                packet.receiveTimeUs >= previousReceiveTimeUs) {
                const double sendIntervalMs =
                    static_cast<double>(
                        packet.sendTimeUs - previousSendTimeUs) / 1000.0;
                const double receiveIntervalMs =
                    static_cast<double>(
                        packet.receiveTimeUs - previousReceiveTimeUs) / 1000.0;
                const double intervalDeltaMs =
                    receiveIntervalMs - sendIntervalMs;

                jitterSumMs += std::abs(intervalDeltaMs);
                jitterSamples++;

                if (intervalDeltaMs > 0.0) {
                    queueDelaySumMs += intervalDeltaMs;
                    queueDelaySamples++;
                }
            }

            previousSendTimeUs = packet.sendTimeUs;
            previousReceiveTimeUs = packet.receiveTimeUs;
            hasPreviousReceived = true;
        }

        const uint64_t statusCount = receivedPackets + missingPackets;
        const double lossRate = statusCount > 0
            ? static_cast<double>(missingPackets) /
                static_cast<double>(statusCount)
            : 0.0;
        const double jitterMs = jitterSamples > 0
            ? jitterSumMs / static_cast<double>(jitterSamples)
            : 0.0;
        const double queueDelayMs = queueDelaySamples > 0
            ? queueDelaySumMs / static_cast<double>(queueDelaySamples)
            : 0.0;

        double deliveryRateBps = 0.0;
        if (validReceivedPackets >= 2 &&
            lastReceiveTimeUs > firstReceiveTimeUs) {
            const double receiveSpanUs =
                static_cast<double>(lastReceiveTimeUs - firstReceiveTimeUs);
            deliveryRateBps =
                static_cast<double>(receivedBytes) * 8.0 * 1000000.0 /
                receiveSpanUs;
            deliveryRateBps = (std::min)(
                deliveryRateBps,
                kMaxObservedDeliveryRateBps
            );
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (deliveryRateBps > 0.0) {
            smoothedDeliveryRateBps_ = Ewma(
                smoothedDeliveryRateBps_,
                deliveryRateBps,
                0.25
            );
        }
        smoothedQueueDelayMs_ = Ewma(smoothedQueueDelayMs_, queueDelayMs, 0.25);
        smoothedLossRate_ = Ewma(smoothedLossRate_, lossRate, 0.25);
        smoothedJitterMs_ = Ewma(smoothedJitterMs_, jitterMs, 0.25);

        stats_.deliveryRateBps = ClampObservedRate(smoothedDeliveryRateBps_);
        stats_.queueDelayMs = smoothedQueueDelayMs_;
        stats_.lossTrend = smoothedLossRate_;
        stats_.jitterTrendMs = smoothedJitterMs_;
        stats_.rttTrendMs = smoothedRttTrendMs_;
        stats_.feedbackSamples += statusCount;

        UpdateEstimatedBandwidthLocked(deliveryRateBps);
    }

    void BandwidthEstimator::OnRttSample(double rttMs) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (hasLastRttSample_) {
            const double trendMs = rttMs - lastRttMs_;
            smoothedRttTrendMs_ = Ewma(smoothedRttTrendMs_, trendMs, 0.20);
            stats_.rttTrendMs = smoothedRttTrendMs_;
        }

        lastRttMs_ = rttMs;
        hasLastRttSample_ = true;
    }

    BandwidthEstimatorStats BandwidthEstimator::GetStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    void BandwidthEstimator::UpdateEstimatedBandwidthLocked(
        double measuredDeliveryRateBps
    ) {
        double nextEstimate =
            static_cast<double>(stats_.estimatedBandwidthBps);

        if (measuredDeliveryRateBps > 0.0) {
            nextEstimate =
                nextEstimate * 0.70 + measuredDeliveryRateBps * 0.30;
        }

        const bool severeCongestion =
            smoothedLossRate_ >= 0.08 ||
            smoothedQueueDelayMs_ >= 12.0 ||
            smoothedRttTrendMs_ >= 20.0 ||
            smoothedJitterMs_ >= 20.0;

        const bool moderateCongestion =
            smoothedLossRate_ >= 0.03 ||
            smoothedQueueDelayMs_ >= 4.0 ||
            smoothedRttTrendMs_ >= 8.0 ||
            smoothedJitterMs_ >= 8.0;

        const bool stableNetwork =
            smoothedLossRate_ <= 0.01 &&
            smoothedQueueDelayMs_ <= 1.5 &&
            smoothedRttTrendMs_ <= 2.0 &&
            smoothedJitterMs_ <= 4.0;

        if (severeCongestion) {
            nextEstimate *= 0.85;
        }
        else if (moderateCongestion) {
            nextEstimate *= 0.93;
        }
        else if (stableNetwork) {
            nextEstimate = (std::max)(
                nextEstimate,
                static_cast<double>(stats_.estimatedBandwidthBps) * 1.04 +
                    100000.0
            );
        }

        stats_.estimatedBandwidthBps = ClampBandwidth(nextEstimate);
    }

} // namespace net
