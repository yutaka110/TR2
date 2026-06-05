#include "FrameReassembler.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace net {

    FrameReassembler::FrameReassembler(NetworkStats* stats)
        : stats_(stats) {
    }

    void FrameReassembler::SetStats(NetworkStats* stats) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_ = stats;
    }

    std::optional<CompletedFrame> FrameReassembler::PushPacket(
        const uint8_t* packetData,
        size_t packetSize,
        uint64_t receiveTimeUs
    ) {
        return PushPacketWithAckInfo(
            packetData,
            packetSize,
            receiveTimeUs,
            nullptr
        );
    }

    std::optional<CompletedFrame> FrameReassembler::PushPacketWithAckInfo(
        const uint8_t* packetData,
        size_t packetSize,
        uint64_t receiveTimeUs,
        FrameAckInfo* outAckInfo
    ) {
        if (outAckInfo) {
            *outAckInfo = FrameAckInfo{};
        }

        ParsedDataPacket parsed{};
        if (!TryParseDataPacket(packetData, packetSize, parsed)) {
            return std::nullopt;
        }

        if (stats_) {
            stats_->OnPacketReceived(static_cast<uint32_t>(packetSize));
        }

        std::lock_guard<std::mutex> lock(mutex_);

        TrackRnvpSequence(parsed);
        CleanupOldFrames(receiveTimeUs);

        const uint64_t frameKey = MakeFrameKey(parsed.streamId, parsed.frameId);

        if (IsRecentlyCompletedFrame(frameKey)) {
            if (stats_) {
                stats_->OnDuplicatePacket();
            }

            if (outAckInfo && parsed.isRnvp) {
                FrameAckInfo ack{};
                ack.valid = true;
                ack.frameId = parsed.frameId;
                ack.streamId = parsed.streamId;
                ack.latestSequence = parsed.sequence;
                ack.receivedChunkCount = parsed.chunkCount;
                ack.missingChunkCount = 0;
                *outAckInfo = std::move(ack);
            }

            return std::nullopt;
        }

        auto& frame = pendingFrames_[frameKey];

        if (frame.chunkCount == 0) {
            frame.frameId = parsed.frameId;
            frame.streamId = parsed.streamId;
            frame.codecType = parsed.codecType;

            frame.chunkCount = parsed.chunkCount;
            frame.receivedCount = 0;
            frame.latestSequence = parsed.sequence;

            frame.firstReceiveTimeUs = receiveTimeUs;
            frame.sendTimeUs = parsed.sendTimeUs;
            frame.lastUpdateTimeUs = receiveTimeUs;
            frame.recoveryExpireTimeUs = 0;
            frame.recoveryState = FrameRecoveryState::Waiting;

            frame.chunks.resize(parsed.chunkCount);
            frame.received.resize(parsed.chunkCount, false);
        }

        if (frame.chunkCount != parsed.chunkCount) {
            if (stats_) {
                stats_->OnDroppedFrame();
            }

            pendingFrames_.erase(frameKey);
            return std::nullopt;
        }

        if (parsed.isFec) {
            if (StoreFecParity(frame, parsed) && stats_) {
                stats_->OnFecParityPacket();
            }
            frame.lastUpdateTimeUs = receiveTimeUs;

            const uint32_t fecRecoveredChunks =
                TryRecoverMissingChunksWithFec(frame);
            if (fecRecoveredChunks > 0 && stats_) {
                stats_->OnFecRecoveredFrame(fecRecoveredChunks);
            }

            if (outAckInfo && parsed.isRnvp) {
                *outAckInfo = BuildAckInfoFromPendingFrame(frame);
            }

            if (frame.receivedCount == frame.chunkCount) {
                auto completed = TryBuildFrame(frame, receiveTimeUs);

                if (frame.nackSent && completed && stats_) {
                    stats_->OnDeadlineNackRecoveredFrame();
                }
                frame.recoveryState = FrameRecoveryState::Recovered;

                pendingFrames_.erase(frameKey);
                RetireFrame(frameKey);

                if (completed && stats_) {
                    stats_->OnFrameCompleted(
                        completed->frameId,
                        static_cast<uint64_t>(completed->data.size()),
                        completed->sendTimeUs,
                        completed->receiveTimeUs
                    );
                }

                return completed;
            }

            return std::nullopt;
        }

        if (frame.received[parsed.chunkIndex]) {
            if (stats_) {
                stats_->OnDuplicatePacket();
            }

            if (outAckInfo && parsed.isRnvp) {
                *outAckInfo = BuildAckInfoFromPendingFrame(frame);
            }

            return std::nullopt;
        }

        frame.chunks[parsed.chunkIndex].assign(
            parsed.payload,
            parsed.payload + parsed.payloadSize
        );

        frame.received[parsed.chunkIndex] = true;
        frame.receivedCount++;
        frame.latestSequence = parsed.sequence;
        frame.lastUpdateTimeUs = receiveTimeUs;

        const uint32_t fecRecoveredChunks =
            TryRecoverMissingChunksWithFec(frame);
        if (fecRecoveredChunks > 0 && stats_) {
            stats_->OnFecRecoveredFrame(fecRecoveredChunks);
        }

        if (outAckInfo && parsed.isRnvp) {
            *outAckInfo = BuildAckInfoFromPendingFrame(frame);
        }

        if (frame.receivedCount == frame.chunkCount) {
            auto completed = TryBuildFrame(frame, receiveTimeUs);

            if (frame.nackSent && completed && stats_) {
                stats_->OnDeadlineNackRecoveredFrame();
            }
            frame.recoveryState = FrameRecoveryState::Recovered;

            pendingFrames_.erase(frameKey);
            RetireFrame(frameKey);

            if (completed && stats_) {
                stats_->OnFrameCompleted(
                    completed->frameId,
                    static_cast<uint64_t>(completed->data.size()),
                    completed->sendTimeUs,
                    completed->receiveTimeUs
                );
            }

            return completed;
        }

        return std::nullopt;
    }

    std::vector<FrameAckInfo> FrameReassembler::CollectExpiredAckInfos(
        uint64_t nowUs,
        uint64_t deadlineUs,
        uint64_t nackIntervalUs,
        uint32_t maxNacksPerFrame
    ) {
        FrameRecoveryActions actions = CollectRecoveryActions(
            nowUs,
            deadlineUs,
            nackIntervalUs,
            kDefaultRecoveryExpireUs,
            0,
            maxNacksPerFrame
        );

        return std::move(actions.nackAckInfos);
    }

    FrameRecoveryActions FrameReassembler::CollectRecoveryActions(
        uint64_t nowUs,
        uint64_t nackDeadlineUs,
        uint64_t nackIntervalUs,
        uint64_t recoveryExpireUs,
        uint64_t minRecoverySlackUs,
        uint32_t maxNacksPerFrame
    ) {
        FrameRecoveryActions actions;

        std::lock_guard<std::mutex> lock(mutex_);

        CleanupOldFrames(nowUs);

        for (auto it = pendingFrames_.begin(); it != pendingFrames_.end();) {
            const uint64_t frameKey = it->first;
            PendingFrame& frame = it->second;

            if (frame.chunkCount == 0 ||
                frame.receivedCount >= frame.chunkCount ||
                frame.firstReceiveTimeUs == 0) {
                ++it;
                continue;
            }

            if (frame.recoveryExpireTimeUs == 0) {
                uint64_t recoveryBaseTimeUs = frame.sendTimeUs;
                if (recoveryBaseTimeUs == 0 ||
                    frame.firstReceiveTimeUs < recoveryBaseTimeUs) {
                    recoveryBaseTimeUs = frame.firstReceiveTimeUs;
                }
                frame.recoveryExpireTimeUs =
                    recoveryBaseTimeUs + recoveryExpireUs;
            }

            const bool expiredByLifetime =
                nowUs >= frame.recoveryExpireTimeUs;

            const bool notEnoughRecoverySlack =
                minRecoverySlackUs > 0 &&
                nowUs + minRecoverySlackUs >= frame.recoveryExpireTimeUs;

            if (expiredByLifetime || notEnoughRecoverySlack) {
                const FrameAckInfo expiredInfo =
                    BuildAckInfoFromPendingFrame(frame);

                actions.expiredFrameCount++;
                actions.expiredAfterNackCount += frame.nackSent ? 1 : 0;
                actions.expiredMissingChunkCount +=
                    expiredInfo.missingChunkCount;
                actions.lastExpiredFrameId = frame.frameId;
                actions.lastExpiredStreamId = frame.streamId;

                frame.recoveryState = FrameRecoveryState::Expired;

                if (stats_) {
                    stats_->OnDeadlineNackExpiredFrame(
                        expiredInfo.missingChunkCount,
                        frame.nackSent
                    );
                }

                RetireFrame(frameKey);
                it = pendingFrames_.erase(it);
                continue;
            }

            if (frame.nackCount >= maxNacksPerFrame ||
                nowUs <= frame.firstReceiveTimeUs ||
                nowUs - frame.firstReceiveTimeUs < nackDeadlineUs) {
                ++it;
                continue;
            }

            if (frame.lastNackTimeUs != 0 &&
                nowUs > frame.lastNackTimeUs &&
                nowUs - frame.lastNackTimeUs < nackIntervalUs) {
                ++it;
                continue;
            }

            FrameAckInfo ackInfo = BuildAckInfoFromPendingFrame(frame);
            if (!ackInfo.valid || ackInfo.missingChunkCount == 0) {
                ++it;
                continue;
            }

            frame.lastNackTimeUs = nowUs;
            frame.nackCount++;
            frame.nackSent = true;
            frame.recoveryState = FrameRecoveryState::NackSent;
            actions.nackAckInfos.push_back(std::move(ackInfo));
            ++it;
        }

        return actions;
    }

    void FrameReassembler::Clear() {
        std::lock_guard<std::mutex> lock(mutex_);

        pendingFrames_.clear();
        recentlyCompletedFrames_.clear();
        recentlyCompletedFrameSet_.clear();

        hasLastRnvpSequence_ = false;
        lastRnvpSequence_ = 0;
    }

    bool FrameReassembler::TryParseDataPacket(
        const uint8_t* packetData,
        size_t packetSize,
        ParsedDataPacket& outPacket
    ) const {
        if (!packetData || packetSize < 4) {
            return false;
        }

        const uint32_t magic = ReadU32BE(packetData);

        // ============================================================
        // Legacy PacketHeader
        // ============================================================
        if (magic == kPacketMagic) {
            PacketHeader header;
            if (!DecodeHeader(packetData, packetSize, header)) {
                return false;
            }

            outPacket.isRnvp = false;
            outPacket.sequence = 0;
            outPacket.streamId = 0;

            outPacket.frameId = header.frameId;
            outPacket.chunkIndex = header.chunkIndex;
            outPacket.chunkCount = header.chunkCount;
            outPacket.sendTimeUs = header.sendTimeUs;

            outPacket.payloadSize = header.payloadSize;
            outPacket.flags = header.flags;

            outPacket.codecType = CodecType::Unknown;
            outPacket.payload = packetData + kPacketHeaderSize;

            return true;
        }

        // ============================================================
        // RNVP v1 PacketHeader
        // ============================================================
        if (magic == kRnvpMagic) {
            RnvpHeaderV1 header;
            if (!DecodeRnvpHeaderV1(packetData, packetSize, header)) {
                return false;
            }

            const PacketType packetType =
                static_cast<PacketType>(header.packetType);

            // FrameReassemblerはData専用。
            // ACK / Ping / Pong / Control はUdpReceiver側で処理する。
            if (!IsMediaPacket(packetType)) {
                return false;
            }

            outPacket.isRnvp = true;
            outPacket.isFec = packetType == PacketType::Fec;
            outPacket.sequence = header.sequence;
            outPacket.streamId = header.streamId;

            outPacket.frameId = header.frameId;
            outPacket.chunkIndex = header.chunkIndex;
            outPacket.chunkCount = header.chunkCount;
            outPacket.sendTimeUs = header.sendTimeUs;

            outPacket.payloadSize = header.payloadSize;
            outPacket.flags = header.flags;

            outPacket.codecType = static_cast<CodecType>(header.codecType);
            outPacket.payload = packetData + kRnvpHeaderV1Size;

            if (outPacket.isFec) {
                FecPayloadHeader fecHeader{};
                if (!DecodeFecPayloadHeader(
                    outPacket.payload,
                    header.payloadSize,
                    fecHeader)) {
                    return false;
                }

                const uint32_t groupStart = header.chunkIndex;
                const uint32_t groupEnd =
                    groupStart + fecHeader.protectedChunkCount;
                if (fecHeader.protectedChunkCount == 0 ||
                    groupStart >= header.chunkCount ||
                    groupEnd > header.chunkCount) {
                    return false;
                }

                const size_t expectedChunkCount =
                    (static_cast<size_t>(fecHeader.framePayloadBytes) +
                        kMaxUdpPayloadSize - 1) /
                    kMaxUdpPayloadSize;
                if (expectedChunkCount == 0 ||
                    expectedChunkCount != header.chunkCount) {
                    return false;
                }

                outPacket.fecFramePayloadBytes =
                    fecHeader.framePayloadBytes;
                outPacket.fecParityPayloadBytes =
                    fecHeader.parityPayloadBytes;
                outPacket.fecProtectedChunkCount =
                    fecHeader.protectedChunkCount;
                outPacket.payload =
                    packetData + kRnvpHeaderV1Size + kFecPayloadHeaderSize;
                outPacket.payloadSize = fecHeader.parityPayloadBytes;
            }

            return true;
        }

        return false;
    }

    std::optional<CompletedFrame> FrameReassembler::TryBuildFrame(
        PendingFrame& frame,
        uint64_t receiveTimeUs
    ) {
        size_t totalSize = 0;

        for (const auto& chunk : frame.chunks) {
            totalSize += chunk.size();
        }

        CompletedFrame completed;
        completed.frameId = frame.frameId;
        completed.streamId = frame.streamId;
        completed.codecType = frame.codecType;

        completed.sendTimeUs = frame.sendTimeUs;
        completed.receiveTimeUs = receiveTimeUs;

        completed.data.reserve(totalSize);

        for (const auto& chunk : frame.chunks) {
            completed.data.insert(
                completed.data.end(),
                chunk.begin(),
                chunk.end()
            );
        }

        return completed;
    }

    bool FrameReassembler::StoreFecParity(
        PendingFrame& frame,
        const ParsedDataPacket& packet
    ) {
        if (!packet.isFec ||
            packet.payload == nullptr ||
            packet.payloadSize == 0 ||
            packet.fecParityPayloadBytes != packet.payloadSize) {
            return false;
        }

        const uint64_t maxFramePayloadBytes =
            static_cast<uint64_t>(frame.chunkCount) * kMaxUdpPayloadSize;
        if (packet.fecFramePayloadBytes == 0 ||
            packet.fecFramePayloadBytes > maxFramePayloadBytes ||
            packet.chunkIndex >= frame.chunkCount ||
            packet.fecProtectedChunkCount == 0 ||
            static_cast<uint32_t>(packet.chunkIndex) +
            packet.fecProtectedChunkCount > frame.chunkCount) {
            return false;
        }

        frame.fecFramePayloadBytes = packet.fecFramePayloadBytes;
        FecParityGroup group{};
        group.startChunkIndex = packet.chunkIndex;
        group.protectedChunkCount = packet.fecProtectedChunkCount;
        group.parityPayloadBytes = packet.fecParityPayloadBytes;
        group.parity.assign(
            packet.payload,
            packet.payload + packet.payloadSize
        );

        auto existing = std::find_if(
            frame.fecParityGroups.begin(),
            frame.fecParityGroups.end(),
            [&group](const FecParityGroup& candidate) {
                return candidate.startChunkIndex == group.startChunkIndex &&
                    candidate.protectedChunkCount == group.protectedChunkCount;
            }
        );

        if (existing != frame.fecParityGroups.end()) {
            *existing = std::move(group);
        }
        else {
            frame.fecParityGroups.push_back(std::move(group));
        }
        frame.latestSequence = packet.sequence;
        frame.lastUpdateTimeUs = packet.sendTimeUs;

        return true;
    }

    uint32_t FrameReassembler::TryRecoverMissingChunksWithFec(
        PendingFrame& frame
    ) {
        if (frame.fecFramePayloadBytes == 0 ||
            frame.fecParityGroups.empty()) {
            return 0;
        }

        uint32_t recoveredChunkCount = 0;

        for (const FecParityGroup& group : frame.fecParityGroups) {
            if (group.parity.empty() ||
                group.startChunkIndex >= frame.chunkCount ||
                group.protectedChunkCount == 0 ||
                static_cast<uint32_t>(group.startChunkIndex) +
                group.protectedChunkCount > frame.chunkCount) {
                continue;
            }

            uint16_t missingChunkIndex = 0;
            uint32_t missingInGroup = 0;

            const uint16_t groupEnd = static_cast<uint16_t>(
                group.startChunkIndex + group.protectedChunkCount
            );
            for (uint16_t chunkIndex = group.startChunkIndex;
                chunkIndex < groupEnd;
                ++chunkIndex) {
                if (!frame.received[chunkIndex]) {
                    missingChunkIndex = chunkIndex;
                    missingInGroup++;
                }
            }

            if (missingInGroup != 1) {
                continue;
            }

            const size_t recoveredChunkSize =
                ExpectedChunkSize(frame, missingChunkIndex);
            if (recoveredChunkSize == 0 ||
                recoveredChunkSize > group.parity.size()) {
                continue;
            }

            std::vector<uint8_t> recovered(
                group.parity.begin(),
                group.parity.begin() + recoveredChunkSize
            );

            for (uint16_t chunkIndex = group.startChunkIndex;
                chunkIndex < groupEnd;
                ++chunkIndex) {
                if (!frame.received[chunkIndex]) {
                    continue;
                }

                const std::vector<uint8_t>& chunk = frame.chunks[chunkIndex];
                const size_t xorSize =
                    (std::min)(recovered.size(), chunk.size());
                for (size_t i = 0; i < xorSize; ++i) {
                    recovered[i] ^= chunk[i];
                }
            }

            frame.chunks[missingChunkIndex] = std::move(recovered);
            frame.received[missingChunkIndex] = true;
            frame.receivedCount++;
            recoveredChunkCount++;
        }

        return recoveredChunkCount;
    }

    size_t FrameReassembler::ExpectedChunkSize(
        const PendingFrame& frame,
        uint16_t chunkIndex
    ) const {
        if (frame.fecFramePayloadBytes == 0 ||
            chunkIndex >= frame.chunkCount) {
            return 0;
        }

        const size_t offset =
            static_cast<size_t>(chunkIndex) * kMaxUdpPayloadSize;
        if (offset >= frame.fecFramePayloadBytes) {
            return 0;
        }

        return (std::min)(
            kMaxUdpPayloadSize,
            static_cast<size_t>(frame.fecFramePayloadBytes) - offset
        );
    }

    void FrameReassembler::CleanupOldFrames(uint64_t nowUs) {
        for (auto it = pendingFrames_.begin(); it != pendingFrames_.end();) {
            const auto& frame = it->second;

            if (nowUs > frame.lastUpdateTimeUs &&
                nowUs - frame.lastUpdateTimeUs > kFrameTimeoutUs) {

                // 未完成のままタイムアウトしたフレームはdrop扱い
                if (stats_) {
                    stats_->OnDroppedFrame();
                }

                RetireFrame(it->first);
                it = pendingFrames_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void FrameReassembler::RetireFrame(uint64_t frameKey) {
        TrackCompletedFrame(frameKey);
    }

    bool FrameReassembler::IsRecentlyCompletedFrame(uint64_t frameKey) const {
        return recentlyCompletedFrameSet_.find(frameKey) !=
            recentlyCompletedFrameSet_.end();
    }

    void FrameReassembler::TrackCompletedFrame(uint64_t frameKey) {
        if (recentlyCompletedFrameSet_.find(frameKey) !=
            recentlyCompletedFrameSet_.end()) {
            return;
        }

        recentlyCompletedFrames_.push_back(frameKey);
        recentlyCompletedFrameSet_.insert(frameKey);

        while (recentlyCompletedFrames_.size() > kCompletedFrameHistoryLimit) {
            const uint64_t oldFrameKey = recentlyCompletedFrames_.front();
            recentlyCompletedFrames_.pop_front();
            recentlyCompletedFrameSet_.erase(oldFrameKey);
        }
    }

    uint64_t FrameReassembler::MakeFrameKey(
        uint32_t streamId,
        uint32_t frameId
    ) const {
        return (static_cast<uint64_t>(streamId) << 32) |
            static_cast<uint64_t>(frameId);
    }

    FrameAckInfo FrameReassembler::BuildAckInfoFromPendingFrame(
        const PendingFrame& frame
    ) const {
        FrameAckInfo ack{};
        ack.valid = true;

        ack.frameId = frame.frameId;
        ack.streamId = frame.streamId;

        ack.latestSequence = frame.latestSequence;

        ack.receivedChunkCount = frame.receivedCount;

        uint32_t totalMissingChunks = 0;

        for (uint16_t chunkIndex = 0; chunkIndex < frame.chunkCount; ++chunkIndex) {
            if (!frame.received[chunkIndex]) {
                totalMissingChunks++;

                if (ack.missingChunkIndices.size() < kMaxAckMissingChunkIndices) {
                    ack.missingChunkIndices.push_back(chunkIndex);
                }
            }
        }

        ack.missingChunkCount = totalMissingChunks;

        return ack;
    }

    void FrameReassembler::TrackRnvpSequence(const ParsedDataPacket& packet) {
        if (!packet.isRnvp) {
            return;
        }

        if (!hasLastRnvpSequence_) {
            hasLastRnvpSequence_ = true;
            lastRnvpSequence_ = packet.sequence;
            return;
        }

        // 古いsequence、または同じsequenceが後から来た場合
        // UDPの順序入れ替え、または重複受信として観測する
        if (packet.sequence <= lastRnvpSequence_) {
            if (stats_) {
                stats_->OnReorderedPacket();
            }

            return;
        }

        // sequenceが飛んだ場合、その間の番号を欠番として数える
        //
        // 例：
        // last = 10
        // current = 14
        // missing = 14 - 10 - 1 = 3
        // 欠番: 11, 12, 13
        const uint32_t expectedNext = lastRnvpSequence_ + 1;

        if (packet.sequence > expectedNext) {
            const uint64_t missingCount =
                static_cast<uint64_t>(packet.sequence - expectedNext);

            if (stats_) {
                stats_->OnMissingPackets(missingCount);
            }
        }

        lastRnvpSequence_ = packet.sequence;
    }

} // namespace net
