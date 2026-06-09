#include "PacketPacer.h"

#include "PacketProtocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace net {
namespace {

    std::string ReadEnvString(const char* name) {
        char* buffer = nullptr;
        size_t length = 0;
        if (_dupenv_s(&buffer, &length, name) != 0 ||
            buffer == nullptr) {
            return {};
        }

        std::string value(buffer);
        std::free(buffer);
        return value;
    }

    double ReadEnvDoubleClamped(
        const char* name,
        double fallback,
        double minValue,
        double maxValue
    ) {
        const std::string text = ReadEnvString(name);
        if (text.empty()) {
            return fallback;
        }

        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end == text.c_str()) {
            return fallback;
        }

        return std::clamp(value, minValue, maxValue);
    }

    uint32_t ReadEnvUInt32Clamped(
        const char* name,
        uint32_t fallback,
        uint32_t minValue,
        uint32_t maxValue
    ) {
        const std::string text = ReadEnvString(name);
        if (text.empty()) {
            return fallback;
        }

        char* end = nullptr;
        const unsigned long value = std::strtoul(text.c_str(), &end, 10);
        if (end == text.c_str()) {
            return fallback;
        }

        return static_cast<uint32_t>(
            std::clamp<unsigned long>(value, minValue, maxValue));
    }

    double RepairBurstCreditBytes(uint32_t repairBitrateBps) {
        const uint32_t burstPackets =
            ReadEnvUInt32Clamped(
                "RNVP_REPAIR_MAX_BURST_PACKETS",
                4,
                1,
                16);
        const double burstWindowSec =
            ReadEnvDoubleClamped(
                "RNVP_REPAIR_MAX_BURST_WINDOW_SEC",
                0.008,
                0.001,
                0.050);
        return (std::max)(
            static_cast<double>(burstPackets) *
                static_cast<double>(kMaxUdpPayloadSize),
            static_cast<double>(repairBitrateBps) * burstWindowSec / 8.0);
    }

    bool IsHighPriority(PacketPacingPriority priority) {
        return priority != PacketPacingPriority::Normal;
    }

    int PriorityRank(PacketPacingPriority priority) {
        switch (priority) {
        case PacketPacingPriority::Critical:
            return 2;
        case PacketPacingPriority::High:
            return 1;
        case PacketPacingPriority::Normal:
        default:
            return 0;
        }
    }

    uint64_t DeadlineSortKey(uint64_t deadlineUs) {
        return deadlineUs == 0
            ? (std::numeric_limits<uint64_t>::max)()
            : deadlineUs;
    }

} // namespace

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
            repairCreditBytes_ =
                RepairBurstCreditBytes(GetRepairTargetBitrateBpsLocked());
        }

        workerThread_ = std::thread(&PacketPacer::SendLoop, this);
    }

    void PacketPacer::SetDropCallback(DropCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        dropCallback_ = std::move(callback);
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
        stats_.repairTargetBitrateBps = GetRepairTargetBitrateBpsLocked();
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
        queued.priority = priority;
        FillRnvpDataSequence(queued);

        InsertQueuedPacketLocked(std::move(queued), priority);

        stats_.enabled = enabled_.load(std::memory_order_relaxed);
        stats_.targetBitrateBps =
            targetBitrateBps_.load(std::memory_order_relaxed);
        stats_.repairTargetBitrateBps = GetRepairTargetBitrateBpsLocked();
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
        stats.repairTargetBitrateBps = GetRepairTargetBitrateBpsLocked();
        stats.queuedPackets = QueueSizeLocked();
        stats.highPriorityQueuedPackets =
            static_cast<uint32_t>(highPriorityQueue_.size());
        stats.normalQueuedPackets =
            static_cast<uint32_t>(normalQueue_.size());
        stats.videoCreditBytes = pacingCreditBytes_;
        stats.repairCreditBytes = repairCreditBytes_;
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
        repairCreditBytes_ =
            RepairBurstCreditBytes(GetRepairTargetBitrateBpsLocked());

        stats_ = PacketPacerStats{};
        stats_.enabled = enabled_.load(std::memory_order_relaxed);
        stats_.targetBitrateBps =
            targetBitrateBps_.load(std::memory_order_relaxed);
        stats_.repairTargetBitrateBps = GetRepairTargetBitrateBpsLocked();
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
                    const uint64_t nowUs = NowMicroseconds();
                    RefillPacingCreditLocked(nowUs);
                    DropExpiredFrontPacketsLocked(nowUs);

                    bool useHighPriority = false;
                    if (!SelectNextPacketLocked(useHighPriority)) {
                        break;
                    }

                    QueuedPacket& candidate =
                        useHighPriority
                        ? highPriorityQueue_.front()
                        : normalQueue_.front();

                    const size_t packetBytes = candidate.data.size();
                    if (dropCallback_ &&
                        dropCallback_(candidate.data, candidate.context.c_str())) {
                        if (useHighPriority) {
                            highPriorityQueue_.pop_front();
                        }
                        else {
                            normalQueue_.pop_front();
                        }
                        stats_.droppedPackets++;
                        stats_.queuedPackets = QueueSizeLocked();
                        stats_.highPriorityQueuedPackets =
                            static_cast<uint32_t>(highPriorityQueue_.size());
                        stats_.normalQueuedPackets =
                            static_cast<uint32_t>(normalQueue_.size());
                        continue;
                    }

                    if (!HasCreditForPacketLocked(candidate, useHighPriority)) {
                        const uint64_t waitUs =
                            useHighPriority
                            ? (std::min)(
                                CalculateRepairCreditWaitUsLocked(packetBytes),
                                CalculateVideoCreditWaitUsLocked(packetBytes))
                            : CalculateVideoCreditWaitUsLocked(packetBytes);
                        if (candidate.deadlineUs != 0 &&
                            waitUs >
                                candidate.deadlineUs - (std::min)(
                                    candidate.deadlineUs,
                                    nowUs)) {
                            const uint64_t enqueueTimeUs =
                                candidate.enqueueTimeUs;
                            if (useHighPriority) {
                                highPriorityQueue_.pop_front();
                                stats_.highPriorityDeadlineDroppedPackets++;
                            }
                            else {
                                normalQueue_.pop_front();
                                stats_.normalDeadlineDroppedPackets++;
                            }
                            stats_.droppedPackets++;
                            stats_.deadlineDroppedPackets++;
                            stats_.queuedPackets = QueueSizeLocked();
                            stats_.highPriorityQueuedPackets =
                                static_cast<uint32_t>(
                                    highPriorityQueue_.size());
                            stats_.normalQueuedPackets =
                                static_cast<uint32_t>(
                                    normalQueue_.size());
                            stats_.currentQueueDelayMs =
                                static_cast<double>(
                                    nowUs - enqueueTimeUs) /
                                1000.0;
                            stats_.maxQueueDelayMs =
                                (std::max)(
                                    stats_.maxQueueDelayMs,
                                    stats_.currentQueueDelayMs);
                            continue;
                        }
                        cv_.wait_for(
                            lock,
                            std::chrono::microseconds(waitUs)
                        );
                        break;
                    }

                    bool repairBorrowedFromVideo = false;
                    if (useHighPriority &&
                        repairCreditBytes_ >= static_cast<double>(packetBytes)) {
                        repairCreditBytes_ -= static_cast<double>(packetBytes);
                    }
                    else {
                        pacingCreditBytes_ -= static_cast<double>(packetBytes);
                        repairBorrowedFromVideo = useHighPriority;
                    }

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
                    stats_.videoCreditBytes = pacingCreditBytes_;
                    stats_.repairCreditBytes = repairCreditBytes_;
                    if (useHighPriority) {
                        stats_.repairSentPackets++;
                        stats_.repairSentBytes += packetBytes;
                        if (repairBorrowedFromVideo) {
                            stats_.repairBorrowedPackets++;
                            stats_.repairBorrowedBytes += packetBytes;
                        }
                    }

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
        repairCreditBytes_ +=
            static_cast<double>(elapsedUs) *
            static_cast<double>(GetRepairTargetBitrateBpsLocked()) /
            8000000.0;

        const double maxCreditBytes =
            (std::max)(
                16.0 * 1024.0,
                static_cast<double>(bitrateBps) *
                kMaxBurstWindowSec / 8.0
            );
        const double maxRepairCreditBytes =
            RepairBurstCreditBytes(GetRepairTargetBitrateBpsLocked());

        pacingCreditBytes_ =
            (std::min)(pacingCreditBytes_, maxCreditBytes);
        repairCreditBytes_ =
            (std::min)(repairCreditBytes_, maxRepairCreditBytes);
        lastCreditUpdateUs_ = nowUs;
    }

    uint64_t PacketPacer::CalculateVideoCreditWaitUsLocked(
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

    uint64_t PacketPacer::CalculateRepairCreditWaitUsLocked(
        size_t packetBytes
    ) const {
        const uint32_t bitrateBps = GetRepairTargetBitrateBpsLocked();
        if (bitrateBps == 0) {
            return (std::numeric_limits<uint64_t>::max)();
        }

        const double neededBytes =
            (std::max)(
                0.0,
                static_cast<double>(packetBytes) - repairCreditBytes_
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

    bool PacketPacer::SelectNextPacketLocked(bool& useHighPriority) const {
        useHighPriority = false;
        if (highPriorityQueue_.empty()) {
            return !normalQueue_.empty();
        }
        if (normalQueue_.empty()) {
            useHighPriority = true;
            return true;
        }

        const QueuedPacket& high = highPriorityQueue_.front();
        const QueuedPacket& normal = normalQueue_.front();
        if (HasCreditForPacketLocked(high, true) ||
            !HasCreditForPacketLocked(normal, false)) {
            useHighPriority = true;
            return true;
        }

        useHighPriority = false;
        return true;
    }

    bool PacketPacer::HasCreditForPacketLocked(
        const QueuedPacket& packet,
        bool highPriority
    ) const {
        const double packetBytes = static_cast<double>(packet.data.size());
        if (highPriority &&
            repairCreditBytes_ >= packetBytes) {
            return true;
        }

        return pacingCreditBytes_ >= packetBytes;
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
        else if (IsHighPriority(incomingPriority) &&
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

    void PacketPacer::InsertQueuedPacketLocked(
        QueuedPacket&& packet,
        PacketPacingPriority priority
    ) {
        std::deque<QueuedPacket>& queue =
            IsHighPriority(priority)
            ? highPriorityQueue_
            : normalQueue_;

        const int incomingRank = PriorityRank(priority);
        const uint64_t incomingDeadline =
            DeadlineSortKey(packet.deadlineUs);

        const auto insertAt = std::find_if(
            queue.begin(),
            queue.end(),
            [incomingRank, incomingDeadline](
                const QueuedPacket& candidate) {
                const int candidateRank =
                    PriorityRank(candidate.priority);
                if (incomingRank != candidateRank) {
                    return incomingRank > candidateRank;
                }

                return incomingDeadline <
                    DeadlineSortKey(candidate.deadlineUs);
            });

        queue.insert(insertAt, std::move(packet));
    }

    void PacketPacer::DropExpiredFrontPacketsLocked(uint64_t nowUs) {
        auto dropExpired = [&](std::deque<QueuedPacket>& queue, bool highPriority) {
            while (!queue.empty()) {
                const QueuedPacket& candidate = queue.front();
                if (candidate.deadlineUs == 0 ||
                    nowUs <= candidate.deadlineUs) {
                    break;
                }

                stats_.droppedPackets++;
                stats_.deadlineDroppedPackets++;
                if (highPriority) {
                    stats_.highPriorityDeadlineDroppedPackets++;
                }
                else {
                    stats_.normalDeadlineDroppedPackets++;
                }
                stats_.currentQueueDelayMs =
                    static_cast<double>(
                        nowUs - candidate.enqueueTimeUs) / 1000.0;
                stats_.maxQueueDelayMs =
                    (std::max)(
                        stats_.maxQueueDelayMs,
                        stats_.currentQueueDelayMs
                    );
                queue.pop_front();
            }

            stats_.queuedPackets = QueueSizeLocked();
            stats_.highPriorityQueuedPackets =
                static_cast<uint32_t>(highPriorityQueue_.size());
            stats_.normalQueuedPackets =
                static_cast<uint32_t>(normalQueue_.size());
        };

        dropExpired(highPriorityQueue_, true);
        dropExpired(normalQueue_, false);
    }

    uint32_t PacketPacer::GetRepairTargetBitrateBpsLocked() const {
        const uint32_t videoTargetBps =
            (std::max)(
                targetBitrateBps_.load(std::memory_order_relaxed),
                kMinBitrateBps
            );
        const double ratio =
            ReadEnvDoubleClamped(
                "RNVP_REPAIR_PACING_BUDGET_RATIO",
                0.20,
                0.0,
                1.0);
        if (ratio <= 0.0) {
            return 0;
        }

        const uint32_t minBps =
            ReadEnvUInt32Clamped(
                "RNVP_REPAIR_PACING_MIN_BPS",
                250000u,
                0u,
                5000000u);
        const uint32_t maxBps =
            ReadEnvUInt32Clamped(
                "RNVP_REPAIR_PACING_MAX_BPS",
                1200000u,
                100000u,
                10000000u);
        const uint32_t ratioBps =
            static_cast<uint32_t>(
                std::lround(
                    static_cast<double>(videoTargetBps) * ratio));

        return std::clamp<uint32_t>(
            (std::max)(ratioBps, minBps),
            0u,
            maxBps);
    }

} // namespace net
