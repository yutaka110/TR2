#include "NetworkConditionSimulator.h"

#include <algorithm>
#include <chrono>

namespace net {

    namespace {

        double ClampRate(double value) {
            return (std::max)(0.0, (std::min)(1.0, value));
        }

    } // namespace

    NetworkConditionSimulator::NetworkConditionSimulator()
        : rng_(static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count())) {
    }

    void NetworkConditionSimulator::SetCondition(
        const NetworkCondition& condition
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        condition_ = condition;
        condition_.lossRate = ClampRate(condition_.lossRate);
        condition_.duplicateRate = ClampRate(condition_.duplicateRate);
        condition_.reorderRate = ClampRate(condition_.reorderRate);

        if (condition_.maxDelayMs < condition_.minDelayMs) {
            condition_.maxDelayMs = condition_.minDelayMs;
        }

        if (condition_.maxPendingPackets == 0) {
            condition_.maxPendingPackets = 1;
        }

        if (!condition_.enabled) {
            pending_.clear();
            burstLossRemaining_ = 0;
            stats_.pendingPackets = 0;
        }
    }

    NetworkCondition NetworkConditionSimulator::GetCondition() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return condition_;
    }

    NetworkSimulationStats NetworkConditionSimulator::GetStats() const {
        std::lock_guard<std::mutex> lock(mutex_);

        NetworkSimulationStats stats = stats_;
        stats.pendingPackets = static_cast<uint32_t>(pending_.size());
        return stats;
    }

    void NetworkConditionSimulator::Reset() {
        std::lock_guard<std::mutex> lock(mutex_);

        stats_ = NetworkSimulationStats{};
        pending_.clear();
        burstLossRemaining_ = 0;
        nextOrder_ = 1;
    }

    bool NetworkConditionSimulator::IsEnabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return condition_.enabled;
    }

    void NetworkConditionSimulator::SubmitPacket(
        std::vector<uint8_t>&& packet,
        uint64_t nowUs
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!condition_.enabled) {
            QueuePacketLocked(std::move(packet), nowUs);
            return;
        }

        stats_.submittedPackets++;

        if (ShouldDropPacket()) {
            stats_.droppedPackets++;
            return;
        }

        uint64_t dueTimeUs = nowUs + RandomDelayUs();
        if (condition_.reorderRate > 0.0 &&
            RandomUnit() < condition_.reorderRate) {
            dueTimeUs += RandomReorderExtraDelayUs();
            stats_.reorderedPackets++;
        }

        if (condition_.duplicateRate > 0.0 &&
            RandomUnit() < condition_.duplicateRate) {
            std::vector<uint8_t> duplicate = packet;
            QueuePacketLocked(
                std::move(duplicate),
                dueTimeUs + RandomReorderExtraDelayUs()
            );
            stats_.duplicatedPackets++;
        }

        QueuePacketLocked(std::move(packet), dueTimeUs);
    }

    void NetworkConditionSimulator::PopReadyPackets(
        uint64_t nowUs,
        std::vector<std::vector<uint8_t>>& outPackets
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<PendingPacket> ready;

        auto it = pending_.begin();
        while (it != pending_.end()) {
            if (it->dueTimeUs <= nowUs) {
                ready.push_back(std::move(*it));
                it = pending_.erase(it);
            }
            else {
                ++it;
            }
        }

        std::sort(
            ready.begin(),
            ready.end(),
            [](const PendingPacket& lhs, const PendingPacket& rhs) {
                if (lhs.dueTimeUs != rhs.dueTimeUs) {
                    return lhs.dueTimeUs < rhs.dueTimeUs;
                }
                return lhs.order < rhs.order;
            });

        for (PendingPacket& packet : ready) {
            outPackets.push_back(std::move(packet.data));
            stats_.sentPackets++;
        }

        stats_.pendingPackets = static_cast<uint32_t>(pending_.size());
    }

    double NetworkConditionSimulator::RandomUnit() {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return dist(rng_);
    }

    uint64_t NetworkConditionSimulator::RandomDelayUs() {
        if (condition_.maxDelayMs == 0) {
            return 0;
        }

        std::uniform_int_distribution<uint32_t> dist(
            condition_.minDelayMs,
            condition_.maxDelayMs
        );

        return static_cast<uint64_t>(dist(rng_)) * 1000ull;
    }

    uint64_t NetworkConditionSimulator::RandomReorderExtraDelayUs() {
        const uint32_t upperMs =
            (std::max)(condition_.maxDelayMs, 30u);

        std::uniform_int_distribution<uint32_t> dist(1u, upperMs);
        return static_cast<uint64_t>(dist(rng_)) * 1000ull;
    }

    bool NetworkConditionSimulator::ShouldDropPacket() {
        if (burstLossRemaining_ > 0) {
            burstLossRemaining_--;
            return true;
        }

        if (condition_.lossRate <= 0.0) {
            return false;
        }

        if (RandomUnit() >= condition_.lossRate) {
            return false;
        }

        if (condition_.burstLossLength > 1) {
            burstLossRemaining_ = condition_.burstLossLength - 1;
            stats_.burstLossEvents++;
        }

        return true;
    }

    void NetworkConditionSimulator::QueuePacketLocked(
        std::vector<uint8_t>&& packet,
        uint64_t dueTimeUs
    ) {
        if (pending_.size() >= condition_.maxPendingPackets) {
            stats_.droppedPackets++;
            return;
        }

        PendingPacket pending{};
        pending.dueTimeUs = dueTimeUs;
        pending.order = nextOrder_++;
        pending.data = std::move(packet);

        pending_.push_back(std::move(pending));
        stats_.pendingPackets = static_cast<uint32_t>(pending_.size());
    }

} // namespace net
