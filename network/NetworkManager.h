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
#include <unordered_set>
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

    struct H264AckKeyFrameRequestTelemetry {
        uint64_t historyMissing = 0;
        uint64_t staleFrameLag = 0;
        uint64_t staleAge = 0;
        uint64_t retransmitBudgetExhausted = 0;
        uint64_t highMissingRate = 0;
        uint64_t cooldownSuppressed = 0;
        uint64_t alreadyPending = 0;
        uint64_t cooldownNoise = 0;
        uint64_t cooldownSyncRisk = 0;
        uint64_t staleAgeCooldownNoise = 0;
        uint64_t staleAgeCooldownSyncRisk = 0;
        std::string lastReason;
    };

    struct RnvpFrameProtectionOptions {
        uint16_t fecGroupChunkCountOverride = 0;
        bool forceFec = false;
        bool highPriorityData = false;
        bool highPriorityFec = false;
        uint64_t extraPacingDeadlineUs = 0;
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
        bool keyFrame = false,
        const RnvpFrameProtectionOptions& protection = {}
    );

    void SendRNVPFragmented(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType = net::CodecType::Raw,
        uint32_t streamId = 1,
        bool keyFrame = false,
        const RnvpFrameProtectionOptions& protection = {}
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
    H264AckKeyFrameRequestTelemetry GetH264AckKeyFrameRequestTelemetry() const;
    uint64_t GetRepairCanceledByCompleteAckPacketCount() const;
    uint64_t GetRepairSkippedByTtlPacketCount() const;
    uint64_t GetRepairQueuedButCanceledPacketCount() const;
    uint64_t GetRepairSentAfterCompleteAckPacketCount() const;
    uint64_t GetRepairSentAfterCompleteAckLargePacketCount() const;
    uint64_t GetRepairSuppressedByFecLikelyFrameCount() const;
    uint64_t GetRepairSuppressedByFecLikelyPacketCount() const;
    uint64_t GetRepairSuppressedByFecLikelyLargeFrameCount() const;
    uint64_t GetRepairSuppressedByFecLikelyLargePacketCount() const;
    uint64_t GetRepairBudgetSuppressedFrameCount() const;
    uint64_t GetRepairBudgetSuppressedPacketCount() const;
    uint64_t GetRepairBudgetSuppressedLargeFrameCount() const;
    uint64_t GetRepairBudgetSuppressedLargePacketCount() const;
    uint64_t GetRepairRaceGuardSuppressedFrameCount() const;
    uint64_t GetRepairRaceGuardSuppressedPacketCount() const;
    uint64_t GetRepairRaceGuardSuppressedLargePacketCount() const;
    std::string GetRepairBudgetProfile() const;
    uint64_t GetRepairBudgetProfileSwitchCount() const;
    double GetRepairBudgetSmoothedMissingRate() const;
    double GetRepairBudgetSmoothedPacingQueueDelayMs() const;
    double GetRepairBudgetSmoothedDeliveryMs() const;
    uint64_t GetRepairFecLikelySuppressedCompletedFrameCount() const;
    uint64_t GetRepairFecLikelySuppressedCompletedPacketCount() const;
    uint64_t GetRepairFecLikelySuppressedExpiredFrameCount() const;
    uint64_t GetRepairFecLikelySuppressedExpiredPacketCount() const;
    uint64_t GetRepairFecLikelySuppressedPendingFrameCount() const;
    uint64_t GetRepairFecLikelySuppressedPendingPacketCount() const;
    uint64_t GetRepairFecLikelySuppressionRescueFrameCount() const;
    uint64_t GetRepairFecLikelySuppressionRescuePacketCount() const;
    uint64_t GetH264KeyTinyMissingCriticalFrameCount() const;
    uint64_t GetH264KeyTinyMissingCriticalPacketCount() const;
    uint64_t GetH264KeyTinyMissingCriticalSentPacketCount() const;
    uint64_t GetH264KeyTinyMissingCriticalSkippedPacketCount() const;
    uint64_t GetH264KeyTinyMissingCriticalFeasibilitySuppressedFrameCount() const;
    uint64_t GetH264KeyTinyMissingCriticalFeasibilitySuppressedPacketCount() const;
    uint64_t GetH264KeyTinyMissingCriticalFeasibilityBypassedFrameCount() const;
    uint64_t GetH264KeyTinyMissingCriticalFeasibilityBypassedPacketCount() const;
    double GetH264KeyTinyMissingCriticalLastPredictedDeliveryMs() const;
    double GetH264KeyTinyMissingCriticalLastRemainingSlackMs() const;
    uint32_t GetH264KeyTinyMissingCriticalLastFrameId() const;
    uint32_t GetH264KeyTinyMissingCriticalLastAckMissingChunks() const;
    uint32_t GetH264KeyTinyMissingCriticalLastRequestedChunks() const;
    std::string GetH264KeyTinyMissingCriticalLastEvent() const;
    uint64_t GetH264KeySmallMissingAckFrameCount() const;
    uint64_t GetH264KeySmallMissingAckMissingChunkCount() const;
    uint64_t GetH264KeySmallMissingAckHistoryMissingFrameCount() const;
    uint64_t GetH264KeySmallMissingAckStaleFrameLagFrameCount() const;
    uint64_t GetH264KeySmallMissingAckStaleAgeFrameCount() const;
    uint64_t GetH264KeySmallMissingAckRetransmitBudgetExhaustedFrameCount() const;
    uint64_t GetH264KeySmallMissingAckDynamicBudgetSuppressedFrameCount() const;
    uint64_t GetH264KeySmallMissingAckDynamicBudgetSuppressedPacketCount() const;
    uint64_t GetH264KeySmallMissingAckSelectedRepairFrameCount() const;
    uint64_t GetH264KeySmallMissingAckSelectedRepairPacketCount() const;
    uint64_t GetH264KeySelectedRepair1To2FrameCount() const;
    uint64_t GetH264KeySelectedRepair1To2PacketCount() const;
    uint64_t GetH264KeySelectedRepair3To4FrameCount() const;
    uint64_t GetH264KeySelectedRepair3To4PacketCount() const;
    uint32_t GetH264KeySmallMissingAckLastFrameId() const;
    uint32_t GetH264KeySmallMissingAckLastMissingChunks() const;
    std::string GetH264KeySmallMissingAckLastGate() const;
    uint64_t GetLateRepairSavedPacketCount() const;
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
        uint64_t feedbackSequenceGapPackets = 0;
        uint64_t feedbackSequenceGapRecoveredPackets = 0;
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
        bool fecEnabled = false;
        uint16_t fecGroupChunkCount = 0;
        std::vector<uint8_t> payload;
    };

    struct SentPacketRecord {
        uint32_t sequence = 0;
        uint64_t sendTimeUs = 0;
        uint32_t packetBytes = 0;
    };

    struct CompletedFrameAckRecord {
        uint32_t frameId = 0;
        uint32_t streamId = 0;
        uint64_t ackTimeUs = 0;
    };

    struct RepairPacketPolicy {
        net::PacketPacingPriority priority = net::PacketPacingPriority::Normal;
        uint64_t ttlUs = 0;
        uint64_t deadlineUs = 0;
        const char* reason = "unknown";
    };

    enum class RepairBudgetProfile {
        Balanced,
        Clean,
        Loss,
        BurstLoss,
        Jitter,
        JitterAckLoss,
        Pacing
    };

    struct RepairBudgetControllerState {
        bool initialized = false;
        RepairBudgetProfile activeProfile = RepairBudgetProfile::Balanced;
        RepairBudgetProfile candidateProfile = RepairBudgetProfile::Balanced;
        uint32_t candidateSamples = 0;
        uint64_t holdUntilUs = 0;
        uint64_t switchCount = 0;
        double smoothedMissingRate = 0.0;
        double smoothedPacingQueueDelayUs = 0.0;
        double smoothedRepairDeliveryUs = 0.0;
        double smoothedRttMs = 0.0;
        double smoothedConfiguredLossRate = 0.0;
        double smoothedDelaySpreadMs = 0.0;
    };

    struct FecLikelySuppressionRecord {
        uint32_t frameId = 0;
        uint32_t streamId = 0;
        net::CodecType codecType = net::CodecType::Unknown;
        bool keyFrame = false;
        uint16_t chunkCount = 0;
        uint64_t frameSendTimeUs = 0;
        uint64_t firstSuppressionTimeUs = 0;
        uint64_t lastSuppressionTimeUs = 0;
        uint32_t suppressedPackets = 0;
        bool rescueAttempted = false;
        bool outcomeRecorded = false;
    };

    void SendRNVPFragmentedInternal(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType,
        uint32_t streamId,
        bool keyFrame,
        bool trackFrame,
        const char* context,
        const RnvpFrameProtectionOptions& protection
    );

    bool SendRNVPFramePackets(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType,
        uint32_t streamId,
        bool keyFrame,
        uint64_t sendTimeUs,
        const char* context,
        const RnvpFrameProtectionOptions& protection
    );

    bool SendRNVPFecParity(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType,
        uint32_t streamId,
        bool keyFrame,
        uint16_t chunkCount,
        uint64_t sendTimeUs,
        const char* context,
        const RnvpFrameProtectionOptions& protection
    );

    uint32_t SendRNVPSelectedChunks(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType,
        uint32_t streamId,
        bool keyFrame,
        uint64_t sendTimeUs,
        const std::vector<uint16_t>& chunkIndices,
        const char* context,
        uint32_t ackLatestSequence = 0,
        uint32_t retransmitAttempt = 0,
        uint32_t ackMissingChunks = 0,
        uint64_t originalFrameSendTimeUs = 0
    );

    void TrackSentFrame(
        const std::vector<uint8_t>& data,
        uint32_t frameId,
        net::CodecType codecType,
        uint32_t streamId,
        bool keyFrame,
        uint16_t chunkCount,
        uint64_t sendTimeUs,
        bool fecEnabled,
        uint16_t fecGroupChunkCount
    );
    bool IsFrameCompleteAckedLocked(uint32_t streamId, uint32_t frameId) const;
    void RememberFrameCompleteAckLocked(
        uint32_t streamId,
        uint32_t frameId,
        uint64_t ackTimeUs
    );
    bool ShouldSkipRepairForFrameLocked(
        uint32_t streamId,
        uint32_t frameId,
        uint64_t nowUs,
        bool& completedAck,
        bool& ttlExpired
    ) const;
    bool IsLargeRepairFrame(const SentFrameRecord& record) const;
    uint64_t CalculateRepairTtlUs(const SentFrameRecord& record) const;
    RepairPacketPolicy BuildRepairPacketPolicy(
        const SentFrameRecord& record,
        uint64_t nowUs,
        uint32_t ackMissingChunks,
        uint32_t requestedChunks
    ) const;
    std::vector<uint16_t> FilterRepairChunksForFecLikelyRecoveryLocked(
        const SentFrameRecord& record,
        const net::AckPayload& ack,
        uint32_t retransmitAttempt,
        uint64_t nowUs
    );
    std::vector<uint16_t> LimitRepairChunksByDynamicBudgetLocked(
        const SentFrameRecord& record,
        const net::AckPayload& ack,
        const std::vector<uint16_t>& chunkIndices,
        uint32_t retransmitAttempt,
        uint64_t nowUs,
        uint64_t estimatedRepairDeliveryUs,
        uint64_t pacingQueueDelayUs,
        double averageRttMs,
        const net::NetworkCondition& condition,
        double missingRate
    );
    RepairBudgetProfile UpdateRepairBudgetControllerLocked(
        uint64_t nowUs,
        uint64_t estimatedRepairDeliveryUs,
        uint64_t pacingQueueDelayUs,
        double averageRttMs,
        const net::NetworkCondition& condition,
        double missingRate
    );
    const char* ToRepairBudgetProfileString(
        RepairBudgetProfile profile
    ) const;
    void RememberFecLikelySuppressionLocked(
        const SentFrameRecord& record,
        uint32_t suppressedPackets,
        uint64_t nowUs
    );
    void MarkFecLikelySuppressionOutcomeLocked(
        uint32_t streamId,
        uint32_t frameId,
        const char* outcome,
        uint64_t nowUs,
        uint32_t ackLatestSequence,
        const char* reason
    );
    bool TryMarkFecLikelySuppressionRescueLocked(
        const SentFrameRecord& record,
        const net::AckPayload& ack,
        uint32_t retransmitAttempt,
        uint64_t nowUs,
        const char* reason
    );
    uint64_t GetPendingFecLikelySuppressionPacketCountLocked() const;
    const char* ToRepairPriorityString(
        net::PacketPacingPriority priority
    ) const;
    bool ShouldDropQueuedRepairPacket(
        const std::vector<uint8_t>& packet,
        const char* context
    );

    void HandleAckControl(
        uint32_t streamId,
        const net::AckPayload& ack,
        net::CodecType ackCodecType,
        bool ackKeyFrame,
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
    uint64_t repairCanceledByCompleteAckPackets_ = 0;
    uint64_t repairSkippedByTtlPackets_ = 0;
    uint64_t repairQueuedButCanceledPackets_ = 0;
    uint64_t repairSentAfterCompleteAckPackets_ = 0;
    uint64_t repairSentAfterCompleteAckLargePackets_ = 0;
    uint64_t repairSuppressedByFecLikelyFrames_ = 0;
    uint64_t repairSuppressedByFecLikelyPackets_ = 0;
    uint64_t repairSuppressedByFecLikelyLargeFrames_ = 0;
    uint64_t repairSuppressedByFecLikelyLargePackets_ = 0;
    uint64_t repairBudgetSuppressedFrames_ = 0;
    uint64_t repairBudgetSuppressedPackets_ = 0;
    uint64_t repairBudgetSuppressedLargeFrames_ = 0;
    uint64_t repairBudgetSuppressedLargePackets_ = 0;
    uint64_t repairRaceGuardSuppressedFrames_ = 0;
    uint64_t repairRaceGuardSuppressedPackets_ = 0;
    uint64_t repairRaceGuardSuppressedLargePackets_ = 0;
    RepairBudgetControllerState repairBudgetController_{};
    uint64_t repairFecLikelySuppressedCompletedFrames_ = 0;
    uint64_t repairFecLikelySuppressedCompletedPackets_ = 0;
    uint64_t repairFecLikelySuppressedExpiredFrames_ = 0;
    uint64_t repairFecLikelySuppressedExpiredPackets_ = 0;
    uint64_t repairFecLikelySuppressionRescueFrames_ = 0;
    uint64_t repairFecLikelySuppressionRescuePackets_ = 0;
    uint64_t h264KeyTinyMissingCriticalFrames_ = 0;
    uint64_t h264KeyTinyMissingCriticalPackets_ = 0;
    uint64_t h264KeyTinyMissingCriticalSentPackets_ = 0;
    uint64_t h264KeyTinyMissingCriticalSkippedPackets_ = 0;
    uint64_t h264KeyTinyMissingCriticalFeasibilitySuppressedFrames_ = 0;
    uint64_t h264KeyTinyMissingCriticalFeasibilitySuppressedPackets_ = 0;
    uint64_t h264KeyTinyMissingCriticalFeasibilityBypassedFrames_ = 0;
    uint64_t h264KeyTinyMissingCriticalFeasibilityBypassedPackets_ = 0;
    double h264KeyTinyMissingCriticalLastPredictedDeliveryMs_ = 0.0;
    double h264KeyTinyMissingCriticalLastRemainingSlackMs_ = 0.0;
    uint32_t h264KeyTinyMissingCriticalLastFrameId_ = 0;
    uint32_t h264KeyTinyMissingCriticalLastAckMissingChunks_ = 0;
    uint32_t h264KeyTinyMissingCriticalLastRequestedChunks_ = 0;
    std::string h264KeyTinyMissingCriticalLastEvent_;
    uint64_t h264KeySmallMissingAckFrames_ = 0;
    uint64_t h264KeySmallMissingAckMissingChunks_ = 0;
    uint64_t h264KeySmallMissingAckHistoryMissingFrames_ = 0;
    uint64_t h264KeySmallMissingAckStaleFrameLagFrames_ = 0;
    uint64_t h264KeySmallMissingAckStaleAgeFrames_ = 0;
    uint64_t h264KeySmallMissingAckRetransmitBudgetExhaustedFrames_ = 0;
    uint64_t h264KeySmallMissingAckDynamicBudgetSuppressedFrames_ = 0;
    uint64_t h264KeySmallMissingAckDynamicBudgetSuppressedPackets_ = 0;
    uint64_t h264KeySmallMissingAckSelectedRepairFrames_ = 0;
    uint64_t h264KeySmallMissingAckSelectedRepairPackets_ = 0;
    uint64_t h264KeySelectedRepair1To2Frames_ = 0;
    uint64_t h264KeySelectedRepair1To2Packets_ = 0;
    uint64_t h264KeySelectedRepair3To4Frames_ = 0;
    uint64_t h264KeySelectedRepair3To4Packets_ = 0;
    uint32_t h264KeySmallMissingAckLastFrameId_ = 0;
    uint32_t h264KeySmallMissingAckLastMissingChunks_ = 0;
    std::string h264KeySmallMissingAckLastGate_;
    uint64_t lateRepairSavedPackets_ = 0;
    uint64_t ackStaleDroppedFrameCount_ = 0;
    uint64_t ackKeyFrameRequestCount_ = 0;
    uint64_t lastAckKeyFrameRequestUs_ = 0;
    uint64_t h264AckKeyFrameRequestHistoryMissing_ = 0;
    uint64_t h264AckKeyFrameRequestStaleFrameLag_ = 0;
    uint64_t h264AckKeyFrameRequestStaleAge_ = 0;
    uint64_t h264AckKeyFrameRequestRetransmitBudgetExhausted_ = 0;
    uint64_t h264AckKeyFrameRequestHighMissingRate_ = 0;
    uint64_t h264AckKeyFrameRequestCooldownSuppressed_ = 0;
    uint64_t h264AckKeyFrameRequestAlreadyPending_ = 0;
    uint64_t h264AckKeyFrameRequestCooldownNoise_ = 0;
    uint64_t h264AckKeyFrameRequestCooldownSyncRisk_ = 0;
    uint64_t h264AckKeyFrameRequestStaleAgeCooldownNoise_ = 0;
    uint64_t h264AckKeyFrameRequestStaleAgeCooldownSyncRisk_ = 0;
    std::string h264AckKeyFrameRequestLastReason_;
    std::deque<CompletedFrameAckRecord> completedFrameAcks_;
    std::deque<FecLikelySuppressionRecord> fecLikelySuppressionRecords_;
    std::atomic<bool> forceNextKeyFrame_{ false };

    mutable std::mutex sentPacketsMutex_;
    std::deque<SentPacketRecord> sentPackets_;

    mutable std::mutex transportFeedbackMutex_;
    TransportFeedbackStats transportFeedbackStats_{};
    std::unordered_set<uint32_t> pendingMissingFeedbackSequences_;

    static constexpr size_t kSentFrameHistoryLimit = 24;
    static constexpr size_t kSentPacketHistoryLimit = 2048;
    static constexpr uint32_t kMaxRetransmitsPerFrame = 1;
    static constexpr uint32_t kMaxRetransmitFrameLag = 2;
    static constexpr uint64_t kMaxRetransmitAgeUs = 180000;
    static constexpr uint64_t kAckKeyFrameRequestCooldownUs = 500000;
    static constexpr size_t kCompletedFrameAckHistoryLimit = 64;
    static constexpr size_t kFecLikelySuppressionHistoryLimit = 128;

    static constexpr int kControlReceiveBufferSize = 2048;
};
