#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net {

    enum class PacketPacingPriority {
        Normal,
        High,
        Critical
    };

    struct PacketPacerStats {
        bool enabled = false;
        uint32_t targetBitrateBps = 0;
        uint32_t repairTargetBitrateBps = 0;
        uint32_t queuedPackets = 0;
        uint32_t highPriorityQueuedPackets = 0;
        uint32_t normalQueuedPackets = 0;
        uint64_t enqueuedPackets = 0;
        uint64_t sentPackets = 0;
        uint64_t sentBytes = 0;
        uint64_t repairSentPackets = 0;
        uint64_t repairSentBytes = 0;
        uint64_t repairBorrowedPackets = 0;
        uint64_t repairBorrowedBytes = 0;
        uint64_t droppedPackets = 0;
        uint64_t deadlineDroppedPackets = 0;
        uint64_t highPriorityDeadlineDroppedPackets = 0;
        uint64_t normalDeadlineDroppedPackets = 0;
        uint64_t overflowDroppedPackets = 0;
        double currentQueueDelayMs = 0.0;
        double maxQueueDelayMs = 0.0;
        double videoCreditBytes = 0.0;
        double repairCreditBytes = 0.0;
    };

    class PacketPacer {
    public:
        using SendCallback =
            std::function<void(std::vector<uint8_t>&&, const char*)>;
        using DropCallback =
            std::function<bool(const std::vector<uint8_t>&, const char*)>;

        PacketPacer();
        ~PacketPacer();

        void Start(SendCallback callback);
        void SetDropCallback(DropCallback callback);
        void Stop();

        void SetEnabled(bool enabled);
        bool IsEnabled() const;

        void SetTargetBitrateBps(uint32_t bitrateBps);
        uint32_t GetTargetBitrateBps() const;

        bool EnqueuePacket(
            std::vector<uint8_t>&& packet,
            const char* context,
            PacketPacingPriority priority,
            uint64_t deadlineUs
        );

        PacketPacerStats GetStats() const;
        void ResetStats();

    private:
        struct QueuedPacket {
            std::vector<uint8_t> data;
            std::string context;
            uint64_t enqueueTimeUs = 0;
            uint64_t deadlineUs = 0;
            PacketPacingPriority priority = PacketPacingPriority::Normal;
            bool hasRnvpDataSequence = false;
            uint32_t rnvpDataSequence = 0;
        };

        void SendLoop();
        uint64_t NowMicroseconds() const;
        void RefillPacingCreditLocked(uint64_t nowUs);
        uint64_t CalculateVideoCreditWaitUsLocked(size_t packetBytes) const;
        uint64_t CalculateRepairCreditWaitUsLocked(size_t packetBytes) const;
        void FillRnvpDataSequence(QueuedPacket& packet) const;
        bool SelectNextPacketLocked(bool& useHighPriority) const;
        bool HasCreditForPacketLocked(
            const QueuedPacket& packet,
            bool highPriority
        ) const;
        void InsertQueuedPacketLocked(
            QueuedPacket&& packet,
            PacketPacingPriority priority
        );
        uint32_t QueueSizeLocked() const;
        void DropOneForOverflowLocked(PacketPacingPriority incomingPriority);
        void DropExpiredFrontPacketsLocked(uint64_t nowUs);
        uint32_t GetRepairTargetBitrateBpsLocked() const;

    private:
        mutable std::mutex mutex_;
        std::condition_variable cv_;

        std::deque<QueuedPacket> highPriorityQueue_;
        std::deque<QueuedPacket> normalQueue_;

        SendCallback sendCallback_;
        DropCallback dropCallback_;
        std::thread workerThread_;
        bool running_ = false;

        std::atomic<bool> enabled_{ true };
        std::atomic<uint32_t> targetBitrateBps_{ 6000000 };

        PacketPacerStats stats_{};

        uint64_t nextSendTimeUs_ = 0;
        uint64_t lastCreditUpdateUs_ = 0;
        double pacingCreditBytes_ = 0.0;
        double repairCreditBytes_ = 0.0;

        static constexpr uint32_t kMinBitrateBps = 100000;
        static constexpr uint32_t kMaxQueuedPackets = 512;
        static constexpr double kMaxBurstWindowSec = 0.020;
        static constexpr double kInitialBurstWindowSec = 0.005;
    };

} // namespace net
