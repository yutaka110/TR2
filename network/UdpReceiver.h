#pragma once

#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "FrameReassembler.h"
#include "NetworkStats.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <queue>
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

    private:
        void ReceiveLoop();

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

        uint64_t NowMicroseconds() const;
        uint32_t NextRNVPSequence();

    private:
        SOCKET socket_ = INVALID_SOCKET;
        std::atomic<bool> running_ = false;
        std::thread receiveThread_;

        NetworkStats stats_;
        FrameReassembler reassembler_;

        std::mutex frameQueueMutex_;
        std::queue<CompletedFrame> completedFrames_;

        // Receiver側からPongなどを返すときのRNVP sequence
        std::atomic<uint32_t> rnvpSequence_{ 1 };

        static constexpr int kReceiveBufferSize = 4096;
        static constexpr size_t kMaxQueuedFrames = 4;
    };

} // namespace net