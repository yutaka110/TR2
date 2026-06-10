#include "FrameReassembler.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

namespace net {
    namespace {
        constexpr uint16_t kH264LargeAuChunkThreshold = 8;
        constexpr uint64_t kH264DeltaRecoveryExpireUs = 140000;
        constexpr uint64_t kH264LargeRecoveryExtraUs = 30000;
        constexpr uint64_t kH264KeyRecoveryExtraUs = 30000;
        constexpr uint64_t kNackFecGraceUs = 5000;
        constexpr uint64_t kNackLikelyArrivalGraceUs = 4000;
        constexpr uint64_t kRecentArrivalWindowUs = 7000;
        constexpr uint64_t kRepairChunkBudgetUs = 2500;
        constexpr uint64_t kTooLateDecisionWindowUs = 45000;
    }

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

        if (const RetiredFrameRecord* retired =
                FindRetiredFrame(frameKey)) {
            if (parsed.isRetransmit) {
                EmitRetiredFrameRetransmitOutcome(
                    *retired,
                    parsed,
                    receiveTimeUs);
            }
            if (stats_ && !parsed.isFec) {
                DuplicatePacketKind duplicateKind =
                    parsed.isRetransmit
                        ? DuplicatePacketKind::LateRetransmitAfterCompleted
                        : DuplicatePacketKind::LateOriginalAfterCompleted;
                if (retired->outcome == RetiredFrameOutcome::Expired) {
                    duplicateKind = DuplicatePacketKind::LateAfterExpired;
                }
                else if (retired->outcome == RetiredFrameOutcome::Rejected) {
                    duplicateKind = DuplicatePacketKind::LateAfterRejected;
                }
                stats_->OnDuplicatePacket(duplicateKind);
            }

            if (outAckInfo &&
                parsed.isRnvp &&
                !parsed.isFec &&
                retired->outcome == RetiredFrameOutcome::Completed) {
                FrameAckInfo ack{};
                ack.valid = true;
                ack.frameId = parsed.frameId;
                ack.streamId = parsed.streamId;
                ack.latestSequence = parsed.sequence;
                ack.receivedChunkCount = parsed.chunkCount;
                ack.missingChunkCount = 0;
                ack.chunkCount = parsed.chunkCount;
                ack.codecType = parsed.codecType;
                ack.keyFrame =
                    (parsed.flags & PacketFlag_KeyFrame) != 0;
                ack.largeFrame =
                    parsed.codecType == CodecType::H264 &&
                    parsed.chunkCount >= kH264LargeAuChunkThreshold;
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
            frame.lastPacketSequence = parsed.sequence;
            frame.lastPacketChunkIndex = parsed.chunkIndex;
            frame.keyFrame =
                (parsed.flags & PacketFlag_KeyFrame) != 0;

            frame.firstReceiveTimeUs = receiveTimeUs;
            frame.sendTimeUs = parsed.sendTimeUs;
            frame.lastUpdateTimeUs = receiveTimeUs;
            frame.recoveryExpireTimeUs = 0;
            frame.recoveryState = FrameRecoveryState::Waiting;

            frame.chunks.resize(parsed.chunkCount);
            frame.received.resize(parsed.chunkCount, false);
        }

        if (frame.chunkCount != parsed.chunkCount) {
            EmitFrameRecoveryOutcome(
                frame,
                "chunk-count-mismatch",
                "rejected",
                frame.chunkCount - frame.receivedCount,
                receiveTimeUs);
            if (stats_) {
                stats_->OnDroppedFrame();
            }

            pendingFrames_.erase(frameKey);
            return std::nullopt;
        }

        if (parsed.isFec) {
            if (StoreFecParity(frame, parsed)) {
                frame.fecParityPackets++;
                if (stats_) {
                    stats_->OnFecParityPacket();
                }
                EmitFrameRecoveryOutcome(
                    frame,
                    "fec-parity",
                    "pending",
                    frame.chunkCount - frame.receivedCount,
                    receiveTimeUs);
            }
            UpdateFrameArrivalTiming(frame, receiveTimeUs);

            const uint32_t fecRecoveredChunks =
                TryRecoverMissingChunksWithFec(frame);
            if (fecRecoveredChunks > 0) {
                frame.fecRecoveredChunks += fecRecoveredChunks;
                if (stats_) {
                    stats_->OnFecRecoveredFrame(fecRecoveredChunks);
                }
                EmitFrameRecoveryOutcome(
                    frame,
                    "fec-recovered",
                    "pending",
                    frame.chunkCount - frame.receivedCount,
                    receiveTimeUs);
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
                EmitFrameRecoveryOutcome(
                    frame,
                    completed ? "completed" : "rejected",
                    completed ? "completed" : "rejected",
                    0,
                    receiveTimeUs);

                RetireFrame(
                    frameKey,
                    frame,
                    completed
                        ? RetiredFrameOutcome::Completed
                        : RetiredFrameOutcome::Rejected,
                    receiveTimeUs);
                pendingFrames_.erase(frameKey);

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

        frame.lastPacketSequence = parsed.sequence;
        frame.lastPacketChunkIndex = parsed.chunkIndex;

        if (frame.received[parsed.chunkIndex]) {
            if (parsed.isRetransmit) {
                frame.retransmitDuplicatePackets++;
                frame.lastRetransmitSequence = parsed.sequence;
                frame.lastRetransmitChunkIndex = parsed.chunkIndex;
                EmitFrameRecoveryOutcome(
                    frame,
                    "retransmit-duplicate",
                    "pending",
                    frame.chunkCount - frame.receivedCount,
                    receiveTimeUs,
                    parsed.sequence,
                    parsed.chunkIndex,
                    true);
            }
            if (stats_) {
                stats_->OnDuplicatePacket(
                    parsed.isRetransmit
                        ? DuplicatePacketKind::Retransmit
                        : DuplicatePacketKind::Original);
            }

            if (outAckInfo && parsed.isRnvp) {
                *outAckInfo = BuildAckInfoFromPendingFrame(frame);
            }

            return std::nullopt;
        }

        if (frame.nackSent) {
            frame.postNackReceivedChunks++;
        }
        if (parsed.isRetransmit) {
            frame.retransmitReceivedChunks++;
            frame.lastRetransmitSequence = parsed.sequence;
            frame.lastRetransmitChunkIndex = parsed.chunkIndex;
            EmitFrameRecoveryOutcome(
                frame,
                "retransmit-arrived",
                "pending",
                frame.chunkCount - frame.receivedCount,
                receiveTimeUs,
                parsed.sequence,
                parsed.chunkIndex,
                true);
        }

        frame.chunks[parsed.chunkIndex].assign(
            parsed.payload,
            parsed.payload + parsed.payloadSize
        );

        frame.received[parsed.chunkIndex] = true;
        frame.receivedCount++;
        frame.latestSequence = parsed.sequence;
        UpdateFrameArrivalTiming(frame, receiveTimeUs);

        const uint32_t fecRecoveredChunks =
            TryRecoverMissingChunksWithFec(frame);
        if (fecRecoveredChunks > 0) {
            frame.fecRecoveredChunks += fecRecoveredChunks;
            if (stats_) {
                stats_->OnFecRecoveredFrame(fecRecoveredChunks);
            }
            EmitFrameRecoveryOutcome(
                frame,
                "fec-recovered",
                "pending",
                frame.chunkCount - frame.receivedCount,
                receiveTimeUs);
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
            EmitFrameRecoveryOutcome(
                frame,
                completed ? "completed" : "rejected",
                completed ? "completed" : "rejected",
                0,
                receiveTimeUs);

            RetireFrame(
                frameKey,
                frame,
                completed
                    ? RetiredFrameOutcome::Completed
                    : RetiredFrameOutcome::Rejected,
                receiveTimeUs);
            pendingFrames_.erase(frameKey);

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

            const bool h264Frame = frame.codecType == CodecType::H264;
            const bool largeH264Frame =
                h264Frame &&
                frame.chunkCount >= kH264LargeAuChunkThreshold;
            const bool protectedH264Frame =
                h264Frame && (frame.keyFrame || largeH264Frame);

            uint64_t effectiveRecoveryExpireUs = recoveryExpireUs;
            if (h264Frame && !protectedH264Frame) {
                effectiveRecoveryExpireUs =
                    (std::min)(
                        effectiveRecoveryExpireUs,
                        kH264DeltaRecoveryExpireUs);
            }
            if (largeH264Frame) {
                effectiveRecoveryExpireUs += kH264LargeRecoveryExtraUs;
            }
            if (frame.keyFrame && h264Frame) {
                effectiveRecoveryExpireUs += kH264KeyRecoveryExtraUs;
            }

            if (frame.recoveryExpireTimeUs == 0) {
                uint64_t recoveryBaseTimeUs = frame.sendTimeUs;
                if (recoveryBaseTimeUs == 0 ||
                    frame.firstReceiveTimeUs < recoveryBaseTimeUs) {
                    recoveryBaseTimeUs = frame.firstReceiveTimeUs;
                }
                frame.recoveryExpireTimeUs =
                    recoveryBaseTimeUs + effectiveRecoveryExpireUs;
            }

            const uint64_t effectiveNackDeadlineUs = nackDeadlineUs;
            const uint64_t effectiveNackIntervalUs = nackIntervalUs;
            const uint32_t effectiveMaxNacksPerFrame =
                protectedH264Frame
                ? maxNacksPerFrame
                : maxNacksPerFrame;

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
                EmitFrameRecoveryOutcome(
                    frame,
                    expiredByLifetime ? "expired" : "expired-no-slack",
                    "expired",
                    expiredInfo.missingChunkCount,
                    nowUs);

                if (stats_) {
                    stats_->OnDeadlineNackExpiredFrame(
                        expiredInfo.missingChunkCount,
                        frame.nackSent,
                        expiredInfo.codecType,
                        expiredInfo.keyFrame,
                        expiredInfo.largeFrame
                    );
                }

                RetireFrame(
                    frameKey,
                    frame,
                    RetiredFrameOutcome::Expired,
                    nowUs);
                it = pendingFrames_.erase(it);
                continue;
            }

            if (frame.nackCount >= effectiveMaxNacksPerFrame ||
                nowUs <= frame.firstReceiveTimeUs ||
                nowUs - frame.firstReceiveTimeUs < effectiveNackDeadlineUs) {
                ++it;
                continue;
            }

            if (frame.lastNackTimeUs != 0 &&
                nowUs > frame.lastNackTimeUs &&
                nowUs - frame.lastNackTimeUs < effectiveNackIntervalUs) {
                ++it;
                continue;
            }

            FrameAckInfo ackInfo = BuildAckInfoFromPendingFrame(frame);
            if (!ackInfo.valid || ackInfo.missingChunkCount == 0) {
                ++it;
                continue;
            }

            if (ShouldSuppressNackForFecGrace(
                    frame,
                    ackInfo,
                    nowUs,
                    minRecoverySlackUs)) {
                frame.fecGraceSuppressionCount++;
                frame.lastNackTimeUs = nowUs;
                actions.suppressedFrameCount++;
                actions.suppressedMissingChunkCount +=
                    ackInfo.missingChunkCount;
                if (stats_) {
                    stats_->OnNackSuppressed(
                        "fec-grace",
                        ackInfo.missingChunkCount);
                }
                EmitFrameRecoveryOutcome(
                    frame,
                    "nack-suppressed-fec-grace",
                    "pending",
                    ackInfo.missingChunkCount,
                    nowUs);
                if (effectiveNackIntervalUs > kNackFecGraceUs) {
                    frame.lastNackTimeUs =
                        nowUs - effectiveNackIntervalUs + kNackFecGraceUs;
                }
                ++it;
                continue;
            }

            if (ShouldDeferNackForLikelyArrival(
                    frame,
                    ackInfo,
                    nowUs,
                    minRecoverySlackUs)) {
                frame.likelyArrivalSuppressionCount++;
                frame.lastNackTimeUs = nowUs;
                actions.suppressedFrameCount++;
                actions.suppressedMissingChunkCount +=
                    ackInfo.missingChunkCount;
                actions.deferredLikelyArrivalCount++;
                if (stats_) {
                    stats_->OnNackSuppressed(
                        "likely-arrival",
                        ackInfo.missingChunkCount);
                    stats_->OnNackShapingDecision(
                        "deferred-likely-arrival",
                        ackInfo.missingChunkCount,
                        0,
                        false);
                }
                EmitFrameRecoveryOutcome(
                    frame,
                    "nack-deferred-likely-arrival",
                    "pending",
                    ackInfo.missingChunkCount,
                    nowUs);
                if (effectiveNackIntervalUs > kNackLikelyArrivalGraceUs) {
                    frame.lastNackTimeUs =
                        nowUs - effectiveNackIntervalUs +
                            kNackLikelyArrivalGraceUs;
                }
                ++it;
                continue;
            }

            const uint32_t requestedChunkBudget =
                CalculateNackRequestedChunkBudget(
                    frame,
                    nowUs,
                    minRecoverySlackUs);
            if (ShouldSkipNackAsTooLate(
                    frame,
                    ackInfo,
                    nowUs,
                    minRecoverySlackUs,
                    requestedChunkBudget)) {
                actions.suppressedFrameCount++;
                actions.suppressedMissingChunkCount +=
                    ackInfo.missingChunkCount;
                actions.skippedTooLateCount++;
                actions.expiredFrameCount++;
                actions.expiredAfterNackCount += frame.nackSent ? 1 : 0;
                actions.expiredMissingChunkCount +=
                    ackInfo.missingChunkCount;
                actions.lastExpiredFrameId = frame.frameId;
                actions.lastExpiredStreamId = frame.streamId;
                frame.recoveryState = FrameRecoveryState::Expired;
                if (stats_) {
                    stats_->OnNackSuppressed(
                        "too-late",
                        ackInfo.missingChunkCount);
                    stats_->OnNackShapingDecision(
                        "skipped-too-late",
                        ackInfo.missingChunkCount,
                        requestedChunkBudget,
                        false);
                    stats_->OnDeadlineNackExpiredFrame(
                        ackInfo.missingChunkCount,
                        frame.nackSent,
                        ackInfo.codecType,
                        ackInfo.keyFrame,
                        ackInfo.largeFrame);
                }
                EmitFrameRecoveryOutcome(
                    frame,
                    "nack-skipped-too-late",
                    "expired",
                    ackInfo.missingChunkCount,
                    nowUs);
                RetireFrame(
                    frameKey,
                    frame,
                    RetiredFrameOutcome::Expired,
                    nowUs);
                it = pendingFrames_.erase(it);
                continue;
            }

            frame.lastNackTimeUs = nowUs;
            frame.nackCount++;
            frame.nackSent = true;
            frame.nackRequestedChunks +=
                static_cast<uint32_t>(ackInfo.missingChunkIndices.size());
            frame.recoveryState = FrameRecoveryState::NackSent;
            actions.predictedUsefulNackCount++;
            actions.requestedChunkBudget +=
                (std::min)(
                    ackInfo.missingChunkCount,
                    requestedChunkBudget);
            if (stats_) {
                stats_->OnNackShapingDecision(
                    "predicted-useful",
                    ackInfo.missingChunkCount,
                    (std::min)(
                        ackInfo.missingChunkCount,
                        requestedChunkBudget),
                    true);
            }
            EmitFrameRecoveryOutcome(
                frame,
                "nack-sent",
                "pending",
                ackInfo.missingChunkCount,
                nowUs);
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
        retiredFrames_.clear();
        retiredFrameIndex_.clear();

        hasLastRnvpSequence_ = false;
        lastRnvpSequence_ = 0;
        pendingMissingRnvpSequences_.clear();
    }

    bool FrameReassembler::RefreshNackAckInfo(
        const FrameAckInfo& candidate,
        uint64_t nowUs,
        uint64_t minRecoverySlackUs,
        FrameAckInfo& outAckInfo
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        const uint64_t frameKey =
            MakeFrameKey(candidate.streamId, candidate.frameId);
        const auto pending = pendingFrames_.find(frameKey);
        if (pending == pendingFrames_.end()) {
            if (stats_) {
                const char* reason =
                    FindRetiredFrame(frameKey) != nullptr
                    ? "preflight-retired"
                    : "preflight-missing-frame";
                stats_->OnNackSuppressed(
                    reason,
                    candidate.missingChunkCount);
            }
            return false;
        }

        PendingFrame& frame = pending->second;
        outAckInfo = BuildAckInfoFromPendingFrame(frame);
        if (!outAckInfo.valid || outAckInfo.missingChunkCount == 0) {
            if (stats_) {
                stats_->OnNackSuppressed(
                    "preflight-completed",
                    candidate.missingChunkCount);
            }
            EmitFrameRecoveryOutcome(
                frame,
                "nack-suppressed-preflight-completed",
                "pending",
                0,
                nowUs);
            return false;
        }

        if (outAckInfo.missingChunkCount < candidate.missingChunkCount &&
            stats_) {
            stats_->OnNackSuppressed(
                "preflight-shrunk",
                candidate.missingChunkCount - outAckInfo.missingChunkCount);
        }

        if (ShouldSuppressNackForFecGrace(
                frame,
                outAckInfo,
                nowUs,
                minRecoverySlackUs)) {
            frame.fecGraceSuppressionCount++;
            frame.lastNackTimeUs = nowUs;
            if (stats_) {
                stats_->OnNackSuppressed(
                    "preflight-fec-grace",
                    outAckInfo.missingChunkCount);
            }
            EmitFrameRecoveryOutcome(
                frame,
                "nack-suppressed-preflight-fec-grace",
                "pending",
                outAckInfo.missingChunkCount,
                nowUs);
            return false;
        }

        if (ShouldDeferNackForLikelyArrival(
                frame,
                outAckInfo,
                nowUs,
                minRecoverySlackUs)) {
            frame.likelyArrivalSuppressionCount++;
            frame.lastNackTimeUs = nowUs;
            if (stats_) {
                stats_->OnNackSuppressed(
                    "preflight-likely-arrival",
                    outAckInfo.missingChunkCount);
                stats_->OnNackShapingDecision(
                    "preflight-deferred-likely-arrival",
                    outAckInfo.missingChunkCount,
                    0,
                    false);
            }
            EmitFrameRecoveryOutcome(
                frame,
                "nack-deferred-preflight-likely-arrival",
                "pending",
                outAckInfo.missingChunkCount,
                nowUs);
            return false;
        }

        return true;
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
            outPacket.isRetransmit =
                HasPacketFlag(header.flags, PacketFlag_Retransmit);
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

        if (!ValidateCompletedFramePayload(completed)) {
            return std::nullopt;
        }

        return completed;
    }

    bool FrameReassembler::ValidateCompletedFramePayload(
        const CompletedFrame& frame
    ) const {
        if (frame.codecType != CodecType::H264) {
            return true;
        }

        H264AccessUnitPayloadHeader auHeader{};
        if (!DecodeH264AccessUnitPayloadHeader(
                frame.data.data(),
                frame.data.size(),
                auHeader)) {
            if (stats_) {
                stats_->OnH264ReassemblerAuRejected("payload-header-failure");
            }
            static uint32_t logCount = 0;
            if (logCount < 16) {
                OutputDebugStringA(
                    "[FrameReassembler] Dropped H.264 AU: payload header failure.\n");
                ++logCount;
            }
            return false;
        }

        const size_t expectedBytes =
            static_cast<size_t>(auHeader.headerBytes) +
            static_cast<size_t>(auHeader.accessUnitBytes);
        if (frame.data.size() != expectedBytes) {
            if (stats_) {
                stats_->OnH264ReassemblerAuRejected("payload-size-mismatch");
            }
            static uint32_t logCount = 0;
            if (logCount < 16) {
                std::ostringstream oss;
                oss << "[FrameReassembler] Dropped H.264 AU: size mismatch. frameId="
                    << frame.frameId
                    << " bytes=" << frame.data.size()
                    << " expected=" << expectedBytes
                    << "\n";
                OutputDebugStringA(oss.str().c_str());
                ++logCount;
            }
            return false;
        }

        if (auHeader.frameId != frame.frameId) {
            if (stats_) {
                stats_->OnH264ReassemblerAuRejected("frame-id-mismatch");
            }
            static uint32_t logCount = 0;
            if (logCount < 16) {
                std::ostringstream oss;
                oss << "[FrameReassembler] Dropped H.264 AU: frame id mismatch. rnvp="
                    << frame.frameId
                    << " au=" << auHeader.frameId
                    << "\n";
                OutputDebugStringA(oss.str().c_str());
                ++logCount;
            }
            return false;
        }

        if (auHeader.magic == kH264AccessUnitPayloadMagicV2 ||
            auHeader.magic == kH264AccessUnitPayloadMagicV3) {
            const uint8_t* accessUnit =
                frame.data.data() + auHeader.headerBytes;
            const uint32_t calculatedCrc =
                ComputeCrc32(accessUnit, auHeader.accessUnitBytes);
            if (calculatedCrc != auHeader.accessUnitCrc32) {
                if (stats_) {
                    stats_->OnH264ReassemblerAuRejected("crc-mismatch");
                }
                static uint32_t logCount = 0;
                if (logCount < 16) {
                    std::ostringstream oss;
                    oss << "[FrameReassembler] Dropped H.264 AU: CRC mismatch. frameId="
                        << frame.frameId
                        << "\n";
                    OutputDebugStringA(oss.str().c_str());
                    ++logCount;
                }
                return false;
            }
        }

        return true;
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
                EmitFrameRecoveryOutcome(
                    frame,
                    "timeout-expired",
                    "expired",
                    frame.chunkCount - frame.receivedCount,
                    nowUs);
                if (stats_) {
                    stats_->OnDroppedFrame();
                }

                RetireFrame(
                    it->first,
                    frame,
                    RetiredFrameOutcome::Expired,
                    nowUs);
                it = pendingFrames_.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void FrameReassembler::RetireFrame(uint64_t frameKey) {
        recentlyCompletedFrames_.push_back(frameKey);
        recentlyCompletedFrameSet_.insert(frameKey);

        while (recentlyCompletedFrames_.size() > kCompletedFrameHistoryLimit) {
            const uint64_t oldFrameKey = recentlyCompletedFrames_.front();
            recentlyCompletedFrames_.pop_front();
            recentlyCompletedFrameSet_.erase(oldFrameKey);
        }
    }

    void FrameReassembler::RetireFrame(
        uint64_t frameKey,
        const PendingFrame& frame,
        RetiredFrameOutcome outcome,
        uint64_t eventTimeUs
    ) {
        RetireFrame(frameKey);
        TrackRetiredFrame(frameKey, frame, outcome, eventTimeUs);
    }

    bool FrameReassembler::IsRecentlyCompletedFrame(uint64_t frameKey) const {
        return recentlyCompletedFrameSet_.find(frameKey) !=
            recentlyCompletedFrameSet_.end();
    }

    const FrameReassembler::RetiredFrameRecord*
        FrameReassembler::FindRetiredFrame(uint64_t frameKey) const {
        const auto found = retiredFrameIndex_.find(frameKey);
        if (found == retiredFrameIndex_.end() ||
            found->second >= retiredFrames_.size()) {
            return nullptr;
        }

        return &retiredFrames_[found->second];
    }

    void FrameReassembler::TrackRetiredFrame(
        uint64_t frameKey,
        const PendingFrame& frame,
        RetiredFrameOutcome outcome,
        uint64_t eventTimeUs
    ) {
        if (retiredFrameIndex_.find(frameKey) !=
            retiredFrameIndex_.end()) {
            return;
        }

        RetiredFrameRecord record{};
        record.frameKey = frameKey;
        record.frameId = frame.frameId;
        record.streamId = frame.streamId;
        record.codecType = frame.codecType;
        record.keyFrame = frame.keyFrame;
        record.largeFrame =
            frame.codecType == CodecType::H264 &&
            frame.chunkCount >= kH264LargeAuChunkThreshold;
        record.chunkCount = frame.chunkCount;
        record.receivedCount = frame.receivedCount;
        record.nackCount = frame.nackCount;
        record.retransmitReceivedChunks =
            frame.retransmitReceivedChunks;
        record.retransmitDuplicatePackets =
            frame.retransmitDuplicatePackets;
        record.sendTimeUs = frame.sendTimeUs;
        record.firstReceiveTimeUs = frame.firstReceiveTimeUs;
        record.eventTimeUs = eventTimeUs;
        record.outcome = outcome;

        retiredFrameIndex_[frameKey] = retiredFrames_.size();
        retiredFrames_.push_back(std::move(record));

        while (retiredFrames_.size() > kCompletedFrameHistoryLimit) {
            retiredFrameIndex_.erase(retiredFrames_.front().frameKey);
            retiredFrames_.pop_front();
            retiredFrameIndex_.clear();
            for (size_t i = 0; i < retiredFrames_.size(); ++i) {
                retiredFrameIndex_[retiredFrames_[i].frameKey] = i;
            }
        }
    }

    void FrameReassembler::EmitRetiredFrameRetransmitOutcome(
        const RetiredFrameRecord& retired,
        const ParsedDataPacket& packet,
        uint64_t receiveTimeUs
    ) const {
        if (stats_ == nullptr) {
            return;
        }

        const char* eventName = "retransmit-late-after-completed";
        const char* outcome = "completed";
        if (retired.outcome == RetiredFrameOutcome::Expired) {
            eventName = "retransmit-late-after-expired";
            outcome = "expired";
        }
        else if (retired.outcome == RetiredFrameOutcome::Rejected) {
            eventName = "retransmit-late-after-rejected";
            outcome = "rejected";
        }

        if (retired.outcome == RetiredFrameOutcome::Completed) {
            stats_->OnRetransmitLateAfterCompletedTiming(
                retired.largeFrame,
                packet.sendTimeUs,
                retired.eventTimeUs,
                receiveTimeUs);
        }

        const uint32_t missingChunks =
            retired.chunkCount > retired.receivedCount
            ? static_cast<uint32_t>(
                retired.chunkCount - retired.receivedCount)
            : 0;

        stats_->OnFrameRecoveryOutcome(
            eventName,
            outcome,
            retired.frameId,
            retired.streamId,
            retired.codecType,
            retired.keyFrame,
            retired.largeFrame,
            retired.chunkCount,
            retired.receivedCount,
            missingChunks,
            0,
            0,
            retired.nackCount,
            0,
            0,
            retired.retransmitReceivedChunks,
            retired.retransmitDuplicatePackets,
            packet.sequence,
            packet.chunkIndex,
            packet.sequence,
            packet.chunkIndex,
            packet.sequence,
            packet.chunkIndex,
            true,
            retired.sendTimeUs,
            retired.firstReceiveTimeUs != 0
                ? retired.firstReceiveTimeUs
                : retired.eventTimeUs,
            receiveTimeUs);
    }

    bool FrameReassembler::ShouldSuppressNackForFecGrace(
        const PendingFrame& frame,
        const FrameAckInfo& ackInfo,
        uint64_t nowUs,
        uint64_t minRecoverySlackUs
    ) const {
        if (frame.fecGraceSuppressionCount > 0 ||
            frame.recoveryExpireTimeUs == 0 ||
            nowUs >= frame.recoveryExpireTimeUs ||
            ackInfo.missingChunkCount == 0) {
            return false;
        }

        const bool h264Frame = frame.codecType == CodecType::H264;
        const bool largeH264Frame =
            h264Frame &&
            frame.chunkCount >= kH264LargeAuChunkThreshold;
        (void)largeH264Frame;
        if (h264Frame && frame.keyFrame) {
            return false;
        }

        const uint64_t requiredSlackUs =
            kNackFecGraceUs + minRecoverySlackUs;
        if (nowUs + requiredSlackUs >= frame.recoveryExpireTimeUs) {
            return false;
        }

        const bool hasFecParity = !frame.fecParityGroups.empty();
        const bool smallLoss = h264Frame
            ? ackInfo.missingChunkCount <= 2
            : ackInfo.missingChunkCount == 1;
        return hasFecParity || smallLoss;
    }

    bool FrameReassembler::ShouldDeferNackForLikelyArrival(
        const PendingFrame& frame,
        const FrameAckInfo& ackInfo,
        uint64_t nowUs,
        uint64_t minRecoverySlackUs
    ) const {
        if (frame.likelyArrivalSuppressionCount > 1 ||
            frame.recoveryExpireTimeUs == 0 ||
            nowUs >= frame.recoveryExpireTimeUs ||
            ackInfo.missingChunkCount == 0 ||
            frame.keyFrame) {
            return false;
        }

        const uint64_t requiredSlackUs =
            kNackLikelyArrivalGraceUs + minRecoverySlackUs;
        if (nowUs + requiredSlackUs >= frame.recoveryExpireTimeUs) {
            return false;
        }

        const bool h264Frame = frame.codecType == CodecType::H264;
        const bool smallLoss = h264Frame
            ? ackInfo.missingChunkCount <= 2
            : ackInfo.missingChunkCount == 1;
        if (!smallLoss) {
            return false;
        }

        const bool recentArrival =
            frame.lastUpdateTimeUs != 0 &&
            nowUs >= frame.lastUpdateTimeUs &&
            nowUs - frame.lastUpdateTimeUs <= kRecentArrivalWindowUs;
        const bool tightArrivalCadence =
            frame.recentArrivalIntervalUs > 0 &&
            frame.recentArrivalIntervalUs <= kRecentArrivalWindowUs;
        const bool hasFecParity = !frame.fecParityGroups.empty();

        return recentArrival || tightArrivalCadence || hasFecParity;
    }

    uint32_t FrameReassembler::CalculateNackRequestedChunkBudget(
        const PendingFrame& frame,
        uint64_t nowUs,
        uint64_t minRecoverySlackUs
    ) const {
        if (frame.recoveryExpireTimeUs == 0 ||
            nowUs >= frame.recoveryExpireTimeUs) {
            return 0;
        }

        uint64_t slackUs = frame.recoveryExpireTimeUs - nowUs;
        if (slackUs <= minRecoverySlackUs) {
            return 0;
        }
        slackUs -= minRecoverySlackUs;
        const uint32_t budget =
            static_cast<uint32_t>(slackUs / kRepairChunkBudgetUs);
        return std::clamp<uint32_t>(budget, 1, 64);
    }

    bool FrameReassembler::ShouldSkipNackAsTooLate(
        const PendingFrame& frame,
        const FrameAckInfo& ackInfo,
        uint64_t nowUs,
        uint64_t minRecoverySlackUs,
        uint32_t requestedChunkBudget
    ) const {
        if (frame.recoveryExpireTimeUs == 0 ||
            nowUs >= frame.recoveryExpireTimeUs ||
            frame.keyFrame ||
            ackInfo.missingChunkCount <= requestedChunkBudget) {
            return false;
        }

        const uint64_t slackUs = frame.recoveryExpireTimeUs - nowUs;
        if (slackUs > kTooLateDecisionWindowUs + minRecoverySlackUs) {
            return false;
        }

        const bool h264Frame = frame.codecType == CodecType::H264;
        const bool largeH264Frame =
            h264Frame &&
            frame.chunkCount >= kH264LargeAuChunkThreshold;
        if (largeH264Frame && frame.nackCount == 0) {
            return false;
        }

        return ackInfo.missingChunkCount >=
            (std::max<uint32_t>)(3, requestedChunkBudget + 2);
    }

    void FrameReassembler::UpdateFrameArrivalTiming(
        PendingFrame& frame,
        uint64_t receiveTimeUs
    ) const {
        if (frame.lastUpdateTimeUs != 0 &&
            receiveTimeUs >= frame.lastUpdateTimeUs) {
            frame.previousUpdateTimeUs = frame.lastUpdateTimeUs;
            frame.recentArrivalIntervalUs =
                receiveTimeUs - frame.lastUpdateTimeUs;
        }
        frame.lastUpdateTimeUs = receiveTimeUs;
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
        ack.chunkCount = frame.chunkCount;
        ack.codecType = frame.codecType;
        ack.keyFrame = frame.keyFrame;
        ack.largeFrame =
            frame.codecType == CodecType::H264 &&
            frame.chunkCount >= kH264LargeAuChunkThreshold;

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

    void FrameReassembler::EmitFrameRecoveryOutcome(
        const PendingFrame& frame,
        const char* eventName,
        const char* outcome,
        uint32_t missingChunks,
        uint64_t eventTimeUs,
        uint32_t packetSequence,
        uint16_t packetChunkIndex,
        bool packetWasRetransmit
    ) const {
        if (stats_ == nullptr || frame.chunkCount == 0) {
            return;
        }

        stats_->OnFrameRecoveryOutcome(
            eventName,
            outcome,
            frame.frameId,
            frame.streamId,
            frame.codecType,
            frame.keyFrame,
            frame.codecType == CodecType::H264 &&
                frame.chunkCount >= kH264LargeAuChunkThreshold,
            frame.chunkCount,
            frame.receivedCount,
            missingChunks,
            frame.fecParityPackets,
            frame.fecRecoveredChunks,
            frame.nackCount,
            frame.postNackReceivedChunks,
            frame.nackRequestedChunks,
            frame.retransmitReceivedChunks,
            frame.retransmitDuplicatePackets,
            frame.lastPacketSequence,
            frame.lastPacketChunkIndex,
            frame.lastRetransmitSequence,
            frame.lastRetransmitChunkIndex,
            packetSequence,
            packetChunkIndex,
            packetWasRetransmit,
            frame.sendTimeUs,
            frame.firstReceiveTimeUs,
            eventTimeUs);
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
                if (pendingMissingRnvpSequences_.erase(packet.sequence) > 0) {
                    stats_->OnMissingPacketsRecovered(1);
                }
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
            const uint32_t gapCount = packet.sequence - expectedNext;

            if (gapCount <= kMaxTrackedRnvpSequenceGap) {
                uint64_t newlyMissing = 0;
                for (uint32_t sequence = expectedNext;
                    sequence < packet.sequence;
                    ++sequence) {
                    if (pendingMissingRnvpSequences_.insert(sequence).second) {
                        newlyMissing++;
                    }
                }

                if (stats_) {
                    stats_->OnMissingPackets(newlyMissing);
                }
            }
            else {
                pendingMissingRnvpSequences_.clear();
            }
        }

        lastRnvpSequence_ = packet.sequence;
    }

} // namespace net
