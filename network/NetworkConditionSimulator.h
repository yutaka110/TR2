#pragma once

#include <cstdint>
#include <mutex>
#include <random>
#include <vector>

namespace net {

    struct NetworkCondition {
        bool enabled = false;

        double lossRate = 0.0;
        double duplicateRate = 0.0;
        double reorderRate = 0.0;

        uint32_t minDelayMs = 0;
        uint32_t maxDelayMs = 0;

        uint32_t burstLossLength = 0;
        uint32_t maxPendingPackets = 4096;
    };

    struct NetworkSimulationStats {
        uint64_t submittedPackets = 0;
        uint64_t sentPackets = 0;
        uint64_t droppedPackets = 0;
        uint64_t duplicatedPackets = 0;
        uint64_t reorderedPackets = 0;
        uint64_t burstLossEvents = 0;
        uint32_t pendingPackets = 0;
    };

    class NetworkConditionSimulator {
    public:
        NetworkConditionSimulator();

        void SetCondition(const NetworkCondition& condition);
        NetworkCondition GetCondition() const;

        NetworkSimulationStats GetStats() const;
        void Reset();

        bool IsEnabled() const;

        void SubmitPacket(
            std::vector<uint8_t>&& packet,
            uint64_t nowUs
        );

        void PopReadyPackets(
            uint64_t nowUs,
            std::vector<std::vector<uint8_t>>& outPackets
        );

    private:
        struct PendingPacket {
            uint64_t dueTimeUs = 0;
            uint64_t order = 0;
            std::vector<uint8_t> data;
        };

        double RandomUnit();
        uint64_t RandomDelayUs();
        uint64_t RandomReorderExtraDelayUs();

        bool ShouldDropPacket();
        void QueuePacketLocked(
            std::vector<uint8_t>&& packet,
            uint64_t dueTimeUs
        );

    private:
        mutable std::mutex mutex_;

        NetworkCondition condition_{};
        NetworkSimulationStats stats_{};

        std::mt19937 rng_;
        uint64_t nextOrder_ = 1;
        uint32_t burstLossRemaining_ = 0;

        std::vector<PendingPacket> pending_;
    };

} // namespace net
