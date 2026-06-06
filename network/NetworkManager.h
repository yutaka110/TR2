#pragma once

#define _WINSOCKAPI_
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "PacketProtocol.h"
#include "PacketPacer.h"
#include "BandwidthEstimator.h"
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
    struct AdaptiveFecDecisionTelemetry {
        std::string decisionReason = "disabled";
        std::string holdReason;
        bool g8ToG4Recovery = false;
        bool emergencyG2Active = false;
        std::string earlyOffReason;
    };

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
    double GetMaxRttMs() const;
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

    void SetPacingEnabled(bool enabled);
    bool IsPacingEnabled() const;
    void SetPacingTargetBitrateKbps(uint32_t bitrateKbps);
    net::PacketPacerStats GetPacingStats() const;
    void SetFecEnabled(bool enabled);
    bool IsFecEnabled() const;
    void SetAdaptiveFecEnabled(bool enabled);
    bool IsAdaptiveFecEnabled() const;
    void SetFecGroupChunkCount(uint16_t groupChunkCount);
    uint16_t GetFecGroupChunkCount() const;
    AdaptiveFecDecisionTelemetry GetAdaptiveFecDecisionTelemetry() const;
    void UpdateAdaptiveFec(
        double packetLossRate,
        double ackMissingRate,
        uint64_t deadlineNackSentFrames,
        uint64_t deadlineNackExpiredDroppedFrames,
        uint64_t fecParityPackets,
        uint64_t fecRecoveredFrames,
        uint32_t estimatedBandwidthBps,
        uint32_t targetBitrateKbps,
        double queueDelayMs
    );

    struct TransportFeedbackStats {
        uint64_t feedbackPackets = 0;
        uint64_t feedbackPacketStatuses = 0;
        uint64_t feedbackReceivedPackets = 0;
        uint64_t feedbackMissingPackets = 0;
        double feedbackLossRate = 0.0;
        double feedbackArrivalJitterMs = 0.0;
        double feedbackQueueDelayTrendMs = 0.0;
        uint16_t lastFeedbackSequence = 0;
    };

    TransportFeedbackStats GetTransportFeedbackStats() const;
    net::BandwidthEstimatorStats GetBandwidthEstimatorStats() const;

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

    struct SentPacketRecord {
        uint32_t sequence = 0;
        uint64_t sendTimeUs = 0;
        uint32_t packetBytes = 0;
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

    bool SendRNVPFecParity(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType,
        uint32_t streamId,
        bool keyFrame,
        uint16_t chunkCount,
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

    void HandleRnvpTransportFeedback(
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

    void SendPacedPacketWithSimulation(
        std::vector<uint8_t>&& packet,
        const char* context,
        net::PacketPacingPriority priority,
        uint64_t deadlineUs
    );

    void SendPacketWithSimulation(
        std::vector<uint8_t>&& packet,
        const char* context
    );

    void TrackSentRnvpDataPacket(
        const uint8_t* packetData,
        size_t packetSize,
        uint64_t sendTimeUs
    );

private:
    SOCKET udpSocket_ = INVALID_SOCKET;
    sockaddr_in udpAddr_{};
    mutable std::mutex udpSendMutex_;

    net::NetworkConditionSimulator networkSimulator_;
    net::PacketPacer packetPacer_;
    net::BandwidthEstimator bandwidthEstimator_;
    std::atomic<bool> fecEnabled_{ true };
    std::atomic<bool> adaptiveFecEnabled_{ false };
    std::atomic<uint16_t> fecGroupChunkCount_{ 4 };
    mutable std::mutex adaptiveFecMutex_;
    uint64_t adaptiveFecLastDeadlineNackSentFrames_ = 0;
    uint64_t adaptiveFecLastDeadlineNackExpiredDroppedFrames_ = 0;
    uint64_t adaptiveFecLastParityPackets_ = 0;
    uint64_t adaptiveFecLastRecoveredFrames_ = 0;
    double adaptiveFecLossPressureEma_ = 0.0;
    uint32_t adaptiveFecStableSamples_ = 0;
    uint32_t adaptiveFecHoldSamples_ = 0;
    uint16_t adaptiveFecHoldGroupChunkCount_ = 8;
    uint64_t adaptiveFecHoldUntilUs_ = 0;
    uint64_t adaptiveFecG2UntilUs_ = 0;
    uint64_t adaptiveFecG2CooldownUntilUs_ = 0;
    uint64_t adaptiveFecIneffectiveOffUntilUs_ = 0;
    uint64_t adaptiveFecIneffectiveOffStartExpiredFrames_ = 0;
    uint16_t adaptiveFecIneffectiveOffRearmGroupChunkCount_ = 8;
    uint32_t adaptiveFecPostOffRearmSamples_ = 0;
    uint16_t adaptiveFecPostOffRearmGroupChunkCount_ = 8;
    uint32_t adaptiveFecWasteSamples_ = 0;
    uint32_t adaptiveFecIneffectiveSamples_ = 0;
    uint32_t adaptiveFecG8NackExpiredSamples_ = 0;
    uint32_t adaptiveFecG4DefenseExpiredSamples_ = 0;
    uint32_t adaptiveFecUncoveredDeadlineSamples_ = 0;
    uint32_t adaptiveFecCoveredRecoverySamples_ = 0;
    AdaptiveFecDecisionTelemetry adaptiveFecDecisionTelemetry_{};

    std::atomic<uint32_t> rnvpSequence_{ 1 };

    // RNVP制御受信用
    std::atomic<bool> controlReceiverRunning_{ false };
    std::thread controlReceiveThread_;

    // RTT統計
    mutable std::mutex rttMutex_;
    double lastRttMs_ = 0.0;
    double averageRttMs_ = 0.0;
    double maxRttMs_ = 0.0;
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

    mutable std::mutex sentPacketsMutex_;
    std::deque<SentPacketRecord> sentPackets_;

    mutable std::mutex transportFeedbackMutex_;
    TransportFeedbackStats transportFeedbackStats_{};

    static constexpr size_t kSentFrameHistoryLimit = 24;
    static constexpr size_t kSentPacketHistoryLimit = 2048;
    static constexpr uint32_t kMaxRetransmitsPerFrame = 1;
    static constexpr uint32_t kMaxRetransmitFrameLag = 2;
    static constexpr uint64_t kMaxRetransmitAgeUs = 180000;

    static constexpr int kControlReceiveBufferSize = 2048;
};
