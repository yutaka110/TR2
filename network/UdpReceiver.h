#pragma once

#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "FrameReassembler.h"
#include "JitterBuffer.h"
#include "NetworkStats.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace net {

    class UdpReceiver {
    public:
        UdpReceiver();
        ~UdpReceiver();

        bool Start(uint16_t listenPort);
        void Stop();

        bool IsRunning() const;

        bool TryPopFrame(CompletedFrame& outFrame);
        bool WaitPopFrame(CompletedFrame& outFrame, uint32_t timeoutMs);

        NetworkStatsSnapshot GetStats() const;
        void ResetStats();

        void SetJitterBufferTargetDelayMs(uint32_t delayMs);
        uint32_t GetJitterBufferTargetDelayMs() const;

        void SetJitterBufferAutoModeEnabled(bool enabled);
        bool IsJitterBufferAutoModeEnabled() const;

        void NotifyDecodeFrame();
        void NotifyDisplayFrame();
        void RequestKeyFrame(
            uint32_t frameId,
            const char* reason = "unspecified"
        );
    private:
        void ReceiveLoop();

        void PushCompletedFrameToJitterBuffer(
            CompletedFrame&& frame,
            uint64_t nowUs
        );

        void DrainReadyJitterBuffer(uint64_t nowUs);
        bool TryPopFrameInternal(
            CompletedFrame& outFrame,
            bool recordEmptyPoll
        );

        void UpdateJitterBufferAutoMode(uint64_t nowUs);
        bool IsFramePastReceiverSafetyDeadline(
            const CompletedFrame& frame,
            uint64_t nowUs
        ) const;
        double CalculateFrameAgeMs(
            const CompletedFrame& frame,
            uint64_t nowUs
        ) const;

        // ============================================================
        // RNVP v1 Control Entry
        // ------------------------------------------------------------
        // RNVPパケットをData / Ping / Pong / Ack / Controlに分岐する。
        // DataはFrameReassemblerへ渡す。
        // ============================================================
        void HandleRnvpPacket(
            const uint8_t* packetData,
            size_t packetSize,
            uint64_t receiveTimeUs,
            const sockaddr_in& fromAddr
        );

        void HandleRnvpPing(
            const RnvpHeaderV1& header,
            const uint8_t* payload,
            size_t payloadSize,
            const sockaddr_in& fromAddr
        );

        void HandleRnvpPong(
            const RnvpHeaderV1& header,
            const uint8_t* payload,
            size_t payloadSize
        );

        void HandleRnvpAck(
            const RnvpHeaderV1& header,
            const uint8_t* payload,
            size_t payloadSize
        );

        void HandleRnvpControl(
            const RnvpHeaderV1& header,
            const uint8_t* payload,
            size_t payloadSize
        );

        void SendRnvpPong(
            const RnvpHeaderV1& pingHeader,
            const PingPayload& pingPayload,
            const sockaddr_in& toAddr
        );

        void SendRnvpAck(
            const RnvpHeaderV1& dataHeader,
            const FrameAckInfo& ackInfo,
            const sockaddr_in& toAddr
        );
        void SendDeadlineNacks(uint64_t nowUs);
        uint64_t CalculateDynamicNackDeadlineUs(
            uint64_t nowUs,
            uint64_t baseDeadlineUs
        );
        void TrackTransportFeedback(
            const RnvpHeaderV1& dataHeader,
            uint64_t receiveTimeUs
        );
        void SendTransportFeedback(
            const sockaddr_in& toAddr,
            bool force
        );

        void SendRnvpControl(
            const RnvpHeaderV1& dataHeader,
            ControlCommand command,
            uint32_t value,
            const sockaddr_in& toAddr
        );

        uint64_t NowMicroseconds() const;
        uint32_t NextRNVPSequence();

    private:
        SOCKET socket_ = INVALID_SOCKET;
        std::atomic<bool> running_ = false;
        std::thread receiveThread_;

        NetworkStats stats_;
        FrameReassembler reassembler_;
        JitterBuffer jitterBuffer_;

        std::atomic<bool> jitterBufferAutoModeEnabled_{ false };
        std::atomic<uint64_t> lastJitterAutoUpdateUs_{ 0 };

        std::mutex frameQueueMutex_;
        std::condition_variable frameQueueCondition_;
        std::deque<CompletedFrame> completedFrames_;
        uint64_t completedQueueLastPushUs_ = 0;
        uint64_t completedQueueLastPopUs_ = 0;
        uint64_t completedQueuePushes_ = 0;
        uint64_t completedQueuePops_ = 0;
        uint64_t completedQueueEmptyPolls_ = 0;

        // Receiver側からPongなどを返すときのRNVP sequence
        std::atomic<uint32_t> rnvpSequence_{ 1 };
        uint32_t consecutiveIncompleteFrames_ = 0;
        uint64_t lastKeyFrameRequestUs_ = 0;
        uint64_t dynamicNackDeadlineUs_ = 25000;
        uint64_t lastDynamicNackUpdateUs_ = 0;
        uint64_t lastRetransmitUsefulChunks_ = 0;
        uint64_t lastRetransmitDuplicatePackets_ = 0;
        uint64_t lastRetransmitLateAfterCompletedPackets_ = 0;
        uint64_t lastRetransmitLateAfterExpiredPackets_ = 0;
        uint64_t lastRetransmitCompletedFrames_ = 0;
        uint64_t lastRetransmitExpiredFrames_ = 0;
        bool hasLastRnvpDataAddr_ = false;
        sockaddr_in lastRnvpDataAddr_{};

        struct PendingTransportFeedback {
            uint32_t sequence = 0;
            bool received = false;
            uint64_t receiveTimeUs = 0;
        };

        std::deque<PendingTransportFeedback> pendingTransportFeedback_;
        bool hasLastTransportFeedbackSequence_ = false;
        uint32_t lastTransportFeedbackSequence_ = 0;
        uint16_t transportFeedbackSequence_ = 1;
        uint64_t lastTransportFeedbackSendUs_ = 0;

        static constexpr int kReceiveBufferSize = 4096;
        static constexpr size_t kMaxQueuedFrames = 4;
        // Transport/reassembly safety valve only. Video freshness is owned by
        // NetworkVideoReceiver so low-latency policy is measured separately.
        static constexpr uint64_t kReceiverSafetyExpireUs = 1000000;
        static constexpr uint64_t kFrameNackDeadlineUs = 25000;
        static constexpr uint64_t kFrameNackIntervalUs = 20000;
        static constexpr uint64_t kFrameNackRecoveryExpireUs = 140000;
        static constexpr uint64_t kFrameNackMinRecoverySlackUs = 12000;
        static constexpr uint32_t kMaxDeadlineNacksPerFrame = 3;
        static constexpr uint64_t kKeyFrameRequestCooldownUs = 500000;
        static constexpr uint64_t kTransportFeedbackIntervalUs = 50000;
        static constexpr size_t kTransportFeedbackBatchSize = 32;
    };

} // namespace net
