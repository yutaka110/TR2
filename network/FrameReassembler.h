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
        std::vector<uint16_t> missingChunkIndices;
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

        void Clear();

    private:
        struct ParsedDataPacket {
            bool isRnvp = false;

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
        };

        struct PendingFrame {
            uint32_t frameId = 0;
            uint32_t streamId = 0;

            CodecType codecType = CodecType::Unknown;

            uint16_t chunkCount = 0;
            uint16_t receivedCount = 0;
            uint32_t latestSequence = 0;

            uint64_t firstReceiveTimeUs = 0;
            uint64_t sendTimeUs = 0;
            uint64_t lastUpdateTimeUs = 0;
            uint64_t lastNackTimeUs = 0;
            uint32_t nackCount = 0;
            bool nackSent = false;

            std::vector<std::vector<uint8_t>> chunks;
            std::vector<bool> received;
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

        FrameAckInfo BuildAckInfoFromPendingFrame(const PendingFrame& frame) const;

        void CleanupOldFrames(uint64_t nowUs);
        bool IsRecentlyCompletedFrame(uint64_t frameKey) const;
        void TrackCompletedFrame(uint64_t frameKey);

        uint64_t MakeFrameKey(uint32_t streamId, uint32_t frameId) const;

        void TrackRnvpSequence(const ParsedDataPacket& packet);

    private:
        std::mutex mutex_;
        std::unordered_map<uint64_t, PendingFrame> pendingFrames_;
        std::deque<uint64_t> recentlyCompletedFrames_;
        std::unordered_set<uint64_t> recentlyCompletedFrameSet_;

        NetworkStats* stats_ = nullptr;

        // RNVP sequence観測用
        bool hasLastRnvpSequence_ = false;
        uint32_t lastRnvpSequence_ = 0;

        static constexpr uint64_t kFrameTimeoutUs = 1000000; // 1秒
        static constexpr size_t kCompletedFrameHistoryLimit = 128;
    };

} // namespace net
