#pragma once

#include "NetworkConditionSimulator.h"
#include "NetworkRuntimeMode.h"
#include "PacketProtocol.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace net {

	enum class DuplicatePacketKind {
		Original,
		Retransmit,
		LateOriginalAfterCompleted,
		LateRetransmitAfterCompleted,
		LateAfterExpired,
		LateAfterRejected
	};

	struct NetworkStatsSnapshot {
		// ============================================================
		// Packet / Byte
		// ============================================================
		uint64_t receivedPackets = 0;
		uint64_t receivedBytes = 0;

		// RNVP sequence欠番から推定した欠損パケット数
		uint64_t missingPackets = 0;
		uint64_t sequenceGapPackets = 0;
		uint64_t sequenceGapRecoveredPackets = 0;

		uint64_t duplicatePackets = 0;
		uint64_t duplicateOriginalPackets = 0;
		uint64_t duplicateRetransmitPackets = 0;
		uint64_t duplicateLateAfterCompletedPackets = 0;
		uint64_t duplicateLateAfterCompletedOriginalPackets = 0;
		uint64_t duplicateLateAfterCompletedRetransmitPackets = 0;
		uint64_t duplicateLateAfterExpiredPackets = 0;
		uint64_t duplicateLateAfterRejectedPackets = 0;
		uint64_t reorderedPackets = 0;

		// ============================================================
		// Frame
		// ============================================================
		uint64_t completedFrames = 0;
		uint64_t droppedFrames = 0;
		uint64_t deadlineDroppedFrames = 0;
		uint64_t outputQueueDroppedFrames = 0;
		uint64_t outputQueueDropEvents = 0;
		uint64_t outputQueueDropBurstEvents = 0;
		uint32_t lastOutputQueueDropFrameCount = 0;
		uint32_t lastOutputQueueDropQueueSize = 0;
		double lastOutputQueueDropOldestAgeMs = 0.0;
		double lastOutputQueueDropNewestAgeMs = 0.0;
		double maxOutputQueueDropOldestAgeMs = 0.0;
		std::string lastOutputQueueDropReason;
		uint64_t completedQueuePushes = 0;
		uint64_t completedQueuePops = 0;
		uint64_t completedQueueEmptyPolls = 0;
		uint32_t completedQueueSize = 0;
		uint32_t maxCompletedQueueSize = 0;
		double completedQueueLastPopAgeMs = 0.0;
		double completedQueueMaxPopAgeMs = 0.0;
		double completedQueueLastPushIntervalMs = 0.0;
		double completedQueueMaxPushIntervalMs = 0.0;
		double lastOutputQueueDropPopAgeMs = 0.0;
		double lastOutputQueueDropPushIntervalMs = 0.0;
		uint64_t decodedFrames = 0;
		uint64_t displayedFrames = 0;
		uint64_t frameRecoveryOutcomeEvents = 0;
		uint64_t frameRecoveryCompletedFrames = 0;
		uint64_t frameRecoveryExpiredFrames = 0;
		uint64_t frameRecoveryRejectedFrames = 0;
		uint64_t frameRecoveryNackSentFrames = 0;
		uint64_t frameRecoveryFecRecoveredFrames = 0;
		uint32_t frameRecoveryLastFrameId = 0;
		uint32_t frameRecoveryLastStreamId = 0;
		std::string frameRecoveryLastEvent;
		std::string frameRecoveryLastOutcome;
		std::string frameRecoveryLastCodec;
		bool frameRecoveryLastKeyFrame = false;
		bool frameRecoveryLastLargeFrame = false;
		uint32_t frameRecoveryLastChunkCount = 0;
		uint32_t frameRecoveryLastReceivedChunks = 0;
		uint32_t frameRecoveryLastMissingChunks = 0;
		uint32_t frameRecoveryLastFecParityPackets = 0;
		uint32_t frameRecoveryLastFecRecoveredChunks = 0;
		uint32_t frameRecoveryLastNackCount = 0;
		uint32_t frameRecoveryLastPostNackReceivedChunks = 0;
		uint32_t frameRecoveryLastNackRequestedChunks = 0;
		uint32_t frameRecoveryLastRetransmitReceivedChunks = 0;
		uint32_t frameRecoveryLastRetransmitDuplicatePackets = 0;
		uint32_t frameRecoveryLastPacketSequence = 0;
		uint32_t frameRecoveryLastPacketChunkIndex = 0;
		uint32_t frameRecoveryLastRetransmitSequence = 0;
		uint32_t frameRecoveryLastRetransmitChunkIndex = 0;
		uint32_t frameRecoveryLastEventPacketSequence = 0;
		uint32_t frameRecoveryLastEventPacketChunkIndex = 0;
		bool frameRecoveryLastEventWasRetransmit = false;
		double frameRecoveryLastAgeMs = 0.0;
		uint64_t retransmitUsefulChunks = 0;
		uint64_t retransmitDuplicatePackets = 0;
		uint64_t retransmitLateAfterCompletedPackets = 0;
		uint64_t retransmitLateAfterCompletedLargePackets = 0;
		uint64_t retransmitLateAfterCompletedSentBeforeCompletePackets = 0;
		uint64_t retransmitLateAfterCompletedSentAfterCompletePackets = 0;
		double retransmitLateAfterCompletedAvgSendToCompleteMs = 0.0;
		double retransmitLateAfterCompletedAvgDelayMs = 0.0;
		double retransmitLateAfterCompletedMaxDelayMs = 0.0;
		uint64_t retransmitLateAfterExpiredPackets = 0;
		uint64_t retransmitLateAfterRejectedPackets = 0;
		uint64_t retransmitClassifiedPackets = 0;
		uint64_t retransmitNotArrivedPackets = 0;
		uint64_t retransmitAccountedPackets = 0;
		uint64_t retransmitUnclassifiedPackets = 0;
		uint64_t retransmitCompletedFrames = 0;
		uint64_t retransmitExpiredFrames = 0;
		double retransmitUsefulnessRatio = 0.0;
		double retransmitDuplicateRatio = 0.0;
		double retransmitFinalClassificationRatio = 0.0;
		double retransmitFinalAccountingRatio = 0.0;
		double retransmitExpiredAfterUsefulRatio = 0.0;
		double dynamicNackDeadlineMs = 0.0;
		double dynamicNackUsefulnessRatio = 0.0;
		double dynamicNackDuplicateRatio = 0.0;
		double dynamicNackExpiredAfterRetransmitRatio = 0.0;
		std::string dynamicNackDecisionReason;
		uint64_t nackSuppressedFrames = 0;
		uint64_t nackSuppressedMissingChunks = 0;
		uint64_t nackPreflightSuppressedFrames = 0;
		uint64_t nackPreflightSuppressedChunks = 0;
		uint64_t nackFecGraceSuppressedFrames = 0;
		uint64_t nackFecGraceSuppressedChunks = 0;
		uint64_t nackPredictedUsefulFrames = 0;
		uint64_t nackPredictedUsefulChunks = 0;
		uint64_t nackDeferredForLikelyArrivalFrames = 0;
		uint64_t nackDeferredForLikelyArrivalChunks = 0;
		uint64_t nackSkippedTooLateFrames = 0;
		uint64_t nackSkippedTooLateChunks = 0;
		uint64_t nackRequestedChunkBudget = 0;
		std::string nackLastShapingReason;
		std::string nackLastSuppressionReason;

		double receiveFps = 0.0;
		double decodeFps = 0.0;
		double displayFps = 0.0;

		// ============================================================
		// Latency
		// ============================================================
		double currentLatencyMs = 0.0;
		double averageLatencyMs = 0.0;
		double maxLatencyMs = 0.0;

		// ============================================================
		// RTT
		// ============================================================
		double currentRttMs = 0.0;
		double averageRttMs = 0.0;
		double maxRttMs = 0.0;
		uint64_t rttSamples = 0;

		// ============================================================
        // Sender ACK
        // ------------------------------------------------------------
        // 送信側が受け取ったACK情報。
        // NetworkManager側のACK受信結果をUIに渡すために使う。
        // ============================================================
		uint64_t ackCount = 0;
		uint32_t lastAckFrameId = 0;
		uint32_t lastAckReceivedChunks = 0;
		uint32_t lastAckMissingChunks = 0;
		double lastAckMissingRate = 0.0;
		uint64_t ackRetransmittedFrames = 0;
		uint64_t ackRetransmittedChunks = 0;
		uint64_t repairCanceledByCompleteAckPackets = 0;
		uint64_t repairSkippedByTtlPackets = 0;
		uint64_t repairQueuedButCanceledPackets = 0;
		uint64_t repairSentAfterCompleteAckPackets = 0;
		uint64_t repairSentAfterCompleteAckLargePackets = 0;
		uint64_t repairSuppressedByFecLikelyFrames = 0;
		uint64_t repairSuppressedByFecLikelyPackets = 0;
		uint64_t repairSuppressedByFecLikelyLargeFrames = 0;
		uint64_t repairSuppressedByFecLikelyLargePackets = 0;
		uint64_t repairBudgetSuppressedFrames = 0;
		uint64_t repairBudgetSuppressedPackets = 0;
		uint64_t repairBudgetSuppressedLargeFrames = 0;
		uint64_t repairBudgetSuppressedLargePackets = 0;
		uint64_t repairRaceGuardSuppressedFrames = 0;
		uint64_t repairRaceGuardSuppressedPackets = 0;
		uint64_t repairRaceGuardSuppressedLargePackets = 0;
		std::string repairBudgetProfile;
		uint64_t repairBudgetProfileSwitches = 0;
		double repairBudgetSmoothedMissingRate = 0.0;
		double repairBudgetSmoothedPacingQueueDelayMs = 0.0;
		double repairBudgetSmoothedDeliveryMs = 0.0;
		uint64_t repairFecLikelySuppressedCompletedFrames = 0;
		uint64_t repairFecLikelySuppressedCompletedPackets = 0;
		uint64_t repairFecLikelySuppressedExpiredFrames = 0;
		uint64_t repairFecLikelySuppressedExpiredPackets = 0;
		uint64_t repairFecLikelySuppressedPendingFrames = 0;
		uint64_t repairFecLikelySuppressedPendingPackets = 0;
		uint64_t repairFecLikelySuppressionRescueFrames = 0;
		uint64_t repairFecLikelySuppressionRescuePackets = 0;
		uint64_t lateRepairSavedPackets = 0;
		uint64_t ackStaleDroppedFrames = 0;
		uint64_t ackKeyFrameRequests = 0;
		bool ackKeyFramePending = false;
		bool pacingEnabled = false;
		uint32_t pacingTargetBitrateBps = 0;
		uint32_t pacingRepairTargetBitrateBps = 0;
		uint32_t pacingQueuedPackets = 0;
		uint32_t pacingHighPriorityQueuedPackets = 0;
		uint32_t pacingNormalQueuedPackets = 0;
		uint64_t pacingEnqueuedPackets = 0;
		uint64_t pacingSentPackets = 0;
		uint64_t pacingSentBytes = 0;
		uint64_t pacingRepairSentPackets = 0;
		uint64_t pacingRepairSentBytes = 0;
		uint64_t pacingRepairBorrowedPackets = 0;
		uint64_t pacingRepairBorrowedBytes = 0;
		uint64_t pacingDroppedPackets = 0;
		uint64_t pacingDeadlineDroppedPackets = 0;
		uint64_t pacingHighPriorityDeadlineDroppedPackets = 0;
		uint64_t pacingNormalDeadlineDroppedPackets = 0;
		uint64_t pacingOverflowDroppedPackets = 0;
		double pacingCurrentQueueDelayMs = 0.0;
		double pacingMaxQueueDelayMs = 0.0;
		double pacingVideoCreditBytes = 0.0;
		double pacingRepairCreditBytes = 0.0;
		uint64_t transportFeedbackPackets = 0;
		uint64_t transportFeedbackPacketStatuses = 0;
		uint64_t transportFeedbackReceivedPackets = 0;
		uint64_t transportFeedbackMissingPackets = 0;
		uint64_t transportFeedbackSequenceGapPackets = 0;
		uint64_t transportFeedbackSequenceGapRecoveredPackets = 0;
		double transportFeedbackLossRate = 0.0;
		double transportFeedbackArrivalJitterMs = 0.0;
		double transportFeedbackQueueDelayTrendMs = 0.0;
		uint16_t transportFeedbackLastSequence = 0;
		uint32_t estimatedBandwidthBps = 0;
		uint32_t deliveryRateBps = 0;
		double bandwidthQueueDelayMs = 0.0;
		double bandwidthRttTrendMs = 0.0;
		double bandwidthLossTrend = 0.0;
		double bandwidthJitterTrendMs = 0.0;
		uint64_t bandwidthFeedbackSamples = 0;
		uint64_t deadlineNackSentFrames = 0;
		uint64_t deadlineNackRecoveredFrames = 0;
		uint64_t deadlineNackMissingChunks = 0;
		uint64_t deadlineNackSentH264KeyFrames = 0;
		uint64_t deadlineNackSentH264LargeFrames = 0;
		uint64_t deadlineNackSentH264DeltaFrames = 0;
		uint64_t deadlineNackExpiredDroppedFrames = 0;
		uint64_t deadlineNackExpiredAfterNackFrames = 0;
		uint64_t deadlineNackExpiredMissingChunks = 0;
		uint64_t deadlineNackExpiredH264KeyFrames = 0;
		uint64_t deadlineNackExpiredH264LargeFrames = 0;
		uint64_t deadlineNackExpiredH264DeltaFrames = 0;
		bool fecEnabled = false;
		bool adaptiveFecEnabled = false;
		uint32_t fecGroupChunkCount = 0;
		uint64_t fecParityPackets = 0;
		uint64_t fecRecoveredFrames = 0;
		uint64_t fecRecoveredChunks = 0;
		uint64_t h264ReassemblerAuRejectedFrames = 0;
		uint64_t h264ReassemblerHeaderFailures = 0;
		uint64_t h264ReassemblerPayloadSizeMismatches = 0;
		uint64_t h264ReassemblerFrameIdMismatches = 0;
		uint64_t h264ReassemblerCrcMismatches = 0;
		std::string h264ReassemblerLastRejectReason;
		std::string adaptiveFecDecisionReason;
		std::string adaptiveFecHoldReason;
		bool adaptiveFecG8ToG4Recovery = false;
		bool adaptiveFecQualityHoldActive = false;
		bool adaptiveFecQualityHoldCanceled = false;
		bool adaptiveFecEmergencyG2Active = false;
		std::string adaptiveFecEarlyOffReason;

		// ============================================================
        // Adaptive Streaming
        // ------------------------------------------------------------
        // AdaptiveStreamingController の現在の制御目標。
        // UI表示用。実際のJPEG品質/FPS反映は次Stepで行う。
        // ============================================================
		bool adaptiveEnabled = false;
		std::string adaptiveControlMode;
		std::string adaptiveCongestionControlMode;

		int adaptiveTargetJpegQuality = 0;
		int adaptiveTargetFps = 0;
		int adaptiveTargetBitrateKbps = 0;
		int adaptiveBandwidthCeilingKbps = 0;
		int adaptiveTargetWidth = 0;
		int adaptiveTargetHeight = 0;
		uint64_t adaptiveRawFrameBytes = 0;
		uint64_t adaptiveEncodedFrameBytes = 0;
		double adaptiveCompressionRatio = 0.0;
		std::string sendActualCodec;
		uint32_t sendActualEncodeWidth = 0;
		uint32_t sendActualEncodeHeight = 0;
		uint64_t sendActualRawFrameBytes = 0;
		uint64_t sendActualEncodedFrameBytes = 0;
		double captureFps = 0.0;
		std::string cameraCaptureSubtype;
		uint32_t cameraCaptureWidth = 0;
		uint32_t cameraCaptureHeight = 0;
		double cameraCaptureFormatFps = 0.0;
		double encodeMs = 0.0;
		double sendResizeMs = 0.0;
		double sendNv12PrepareMs = 0.0;
		double sendH264EncodeMs = 0.0;
		uint32_t sendH264EncoderRequestedBitrateKbps = 0;
		uint32_t sendH264EncoderTargetBitrateKbps = 0;
		uint32_t sendH264EncoderAppliedBitrateKbps = 0;
		uint64_t h264DynamicBitrateUpdateRequests = 0;
		uint64_t h264DynamicBitrateUpdateSuccesses = 0;
		uint64_t h264DynamicBitrateUpdateFailures = 0;
		uint64_t h264EncoderReinitializations = 0;
		uint32_t sendPacingTargetBitrateKbps = 0;
		double sendH264VideoBudgetScale = 1.0;
		bool pacingBurstGuardActive = false;
		double adaptiveRepairBudgetUtilization = 0.0;
		double adaptiveRepairBorrowedRatio = 0.0;
		uint64_t adaptiveRepairSentBytesDelta = 0;
		uint64_t adaptiveRepairBorrowedBytesDelta = 0;
		bool adaptiveRepairBudgetGuardActive = false;
		bool adaptiveRepairVideoBudgetPressure = false;
		double adaptiveRetransmitUsefulRatio = 0.0;
		double adaptiveLateRepairWasteRatio = 0.0;
		double adaptiveRetransmitNotArrivedRatio = 0.0;
		bool adaptiveRetransmitAccountingComplete = false;
		bool adaptiveLateRepairWastePressure = false;
		bool adaptiveRetransmitNotArrivedPressure = false;
		std::string adaptiveRepairDecisionReason;
		uint32_t h264AuChunkCount = 0;
		bool h264AuIsIdr = false;
		bool h264AuIsDecoderSync = false;
		std::string h264AuProtectionLevel;
		bool h264AuDroppedBeforeSend = false;
		std::string h264AuDropReason;
		uint64_t h264AuDroppedBytes = 0;
		uint32_t h264AuDroppedChunks = 0;
		double h264AuPacingQueueDelayMs = 0.0;
		double h264AuEstimatedSendMs = 0.0;
		bool h264InputGatedByPacing = false;
		std::string h264InputGateReason;
		double h264InputGateQueueDelayMs = 0.0;
		double h264InputGateVideoCreditBytes = 0.0;
		double h264InputGateFrameBudgetBytes = 0.0;
		double h264InputGateDurationMs = 0.0;
		uint32_t h264InputGateConsecutiveFrames = 0;
		uint64_t h264InputGateSkippedInputFrames = 0;
		bool h264InputGateForcedOpen = false;
		std::string h264InputGateReleaseReason;
		uint64_t fecProtectedH264KeyFrames = 0;
		uint64_t fecProtectedH264LargeFrames = 0;
		uint32_t h264EncoderDelayFrames = 0;
		double h264EncoderDelayMs = 0.0;
		uint32_t h264EncoderPendingFrames = 0;
		uint32_t h264EncodedInputFrameId = 0;
		double h264EncoderCallMs = 0.0;
		double h264EncoderSampleCreateMs = 0.0;
		double h264EncoderProcessInputMs = 0.0;
		double h264EncoderPreInputPollMs = 0.0;
		double h264EncoderPostInputWaitMs = 0.0;
		double h264EncoderProcessOutputMs = 0.0;
		double h264EncoderOutputCopyMs = 0.0;
		uint32_t h264EncoderProcessOutputAttempts = 0;
		uint32_t h264EncoderAsyncEventCount = 0;
		bool h264EncoderHardware = false;
		bool h264EncoderAsync = false;
		bool h264EncoderNeedInputSignaled = false;
		bool h264EncoderOutputProduced = false;
		bool h264EncoderOutputProducedBeforeInput = false;
		bool h264SubmittedNewInput = false;
		bool h264AsyncSubmittedWithoutOutput = false;
		bool h264AsyncPendingNoOutput = false;
		bool h264AsyncCadenceHoldActive = false;
		uint32_t h264AsyncPendingNoOutputStreak = 0;
		double h264AsyncCadenceScale = 1.0;
		double h264AsyncCadenceHoldRemainingMs = 0.0;
		double h264AsyncOutputPollBackoffMs = 0.0;
		uint32_t h264InputCadenceFps = 0;
		bool h264NeedInputSubmitWake = false;
		double h264NeedInputSubmitLeadMs = 0.0;
		uint64_t encodedCameraFrameId = 0;
		int64_t encodedCameraSourceTimestamp100ns = 0;
		uint64_t encodedCameraCaptureCompletedTimeUs = 0;
		double encodedCameraFrameAgeMs = 0.0;
		double encodedCameraReadSampleMs = 0.0;
		double encodedCameraReadSampleEndToCaptureMs = 0.0;
		double encodedCameraCaptureToPublishMs = 0.0;
		double encodedCameraPublishToAcquireMs = 0.0;
		double encodedCameraAcquireToEncoderInputMs = 0.0;
		double sendJpegEncodeMs = 0.0;
		double sendPacketizeMs = 0.0;
		double sendFrameIntervalMs = 0.0;
		bool cameraFrameReady = false;
		uint64_t cameraFrameId = 0;
		int64_t cameraSourceTimestamp100ns = 0;
		uint64_t cameraReadSampleStartTimeUs = 0;
		uint64_t cameraReadSampleEndTimeUs = 0;
		uint64_t cameraCaptureCompletedTimeUs = 0;
		uint64_t cameraFramePublishedTimeUs = 0;
		uint64_t cameraSenderAcquireTimeUs = 0;
		double cameraReadSampleMs = 0.0;
		double cameraReadSampleEndToCaptureMs = 0.0;
		double cameraCaptureToPublishMs = 0.0;
		double cameraPublishToAcquireMs = 0.0;
		double cameraAcquireToSendMs = 0.0;
		double cameraFrameAgeMs = 0.0;
		bool cameraFrameCacheUsed = false;
		double receiveJpegDecodeMs = 0.0;
		double receiveDecodeWorkerFps = 0.0;
		uint64_t receiveDecodeWorkerFrames = 0;
		uint64_t receiveDecodePopSuccesses = 0;
		uint64_t receiveDecodePopEmptyPolls = 0;
		double receiveDecodeLoopLastPopGapMs = 0.0;
		double receiveDecodeLoopMaxPopGapMs = 0.0;
		uint64_t receiveDecodeOverwrittenFrames = 0;
		uint64_t receiveDecodeQueueDroppedFrames = 0;
		uint64_t receiveDecodeRenderOverwriteFrames = 0;
		uint64_t receiveDecodeFailures = 0;
		uint64_t receiveFreshnessDroppedFrames = 0;
		uint64_t receiveH264AuInvalidFrames = 0;
		uint64_t receiveH264AuCrcMismatches = 0;
		uint64_t receiveH264AuPayloadSizeMismatches = 0;
		uint64_t receiveH264AuNalCountMismatches = 0;
		uint64_t receiveH264AuNoAnnexBNals = 0;
		uint64_t receiveH264AuIdrFlagMismatches = 0;
		uint64_t receiveH264AuSpsPpsFlagMismatches = 0;
		uint64_t receiveH264AuSyncWithoutIdr = 0;
		uint64_t receiveH264AuIdrWithoutSpsPps = 0;
		uint64_t receiveH264AuForbiddenZeroBit = 0;
		std::string receiveH264AuLastInvalidReason;
		double receiveDecodeInputFrameAgeMs = 0.0;
		double receiveDecodeInputCameraFrameAgeMs = 0.0;
		double receiveDecodeInputEncoderOutputAgeMs = 0.0;
		double receiveLatestDecodedFrameAgeMs = 0.0;
		double receiveLatestDecodedCameraFrameAgeMs = 0.0;
		double receiveLatestDecodedEncoderOutputAgeMs = 0.0;
		double receiveFreshnessDropThresholdMs = 0.0;
		std::string receiveDecodeLastDropReason;
		double receiveUploadBufferWaitMs = 0.0;
		uint32_t receiveDisplayFrameId = 0;
		double receiveDisplayCameraFrameAgeMs = 0.0;
		double receiveDisplayEncoderOutputAgeMs = 0.0;
		double receiveDisplayDecodedFrameAgeMs = 0.0;
		double textureUploadMs = 0.0;
		double presentGpuWaitMs = 0.0;
		double renderFramePacingWaitMs = 0.0;
		double waitableSwapChainWaitMs = 0.0;
		double presentMs = 0.0;
		uint32_t presentSyncInterval = 1;
		bool lowLatencyPresentMode = false;
		bool waitableSwapChainPacingEnabled = false;
		bool waitableSwapChainAvailable = false;
		uint32_t swapChainBufferCount = 0;

		bool adaptiveQualityChanged = false;
		bool adaptiveFpsChanged = false;
		bool adaptiveBitrateChanged = false;
		bool adaptiveResolutionChanged = false;

		double adaptiveLastAckMissingRate = 0.0;
		double adaptiveLastPacketLossRate = 0.0;
		double adaptiveLastRttMs = 0.0;
		double adaptiveLastLatencyMs = 0.0;
		double adaptiveLastJitterMs = 0.0;
		double adaptiveLastReceiveFps = 0.0;
		double adaptiveLastDecodeFps = 0.0;
		double adaptiveLastDisplayFps = 0.0;
		double adaptiveLastQoeScore = 0.0;
		std::string adaptiveDegradationCause;
		bool adaptiveFecRecoveryWorking = false;
		bool adaptiveFecGuardActive = false;
		double adaptiveFecRecoveryEfficiency = 0.0;
		uint64_t adaptiveFecParityPacketDelta = 0;
		uint64_t adaptiveFecRecoveredFrameDelta = 0;
		uint64_t adaptiveFecRecoveredChunkDelta = 0;

		// ============================================================
		// Jitter
		// ------------------------------------------------------------
		// 連続するフレーム到着間隔の揺れ。
		// RFC3550風の厳密計算ではなく、作品用に分かりやすい
		// 平均絶対変動として扱う。
		// ============================================================
		double currentJitterMs = 0.0;
		double averageJitterMs = 0.0;
		double maxJitterMs = 0.0;

		uint32_t jitterBufferTargetDelayMs = 0;
		uint32_t jitterBufferBufferedFrames = 0;
		uint64_t jitterBufferReleasedFrames = 0;
		uint64_t jitterBufferDroppedFrames = 0;

		bool jitterBufferAutoModeEnabled = false;
		uint32_t jitterBufferAutoCalculatedDelayMs = 0;

		// ============================================================
		// Network Condition Simulator
		// ============================================================
		NetworkCondition networkCondition{};
		NetworkSimulationStats networkSimulation{};

		NetworkRuntimeMode networkRuntimeMode =
			NetworkRuntimeMode::Loopback;
		std::string networkRuntimeModeName;
		bool networkModeSendingEnabled = true;
		bool networkModeReceivingEnabled = true;

		bool networkExperimentActive = false;
		std::string networkExperimentScenarioName;
		std::string networkExperimentAdaptiveMode;
		double networkExperimentRemainingSec = 0.0;
		uint32_t networkExperimentStepIndex = 0;
		uint32_t networkExperimentStepCount = 0;

		// ============================================================
		// Bandwidth
		// ============================================================
		double bitrateMbps = 0.0;     // 受信ペイロード/フレームベースの推定bitrate
		double throughputMbps = 0.0;  // 実際に受信したUDP packet byteベース

		// ============================================================
		// Loss
		// ============================================================
		double packetLossRate = 0.0;
		double frameDropRate = 0.0;

		// ============================================================
		// Debug / Time
		// ============================================================
		uint32_t latestFrameId = 0;
		uint64_t lastUpdateTimeUs = 0;
	};

	class NetworkStats {
	public:
		NetworkStats();

		void Reset();

		// UDP packetを1つ受け取ったときに呼ぶ
		void OnPacketReceived(uint32_t packetBytes);

		// フレーム再構成が完了したときに呼ぶ
		void OnFrameCompleted(
			uint32_t frameId,
			uint64_t frameBytes,
			uint64_t sendTimeUs,
			uint64_t receiveTimeUs
		);

		// Ping/PongでRTTが取れたときに呼ぶ
		void OnRttSample(double rttMs);

		// 欠損や破棄を観測したときに呼ぶ
		void OnDroppedFrame();
		void OnDeadlineDroppedFrames(uint32_t droppedFrames);
		void OnOutputQueueDroppedFrames(uint32_t droppedFrames);
		void OnCompletedQueuePush(
			uint32_t queueSizeAfterPush,
			double popAgeMs,
			double pushIntervalMs
		);
		void OnCompletedQueuePop(uint32_t queueSizeAfterPop);
		void OnCompletedQueueEmptyPoll();
		void OnOutputQueueDropEvent(
			uint32_t droppedFrames,
			uint32_t queueSizeBeforeDrop,
			double oldestDroppedAgeMs,
			double newestFrameAgeMs,
			const char* reason,
			double completedQueuePopAgeMs = 0.0,
			double completedQueuePushIntervalMs = 0.0
		);
		void OnDeadlineNackSent(
			uint32_t missingChunkCount,
			CodecType codecType = CodecType::Unknown,
			bool keyFrame = false,
			bool largeFrame = false
		);
		void OnDeadlineNackRecoveredFrame();
		void OnDeadlineNackExpiredFrame(
			uint32_t missingChunkCount,
			bool nackSent,
			CodecType codecType = CodecType::Unknown,
			bool keyFrame = false,
			bool largeFrame = false
		);
		void OnFecParityPacket();
		void OnFecRecoveredFrame(uint32_t recoveredChunkCount);
		void OnH264ReassemblerAuRejected(const char* reason);
		void OnFrameRecoveryOutcome(
			const char* eventName,
			const char* outcome,
			uint32_t frameId,
			uint32_t streamId,
			CodecType codecType,
			bool keyFrame,
			bool largeFrame,
			uint32_t chunkCount,
			uint32_t receivedChunks,
			uint32_t missingChunks,
			uint32_t fecParityPackets,
			uint32_t fecRecoveredChunks,
			uint32_t nackCount,
			uint32_t postNackReceivedChunks,
			uint32_t nackRequestedChunks,
			uint32_t retransmitReceivedChunks,
			uint32_t retransmitDuplicatePackets,
			uint32_t lastPacketSequence,
			uint32_t lastPacketChunkIndex,
			uint32_t lastRetransmitSequence,
			uint32_t lastRetransmitChunkIndex,
			uint32_t eventPacketSequence,
			uint32_t eventPacketChunkIndex,
			bool eventPacketWasRetransmit,
			uint64_t sendTimeUs,
			uint64_t firstReceiveTimeUs,
			uint64_t eventTimeUs
		);
		void OnRetransmitLateAfterCompletedTiming(
			bool largeFrame,
			uint64_t packetSendTimeUs,
			uint64_t completedTimeUs,
			uint64_t receiveTimeUs
		);
		void OnDynamicNackDeadlineUpdated(
			uint64_t deadlineUs,
			double usefulnessRatio,
			double duplicateRatio,
			double expiredAfterRetransmitRatio,
			const char* reason
		);
		void OnNackSuppressed(
			const char* reason,
			uint32_t missingChunkCount
		);
		void OnNackShapingDecision(
			const char* reason,
			uint32_t missingChunkCount,
			uint32_t requestedChunkBudget,
			bool sent
		);

		// 重複packetを観測したときに呼ぶ
		void OnDuplicatePacket(
			DuplicatePacketKind kind = DuplicatePacketKind::Original
		);

		// sequenceの逆転など、順序入れ替えを観測したときに呼ぶ
		void OnReorderedPacket();

		// デコード成功時に呼ぶ。MJPEG/H.264対応時に使う
		void OnDecodeFrame();

		// 画面表示成功時に呼ぶ。DX12表示側で使う
		void OnDisplayFrame();

		// RNVP sequence の欠番を検出したときに呼ぶ
		void OnMissingPackets(uint64_t missingCount);
		void OnMissingPacketsRecovered(uint64_t recoveredCount);

		// JitterBufferの状態更新
		void OnJitterBufferUpdated(
			uint32_t bufferedFrames,
			uint32_t targetDelayMs
		);

		void OnJitterBufferReleased();

		void OnJitterBufferDropped(uint32_t droppedFrames);

		void OnJitterBufferAutoModeUpdated(
			bool enabled,
			uint32_t calculatedDelayMs
		);

		NetworkStatsSnapshot GetSnapshot() const;

	private:
		uint64_t NowMicroseconds() const;

		void UpdateReceiveFps(uint64_t nowUs);
		void UpdateDecodeFps(uint64_t nowUs);
		void UpdateDisplayFps(uint64_t nowUs);

		void UpdateThroughput(uint64_t nowUs);
		void UpdateBitrate(uint64_t nowUs);

	private:
		mutable std::mutex mutex_;
		NetworkStatsSnapshot snapshot_;

		// ============================================================
		// Internal counters
		// ============================================================
		uint64_t startTimeUs_ = 0;

		uint64_t lastFpsUpdateTimeUs_ = 0;
		uint64_t lastDecodeFpsUpdateTimeUs_ = 0;
		uint64_t lastDisplayFpsUpdateTimeUs_ = 0;

		uint64_t framesAtLastFpsUpdate_ = 0;
		uint64_t decodedFramesAtLastFpsUpdate_ = 0;
		uint64_t displayedFramesAtLastFpsUpdate_ = 0;

		uint64_t bytesAtLastThroughputUpdate_ = 0;
		uint64_t frameBytesAtLastBitrateUpdate_ = 0;

		uint64_t totalFrameBytes_ = 0;

		// latency average
		double latencySumMs_ = 0.0;
		double retransmitLateAfterCompletedDelaySumMs_ = 0.0;
		uint64_t retransmitLateAfterCompletedDelaySamples_ = 0;
		double retransmitLateAfterCompletedSendToCompleteSumMs_ = 0.0;
		uint64_t retransmitLateAfterCompletedSendToCompleteSamples_ = 0;

		// jitter
		bool hasPreviousFrameArrival_ = false;
		uint64_t previousFrameReceiveTimeUs_ = 0;
		double jitterSumMs_ = 0.0;
		uint64_t jitterSamples_ = 0;

		// rtt
		double rttSumMs_ = 0.0;


	};

} // namespace net
