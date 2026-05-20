#pragma once

#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "PacketProtocol.h"
#include "NetworkConditionSimulator.h"

#include <atomic>
#include <cstdint>
#include <deque>
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
        uint32_t streamId = 1,
        bool keyFrame = false
    );

    void SendRNVPFragmented(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType = net::CodecType::Raw,
        uint32_t streamId = 1,
        bool keyFrame = false
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
    uint64_t GetAckRetransmittedFrameCount() const;
    uint64_t GetAckRetransmittedChunkCount() const;
    uint64_t GetAckStaleDroppedFrameCount() const;
    uint64_t GetAckKeyFrameRequestCount() const;
    bool IsKeyFrameRequestPending() const;
    bool ConsumeKeyFrameRequest();

    void SetNetworkCondition(const net::NetworkCondition& condition);
    net::NetworkCondition GetNetworkCondition() const;
    net::NetworkSimulationStats GetNetworkSimulationStats() const;
    void ResetStats();
    void ResetNetworkSimulationStats();
    void FlushNetworkSimulator();

private:
    struct SentFrameRecord {
        uint32_t frameId = 0;
        uint32_t streamId = 0;
        net::CodecType codecType = net::CodecType::Unknown;
        uint16_t chunkCount = 0;
        uint64_t sendTimeUs = 0;
        uint32_t retransmitCount = 0;
        bool acked = false;
        bool keyFrame = false;
        std::vector<uint8_t> payload;
    };

    void SendRNVPFragmentedInternal(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType,
        uint32_t streamId,
        bool keyFrame,
        bool trackFrame,
        const char* context
    );

    bool SendRNVPFramePackets(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType,
        uint32_t streamId,
        bool keyFrame,
        uint64_t sendTimeUs,
        const char* context
    );

    uint32_t SendRNVPSelectedChunks(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType,
        uint32_t streamId,
        bool keyFrame,
        uint64_t sendTimeUs,
        const std::vector<uint16_t>& chunkIndices,
        const char* context
    );

    void TrackSentFrame(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType,
        uint32_t streamId,
        bool keyFrame,
        uint16_t chunkCount,
        uint64_t sendTimeUs
    );

    void HandleAckControl(
        uint32_t streamId,
        const net::AckPayload& ack,
        double missingRate
    );

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

    bool SendPacketRaw(
        const uint8_t* packetData,
        size_t packetSize,
        const char* context
    );

    void SendPacketWithSimulation(
        std::vector<uint8_t>&& packet,
        const char* context
    );

private:
    SOCKET udpSocket_ = INVALID_SOCKET;
    sockaddr_in udpAddr_{};
    mutable std::mutex udpSendMutex_;

    net::NetworkConditionSimulator networkSimulator_;

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

    mutable std::mutex sentFramesMutex_;
    std::deque<SentFrameRecord> sentFrames_;
    uint32_t latestSentFrameId_ = 0;
    uint64_t ackRetransmittedFrameCount_ = 0;
    uint64_t ackRetransmittedChunkCount_ = 0;
    uint64_t ackStaleDroppedFrameCount_ = 0;
    uint64_t ackKeyFrameRequestCount_ = 0;
    std::atomic<bool> forceNextKeyFrame_{ false };

    static constexpr size_t kSentFrameHistoryLimit = 24;
    static constexpr uint32_t kMaxRetransmitsPerFrame = 1;
    static constexpr uint32_t kMaxRetransmitFrameLag = 2;
    static constexpr uint64_t kMaxRetransmitAgeUs = 500000;

    static constexpr int kControlReceiveBufferSize = 2048;
};
