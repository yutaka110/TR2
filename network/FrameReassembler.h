#pragma once

#include "PacketProtocol.h"
#include "NetworkStats.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace net {

    struct CompletedFrame {
        uint32_t frameId = 0;
        uint32_t streamId = 0;

        CodecType codecType = CodecType::Unknown;

        uint64_t sendTimeUs = 0;
        uint64_t receiveTimeUs = 0;

        std::vector<uint8_t> data;
    };

    struct FrameAckInfo {
        bool valid = false;

        uint32_t frameId = 0;
        uint32_t streamId = 0;

        uint32_t latestSequence = 0;

        uint32_t receivedChunkCount = 0;
        uint32_t missingChunkCount = 0;
        uint16_t chunkCount = 0;
        CodecType codecType = CodecType::Unknown;
        bool keyFrame = false;
        bool largeFrame = false;
        std::vector<uint16_t> missingChunkIndices;
    };

    enum class FrameRecoveryState {
        Waiting,
        NackSent,
        Recovered,
        Expired,
        KeyFrameRequested
    };

    enum class RetiredFrameOutcome {
        Completed,
        Expired,
        Rejected
    };

    struct FrameRecoveryActions {
        std::vector<FrameAckInfo> nackAckInfos;
        uint32_t expiredFrameCount = 0;
        uint32_t expiredAfterNackCount = 0;
        uint32_t expiredMissingChunkCount = 0;
        uint32_t lastExpiredFrameId = 0;
        uint32_t lastExpiredStreamId = 0;
        uint32_t suppressedFrameCount = 0;
        uint32_t suppressedMissingChunkCount = 0;
        uint32_t predictedUsefulNackCount = 0;
        uint32_t deferredLikelyArrivalCount = 0;
        uint32_t skippedTooLateCount = 0;
        uint32_t requestedChunkBudget = 0;
    };

    class FrameReassembler {
    public:
        explicit FrameReassembler(NetworkStats* stats = nullptr);

        void SetStats(NetworkStats* stats);

        // 旧PacketHeader / RNVP v1 Header の両方を受け取る
        std::optional<CompletedFrame> PushPacket(
            const uint8_t* packetData,
            size_t packetSize,
            uint64_t receiveTimeUs
        );

        std::optional<CompletedFrame> PushPacketWithAckInfo(
            const uint8_t* packetData,
            size_t packetSize,
            uint64_t receiveTimeUs,
            FrameAckInfo* outAckInfo
        );

        std::vector<FrameAckInfo> CollectExpiredAckInfos(
            uint64_t nowUs,
            uint64_t deadlineUs,
            uint64_t nackIntervalUs,
            uint32_t maxNacksPerFrame
        );

        FrameRecoveryActions CollectRecoveryActions(
            uint64_t nowUs,
            uint64_t nackDeadlineUs,
            uint64_t nackIntervalUs,
            uint64_t recoveryExpireUs,
            uint64_t minRecoverySlackUs,
            uint32_t maxNacksPerFrame
        );
        bool RefreshNackAckInfo(
            const FrameAckInfo& candidate,
            uint64_t nowUs,
            uint64_t minRecoverySlackUs,
            FrameAckInfo& outAckInfo
        );

        void Clear();

    private:
        struct ParsedDataPacket {
            bool isRnvp = false;
            bool isFec = false;
            bool isRetransmit = false;

            uint32_t sequence = 0;
            uint32_t streamId = 0;

            uint32_t frameId = 0;
            uint16_t chunkIndex = 0;
            uint16_t chunkCount = 0;

            uint64_t sendTimeUs = 0;

            uint32_t payloadSize = 0;
            uint32_t flags = 0;

            CodecType codecType = CodecType::Unknown;

            const uint8_t* payload = nullptr;

            uint32_t fecFramePayloadBytes = 0;
            uint16_t fecParityPayloadBytes = 0;
            uint16_t fecProtectedChunkCount = 0;
        };

        struct FecParityGroup {
            uint16_t startChunkIndex = 0;
            uint16_t protectedChunkCount = 0;
            uint16_t parityPayloadBytes = 0;
            std::vector<uint8_t> parity;
        };

        struct PendingFrame {
            uint32_t frameId = 0;
            uint32_t streamId = 0;

            CodecType codecType = CodecType::Unknown;

            uint16_t chunkCount = 0;
            uint16_t receivedCount = 0;
            uint32_t latestSequence = 0;
            bool keyFrame = false;

            uint64_t firstReceiveTimeUs = 0;
            uint64_t sendTimeUs = 0;
            uint64_t previousUpdateTimeUs = 0;
            uint64_t lastUpdateTimeUs = 0;
            uint64_t recentArrivalIntervalUs = 0;
            uint64_t recoveryExpireTimeUs = 0;
            uint64_t lastNackTimeUs = 0;
            uint32_t nackCount = 0;
            uint32_t keySmallMissingDeadlineRescueCount = 0;
            uint32_t keySmallMissingDeadlineRescueMissingChunks = 0;
            uint32_t fecGraceSuppressionCount = 0;
            uint32_t likelyArrivalSuppressionCount = 0;
            bool nackSent = false;
            uint32_t lastNackMissingChunks = 0;
            uint32_t nackRequestedChunks = 0;
            uint32_t postNackReceivedChunks = 0;
            uint32_t retransmitReceivedChunks = 0;
            uint32_t retransmitDuplicatePackets = 0;
            uint32_t lastPacketSequence = 0;
            uint16_t lastPacketChunkIndex = 0;
            uint32_t lastRetransmitSequence = 0;
            uint16_t lastRetransmitChunkIndex = 0;
            uint32_t fecParityPackets = 0;
            uint32_t fecRecoveredChunks = 0;
            FrameRecoveryState recoveryState =
                FrameRecoveryState::Waiting;

            std::vector<std::vector<uint8_t>> chunks;
            std::vector<bool> received;

            uint32_t fecFramePayloadBytes = 0;
            std::vector<FecParityGroup> fecParityGroups;
        };

        struct RetiredFrameRecord {
            uint64_t frameKey = 0;
            uint32_t frameId = 0;
            uint32_t streamId = 0;
            CodecType codecType = CodecType::Unknown;
            bool keyFrame = false;
            bool largeFrame = false;
            uint16_t chunkCount = 0;
            uint16_t receivedCount = 0;
            uint32_t nackCount = 0;
            uint32_t lastNackMissingChunks = 0;
            uint32_t retransmitReceivedChunks = 0;
            uint32_t retransmitDuplicatePackets = 0;
            uint64_t sendTimeUs = 0;
            uint64_t firstReceiveTimeUs = 0;
            uint64_t eventTimeUs = 0;
            RetiredFrameOutcome outcome = RetiredFrameOutcome::Completed;
        };

    private:
        bool TryParseDataPacket(
            const uint8_t* packetData,
            size_t packetSize,
            ParsedDataPacket& outPacket
        ) const;

        std::optional<CompletedFrame> TryBuildFrame(
            PendingFrame& frame,
            uint64_t receiveTimeUs
        );
        bool ValidateCompletedFramePayload(
            const CompletedFrame& frame
        ) const;

        uint32_t TryRecoverMissingChunksWithFec(PendingFrame& frame);
        bool StoreFecParity(
            PendingFrame& frame,
            const ParsedDataPacket& packet
        );
        size_t ExpectedChunkSize(
            const PendingFrame& frame,
            uint16_t chunkIndex
        ) const;

        FrameAckInfo BuildAckInfoFromPendingFrame(const PendingFrame& frame) const;
        void EmitFrameRecoveryOutcome(
            const PendingFrame& frame,
            const char* eventName,
            const char* outcome,
            uint32_t missingChunks,
            uint64_t eventTimeUs,
            uint32_t packetSequence = 0,
            uint16_t packetChunkIndex = 0,
            bool packetWasRetransmit = false
        ) const;

        void CleanupOldFrames(uint64_t nowUs);
        void RetireFrame(uint64_t frameKey);
        void RetireFrame(
            uint64_t frameKey,
            const PendingFrame& frame,
            RetiredFrameOutcome outcome,
            uint64_t eventTimeUs
        );
        bool IsRecentlyCompletedFrame(uint64_t frameKey) const;
        const RetiredFrameRecord* FindRetiredFrame(
            uint64_t frameKey
        ) const;
        void TrackRetiredFrame(
            uint64_t frameKey,
            const PendingFrame& frame,
            RetiredFrameOutcome outcome,
            uint64_t eventTimeUs
        );
        void EmitRetiredFrameRetransmitOutcome(
            const RetiredFrameRecord& retired,
            const ParsedDataPacket& packet,
            uint64_t receiveTimeUs
        ) const;
        bool ShouldSuppressNackForFecGrace(
            const PendingFrame& frame,
            const FrameAckInfo& ackInfo,
            uint64_t nowUs,
            uint64_t minRecoverySlackUs
        ) const;
        bool ShouldDeferNackForLikelyArrival(
            const PendingFrame& frame,
            const FrameAckInfo& ackInfo,
            uint64_t nowUs,
            uint64_t minRecoverySlackUs
        ) const;
        uint32_t CalculateNackRequestedChunkBudget(
            const PendingFrame& frame,
            uint64_t nowUs,
            uint64_t minRecoverySlackUs
        ) const;
        bool ShouldSkipNackAsTooLate(
            const PendingFrame& frame,
            const FrameAckInfo& ackInfo,
            uint64_t nowUs,
            uint64_t minRecoverySlackUs,
            uint32_t requestedChunkBudget
        ) const;
        void UpdateFrameArrivalTiming(
            PendingFrame& frame,
            uint64_t receiveTimeUs
        ) const;

        uint64_t MakeFrameKey(uint32_t streamId, uint32_t frameId) const;

        void TrackRnvpSequence(const ParsedDataPacket& packet);

    private:
        std::mutex mutex_;
        std::unordered_map<uint64_t, PendingFrame> pendingFrames_;
        std::deque<uint64_t> recentlyCompletedFrames_;
        std::unordered_set<uint64_t> recentlyCompletedFrameSet_;
        std::deque<RetiredFrameRecord> retiredFrames_;
        std::unordered_map<uint64_t, size_t> retiredFrameIndex_;

        NetworkStats* stats_ = nullptr;

        // RNVP sequence観測用
        bool hasLastRnvpSequence_ = false;
        uint32_t lastRnvpSequence_ = 0;
        std::unordered_set<uint32_t> pendingMissingRnvpSequences_;

        static constexpr uint64_t kFrameTimeoutUs = 1000000; // 1秒
        static constexpr uint64_t kDefaultRecoveryExpireUs = 150000;
        static constexpr size_t kCompletedFrameHistoryLimit = 128;
        static constexpr uint32_t kMaxTrackedRnvpSequenceGap = 65536;
    };

} // namespace net
