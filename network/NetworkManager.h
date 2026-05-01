#pragma once

#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "PacketProtocol.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class NetworkManager {
public:
    NetworkManager(const std::string& ip, uint16_t port);
    ~NetworkManager();

    // ============================================================
    // Legacy / Current Sender
    // ============================================================
    void SendUDPFragmented(const std::string& data, uint32_t frameId);
    void SendUDPFragmented(const std::vector<uint8_t>& data, uint32_t frameId);
    void SendUDPFragmentedParallel(const std::string& data, uint32_t frameId, int threadCount = 16);

    // ============================================================
    // RNVP v1 Data Sender
    // ============================================================
    void SendRNVPFragmented(
        const std::string& data,
        uint32_t frameId,
        net::CodecType codecType = net::CodecType::Raw,
        uint32_t streamId = 1
    );

    void SendRNVPFragmented(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType = net::CodecType::Raw,
        uint32_t streamId = 1
    );

    // ============================================================
    // RNVP v1 Control
    // ============================================================
    void SendRNVPPing(uint32_t streamId = 1);

    // Pong / Ack / Control を受け取る軽量受信ループ
    bool StartRNVPControlReceiver();
    void StopRNVPControlReceiver();

    double GetLastRttMs() const;
    double GetAverageRttMs() const;
    uint64_t GetRttSampleCount() const;

    uint32_t GetLastAckFrameId() const;
    uint32_t GetLastAckReceivedChunks() const;
    uint32_t GetLastAckMissingChunks() const;
    double GetLastAckMissingRate() const;
    uint64_t GetAckCount() const;

private:
    void RNVPControlReceiveLoop();

    void HandleRnvpControlPacket(
        const uint8_t* packetData,
        size_t packetSize
    );

    void HandleRnvpPong(
        const net::RnvpHeaderV1& header,
        const uint8_t* payload,
        size_t payloadSize
    );

    void HandleRnvpAck(
        const net::RnvpHeaderV1& header,
        const uint8_t* payload,
        size_t payloadSize
    );

    void HandleRnvpControl(
        const net::RnvpHeaderV1& header,
        const uint8_t* payload,
        size_t payloadSize
    );

private:
    uint64_t NowMicroseconds() const;
    uint32_t NextRNVPSequence();

private:
    SOCKET udpSocket_ = INVALID_SOCKET;
    sockaddr_in udpAddr_{};

    std::atomic<uint32_t> rnvpSequence_{ 1 };

    // RNVP制御受信用
    std::atomic<bool> controlReceiverRunning_{ false };
    std::thread controlReceiveThread_;

    // RTT統計
    mutable std::mutex rttMutex_;
    double lastRttMs_ = 0.0;
    double averageRttMs_ = 0.0;
    uint64_t rttSampleCount_ = 0;

    // ACK統計
    mutable std::mutex ackMutex_;
    uint32_t lastAckFrameId_ = 0;
    uint32_t lastAckReceivedChunks_ = 0;
    uint32_t lastAckMissingChunks_ = 0;
    double lastAckMissingRate_ = 0.0;
    uint64_t ackCount_ = 0;

    static constexpr int kControlReceiveBufferSize = 2048;
};