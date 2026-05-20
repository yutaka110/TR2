#pragma once

#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "FrameReassembler.h"
#include "JitterBuffer.h"
#include "NetworkStats.h"

#include <atomic>
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

        NetworkStatsSnapshot GetStats() const;
        void ResetStats();

        void SetJitterBufferTargetDelayMs(uint32_t delayMs);
        uint32_t GetJitterBufferTargetDelayMs() const;

        void SetJitterBufferAutoModeEnabled(bool enabled);
        bool IsJitterBufferAutoModeEnabled() const;

        void NotifyDecodeFrame();
        void NotifyDisplayFrame();
    private:
        void ReceiveLoop();

        void PushCompletedFrameToJitterBuffer(
            CompletedFrame&& frame,
            uint64_t nowUs
        );

        void DrainReadyJitterBuffer(uint64_t nowUs);

        void UpdateJitterBufferAutoMode(uint64_t nowUs);
        bool IsFramePastDisplayDeadline(
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
        std::deque<CompletedFrame> completedFrames_;

        // Receiver側からPongなどを返すときのRNVP sequence
        std::atomic<uint32_t> rnvpSequence_{ 1 };
        uint32_t consecutiveIncompleteFrames_ = 0;
        uint64_t lastKeyFrameRequestUs_ = 0;

        static constexpr int kReceiveBufferSize = 4096;
        static constexpr size_t kMaxQueuedFrames = 4;
        static constexpr uint64_t kMaxDisplayLatencyUs = 150000;
        static constexpr uint64_t kKeyFrameRequestCooldownUs = 500000;
    };

} // namespace net
