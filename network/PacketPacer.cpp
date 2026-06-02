#include "PacketPacer.h"

#include "PacketProtocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace net {

    PacketPacer::PacketPacer() = default;

    PacketPacer::~PacketPacer() {
        Stop();
    }

    void PacketPacer::Start(SendCallback callback) {
        Stop();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            sendCallback_ = std::move(callback);
            running_ = true;
            nextSendTimeUs_ = NowMicroseconds();
            lastCreditUpdateUs_ = nextSendTimeUs_;
            pacingCreditBytes_ =
                static_cast<double>(
                    targetBitrateBps_.load(std::memory_order_relaxed)) *
                kInitialBurstWindowSec / 8.0;
        }

        workerThread_ = std::thread(&PacketPacer::SendLoop, this);
    }

    void PacketPacer::Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ && !workerThread_.joinable()) {
                return;
            }

            running_ = false;
            highPriorityQueue_.clear();
            normalQueue_.clear();
        }

        cv_.notify_all();

        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    }

    void PacketPacer::SetEnabled(bool enabled) {
        enabled_.store(enabled, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.enabled = enabled;
            if (!enabled) {
                highPriorityQueue_.clear();
                normalQueue_.clear();
                stats_.queuedPackets = 0;
                stats_.highPriorityQueuedPackets = 0;
                stats_.normalQueuedPackets = 0;
                stats_.currentQueueDelayMs = 0.0;
            }
        }

        cv_.notify_all();
    }

    bool PacketPacer::IsEnabled() const {
        return enabled_.load(std::memory_order_relaxed);
    }

    void PacketPacer::SetTargetBitrateBps(uint32_t bitrateBps) {
        targetBitrateBps_.store(
            (std::max)(bitrateBps, kMinBitrateBps),
            std::memory_order_relaxed
        );

        std::lock_guard<std::mutex> lock(mutex_);
        stats_.targetBitrateBps =
            targetBitrateBps_.load(std::memory_order_relaxed);
    }

    uint32_t PacketPacer::GetTargetBitrateBps() const {
        return targetBitrateBps_.load(std::memory_order_relaxed);
    }

    bool PacketPacer::EnqueuePacket(
        std::vector<uint8_t>&& packet,
        const char* context,
        PacketPacingPriority priority,
        uint64_t deadlineUs
    ) {
        if (packet.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !enabled_.load(std::memory_order_relaxed)) {
            return false;
        }

        if (QueueSizeLocked() >= kMaxQueuedPackets) {
            DropOneForOverflowLocked(priority);
        }

        QueuedPacket queued{};
        queued.data = std::move(packet);
        queued.context = context != nullptr ? context : "PacketPacer";
        queued.enqueueTimeUs = NowMicroseconds();
        queued.deadlineUs = deadlineUs;
        FillRnvpDataSequence(queued);

        if (priority == PacketPacingPriority::High) {
            highPriorityQueue_.push_back(std::move(queued));
        }
        else {
            normalQueue_.push_back(std::move(queued));
        }

        stats_.enabled = enabled_.load(std::memory_order_relaxed);
        stats_.targetBitrateBps =
            targetBitrateBps_.load(std::memory_order_relaxed);
        stats_.enqueuedPackets++;
        stats_.queuedPackets = QueueSizeLocked();
        stats_.highPriorityQueuedPackets =
            static_cast<uint32_t>(highPriorityQueue_.size());
        stats_.normalQueuedPackets =
            static_cast<uint32_t>(normalQueue_.size());

        cv_.notify_one();
        return true;
    }

    PacketPacerStats PacketPacer::GetStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        PacketPacerStats stats = stats_;
        stats.enabled = enabled_.load(std::memory_order_relaxed);
        stats.targetBitrateBps =
            targetBitrateBps_.load(std::memory_order_relaxed);
        stats.queuedPackets = QueueSizeLocked();
        stats.highPriorityQueuedPackets =
            static_cast<uint32_t>(highPriorityQueue_.size());
        stats.normalQueuedPackets =
            static_cast<uint32_t>(normalQueue_.size());
        return stats;
    }

    void PacketPacer::ResetStats() {
        std::lock_guard<std::mutex> lock(mutex_);

        highPriorityQueue_.clear();
        normalQueue_.clear();
        nextSendTimeUs_ = NowMicroseconds();
        lastCreditUpdateUs_ = nextSendTimeUs_;
        pacingCreditBytes_ =
            static_cast<double>(
                targetBitrateBps_.load(std::memory_order_relaxed)) *
            kInitialBurstWindowSec / 8.0;

        stats_ = PacketPacerStats{};
        stats_.enabled = enabled_.load(std::memory_order_relaxed);
        stats_.targetBitrateBps =
            targetBitrateBps_.load(std::memory_order_relaxed);
    }

    void PacketPacer::SendLoop() {
        while (true) {
            QueuedPacket packet{};

            {
                std::unique_lock<std::mutex> lock(mutex_);

                cv_.wait(lock, [&]() {
                    return !running_ || QueueSizeLocked() > 0;
                });

                if (!running_) {
                    break;
                }

                if (!enabled_.load(std::memory_order_relaxed)) {
                    continue;
                }

                while (true) {
                    const bool useHighPriority =
                        ShouldSendHighPriorityFirstLocked();

                    if (!useHighPriority && normalQueue_.empty()) {
                        break;
                    }

                    QueuedPacket& candidate =
                        useHighPriority
                        ? highPriorityQueue_.front()
                        : normalQueue_.front();

                    const uint64_t nowUs = NowMicroseconds();
                    RefillPacingCreditLocked(nowUs);

                    if (candidate.deadlineUs != 0 &&
                        nowUs > candidate.deadlineUs) {
                        stats_.droppedPackets++;
                        stats_.deadlineDroppedPackets++;
                        stats_.currentQueueDelayMs =
                            static_cast<double>(
                                nowUs - candidate.enqueueTimeUs) / 1000.0;
                        stats_.maxQueueDelayMs =
                            (std::max)(
                                stats_.maxQueueDelayMs,
                                stats_.currentQueueDelayMs
                            );

                        if (useHighPriority) {
                            highPriorityQueue_.pop_front();
                        }
                        else {
                            normalQueue_.pop_front();
                        }

                        stats_.queuedPackets = QueueSizeLocked();
                        stats_.highPriorityQueuedPackets =
                            static_cast<uint32_t>(highPriorityQueue_.size());
                        stats_.normalQueuedPackets =
                            static_cast<uint32_t>(normalQueue_.size());

                        continue;
                    }

                    const size_t packetBytes = candidate.data.size();
                    if (pacingCreditBytes_ <
                        static_cast<double>(packetBytes)) {
                        const uint64_t waitUs =
                            CalculateCreditWaitUsLocked(packetBytes);
                        cv_.wait_for(
                            lock,
                            std::chrono::microseconds(waitUs)
                        );
                        break;
                    }

                    pacingCreditBytes_ -= static_cast<double>(packetBytes);

                    packet = std::move(candidate);
                    if (useHighPriority) {
                        highPriorityQueue_.pop_front();
                    }
                    else {
                        normalQueue_.pop_front();
                    }

                    stats_.queuedPackets = QueueSizeLocked();
                    stats_.highPriorityQueuedPackets =
                        static_cast<uint32_t>(highPriorityQueue_.size());
                    stats_.normalQueuedPackets =
                        static_cast<uint32_t>(normalQueue_.size());

                    stats_.currentQueueDelayMs =
                        static_cast<double>(
                            nowUs - packet.enqueueTimeUs) / 1000.0;
                    stats_.maxQueueDelayMs =
                        (std::max)(
                            stats_.maxQueueDelayMs,
                            stats_.currentQueueDelayMs
                        );

                    break;
                }

                if (packet.data.empty()) {
                    continue;
                }
            }

            const size_t packetBytes = packet.data.size();
            if (sendCallback_) {
                sendCallback_(std::move(packet.data), packet.context.c_str());
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.sentPackets++;
                stats_.sentBytes += packetBytes;
            }
        }
    }

    uint64_t PacketPacer::NowMicroseconds() const {
        using namespace std::chrono;

        return static_cast<uint64_t>(
            duration_cast<microseconds>(
                steady_clock::now().time_since_epoch()
            ).count()
        );
    }

    uint64_t PacketPacer::CalculateIntervalUs(size_t packetBytes) const {
        const uint32_t bitrateBps =
            (std::max)(
                targetBitrateBps_.load(std::memory_order_relaxed),
                kMinBitrateBps
            );
        const double packetBits = static_cast<double>(packetBytes) * 8.0;
        const double intervalUs =
            packetBits * 1000000.0 / static_cast<double>(bitrateBps);
        return (std::max)(
            uint64_t{ 100 },
            static_cast<uint64_t>(intervalUs)
        );
    }

    void PacketPacer::RefillPacingCreditLocked(uint64_t nowUs) {
        if (lastCreditUpdateUs_ == 0 || nowUs < lastCreditUpdateUs_) {
            lastCreditUpdateUs_ = nowUs;
            return;
        }

        const uint64_t elapsedUs = nowUs - lastCreditUpdateUs_;
        if (elapsedUs == 0) {
            return;
        }

        const uint32_t bitrateBps =
            (std::max)(
                targetBitrateBps_.load(std::memory_order_relaxed),
                kMinBitrateBps
            );

        pacingCreditBytes_ +=
            static_cast<double>(elapsedUs) *
            static_cast<double>(bitrateBps) / 8000000.0;

        const double maxCreditBytes =
            (std::max)(
                16.0 * 1024.0,
                static_cast<double>(bitrateBps) *
                kMaxBurstWindowSec / 8.0
            );

        pacingCreditBytes_ =
            (std::min)(pacingCreditBytes_, maxCreditBytes);
        lastCreditUpdateUs_ = nowUs;
    }

    uint64_t PacketPacer::CalculateCreditWaitUsLocked(
        size_t packetBytes
    ) const {
        const uint32_t bitrateBps =
            (std::max)(
                targetBitrateBps_.load(std::memory_order_relaxed),
                kMinBitrateBps
            );

        const double neededBytes =
            (std::max)(
                0.0,
                static_cast<double>(packetBytes) - pacingCreditBytes_
            );
        const double waitUs =
            neededBytes * 8000000.0 / static_cast<double>(bitrateBps);

        return (std::max)(
            uint64_t{ 100 },
            static_cast<uint64_t>(std::ceil(waitUs))
        );
    }

    void PacketPacer::FillRnvpDataSequence(QueuedPacket& packet) const {
        if (packet.data.size() < kRnvpHeaderV1Size ||
            ReadU32BE(packet.data.data()) != kRnvpMagic) {
            return;
        }

        RnvpHeaderV1 header{};
        if (!DecodeRnvpHeaderV1(packet.data.data(), packet.data.size(), header)) {
            return;
        }

        if (static_cast<PacketType>(header.packetType) != PacketType::Data) {
            return;
        }

        packet.hasRnvpDataSequence = true;
        packet.rnvpDataSequence = header.sequence;
    }

    bool PacketPacer::ShouldSendHighPriorityFirstLocked() const {
        if (highPriorityQueue_.empty()) {
            return false;
        }

        if (normalQueue_.empty()) {
            return true;
        }

        const QueuedPacket& high = highPriorityQueue_.front();
        const QueuedPacket& normal = normalQueue_.front();

        if (high.hasRnvpDataSequence &&
            normal.hasRnvpDataSequence &&
            normal.rnvpDataSequence < high.rnvpDataSequence) {
            return false;
        }

        return true;
    }

    uint32_t PacketPacer::QueueSizeLocked() const {
        return static_cast<uint32_t>(
            highPriorityQueue_.size() + normalQueue_.size()
        );
    }

    void PacketPacer::DropOneForOverflowLocked(
        PacketPacingPriority incomingPriority
    ) {
        if (!normalQueue_.empty()) {
            normalQueue_.pop_front();
        }
        else if (incomingPriority == PacketPacingPriority::High &&
            !highPriorityQueue_.empty()) {
            highPriorityQueue_.pop_front();
        }
        else if (!highPriorityQueue_.empty()) {
            highPriorityQueue_.pop_front();
        }
        else {
            return;
        }

        stats_.droppedPackets++;
        stats_.overflowDroppedPackets++;
    }

} // namespace net
