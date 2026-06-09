#include "NetworkCsvLogger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <utility>

namespace net {
namespace {

    std::string MakeTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);

        std::tm localTime{};
        localtime_s(&localTime, &time);

        std::ostringstream oss;
        oss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
        return oss.str();
    }

    bool NearlyEqual(double lhs, double rhs, double epsilon = 0.0001) {
        return std::fabs(lhs - rhs) <= epsilon;
    }

    bool MatchesCondition(
        const NetworkCondition& condition,
        bool enabled,
        double lossRate,
        uint32_t minDelayMs,
        uint32_t maxDelayMs,
        uint32_t burstLossLength,
        double duplicateRate = 0.0,
        double reorderRate = 0.0) {
        return condition.enabled == enabled &&
            NearlyEqual(condition.lossRate, lossRate) &&
            NearlyEqual(condition.duplicateRate, duplicateRate) &&
            NearlyEqual(condition.reorderRate, reorderRate) &&
            condition.minDelayMs == minDelayMs &&
            condition.maxDelayMs == maxDelayMs &&
            condition.burstLossLength == burstLossLength;
    }

    std::string EscapeCsv(std::string value) {
        const bool needsQuote =
            value.find_first_of(",\"\r\n") != std::string::npos;

        if (!needsQuote) {
            return value;
        }

        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back('"');

        for (char ch : value) {
            if (ch == '"') {
                escaped.push_back('"');
            }
            escaped.push_back(ch);
        }

        escaped.push_back('"');
        return escaped;
    }

    int Percent(double ratio) {
        return static_cast<int>(std::lround(ratio * 100.0));
    }

} // namespace

    bool NetworkCsvLogger::Start(const std::string& directory) {
        Stop();

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return false;
        }

        const std::filesystem::path path =
            std::filesystem::path(directory) /
            ("network_" + MakeTimestamp() + ".csv");

        file_.open(path, std::ios::out | std::ios::trunc);
        if (!file_) {
            filePath_.clear();
            return false;
        }

        filePath_ = path.string();
        headerWritten_ = false;
        WriteHeader();
        return true;
    }

    void NetworkCsvLogger::Stop() {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }

        headerWritten_ = false;
    }

    void NetworkCsvLogger::SetScenarioName(std::string name) {
        if (name.empty()) {
            scenarioName_ = "Auto";
            return;
        }

        scenarioName_ = std::move(name);
    }

    void NetworkCsvLogger::WriteSample(
        const NetworkStatsSnapshot& stats,
        double appTimeSec) {
        if (!file_.is_open()) {
            return;
        }

        if (!headerWritten_) {
            WriteHeader();
        }

        file_ << std::fixed << std::setprecision(3)
            << appTimeSec << ','
            << EscapeCsv(ResolveScenarioName(stats)) << ','
            << EscapeCsv(stats.networkRuntimeModeName) << ','
            << (stats.networkModeSendingEnabled ? 1 : 0) << ','
            << (stats.networkModeReceivingEnabled ? 1 : 0) << ','
            << (stats.networkExperimentActive ? 1 : 0) << ','
            << EscapeCsv(stats.networkExperimentScenarioName) << ','
            << EscapeCsv(stats.networkExperimentAdaptiveMode) << ','
            << stats.networkExperimentRemainingSec << ','
            << stats.networkExperimentStepIndex << ','
            << stats.networkExperimentStepCount << ','
            << stats.latestFrameId << ','
            << stats.receiveFps << ','
            << stats.decodeFps << ','
            << stats.displayFps << ','
            << stats.currentLatencyMs << ','
            << stats.averageLatencyMs << ','
            << stats.currentRttMs << ','
            << stats.averageRttMs << ','
            << stats.currentJitterMs << ','
            << stats.packetLossRate << ','
            << stats.frameDropRate << ','
            << stats.receivedPackets << ','
            << stats.missingPackets << ','
            << stats.duplicatePackets << ','
            << stats.reorderedPackets << ','
            << stats.completedFrames << ','
            << stats.droppedFrames << ','
            << stats.deadlineDroppedFrames << ','
            << stats.outputQueueDroppedFrames << ','
            << stats.outputQueueDropEvents << ','
            << stats.outputQueueDropBurstEvents << ','
            << stats.lastOutputQueueDropFrameCount << ','
            << stats.lastOutputQueueDropQueueSize << ','
            << stats.lastOutputQueueDropOldestAgeMs << ','
            << stats.lastOutputQueueDropNewestAgeMs << ','
            << stats.maxOutputQueueDropOldestAgeMs << ','
            << EscapeCsv(stats.lastOutputQueueDropReason) << ','
            << stats.decodedFrames << ','
            << stats.displayedFrames << ','
            << stats.frameRecoveryOutcomeEvents << ','
            << stats.frameRecoveryCompletedFrames << ','
            << stats.frameRecoveryExpiredFrames << ','
            << stats.frameRecoveryRejectedFrames << ','
            << stats.frameRecoveryNackSentFrames << ','
            << stats.frameRecoveryFecRecoveredFrames << ','
            << stats.frameRecoveryLastFrameId << ','
            << stats.frameRecoveryLastStreamId << ','
            << EscapeCsv(stats.frameRecoveryLastEvent) << ','
            << EscapeCsv(stats.frameRecoveryLastOutcome) << ','
            << EscapeCsv(stats.frameRecoveryLastCodec) << ','
            << (stats.frameRecoveryLastKeyFrame ? 1 : 0) << ','
            << (stats.frameRecoveryLastLargeFrame ? 1 : 0) << ','
            << stats.frameRecoveryLastChunkCount << ','
            << stats.frameRecoveryLastReceivedChunks << ','
            << stats.frameRecoveryLastMissingChunks << ','
            << stats.frameRecoveryLastFecParityPackets << ','
            << stats.frameRecoveryLastFecRecoveredChunks << ','
            << stats.frameRecoveryLastNackCount << ','
            << stats.frameRecoveryLastPostNackReceivedChunks << ','
            << stats.frameRecoveryLastNackRequestedChunks << ','
            << stats.frameRecoveryLastRetransmitReceivedChunks << ','
            << stats.frameRecoveryLastRetransmitDuplicatePackets << ','
            << stats.frameRecoveryLastPacketSequence << ','
            << stats.frameRecoveryLastPacketChunkIndex << ','
            << stats.frameRecoveryLastRetransmitSequence << ','
            << stats.frameRecoveryLastRetransmitChunkIndex << ','
            << stats.frameRecoveryLastEventPacketSequence << ','
            << stats.frameRecoveryLastEventPacketChunkIndex << ','
            << (stats.frameRecoveryLastEventWasRetransmit ? 1 : 0) << ','
            << stats.frameRecoveryLastAgeMs << ','
            << stats.retransmitUsefulChunks << ','
            << stats.retransmitDuplicatePackets << ','
            << stats.retransmitLateAfterCompletedPackets << ','
            << stats.retransmitLateAfterExpiredPackets << ','
            << stats.retransmitLateAfterRejectedPackets << ','
            << stats.retransmitClassifiedPackets << ','
            << stats.retransmitNotArrivedPackets << ','
            << stats.retransmitAccountedPackets << ','
            << stats.retransmitUnclassifiedPackets << ','
            << stats.retransmitCompletedFrames << ','
            << stats.retransmitExpiredFrames << ','
            << stats.retransmitUsefulnessRatio << ','
            << stats.retransmitDuplicateRatio << ','
            << stats.retransmitFinalClassificationRatio << ','
            << stats.retransmitFinalAccountingRatio << ','
            << stats.retransmitExpiredAfterUsefulRatio << ','
            << stats.dynamicNackDeadlineMs << ','
            << stats.dynamicNackUsefulnessRatio << ','
            << stats.dynamicNackDuplicateRatio << ','
            << stats.dynamicNackExpiredAfterRetransmitRatio << ','
            << EscapeCsv(stats.dynamicNackDecisionReason) << ','
            << stats.nackSuppressedFrames << ','
            << stats.nackSuppressedMissingChunks << ','
            << stats.nackPreflightSuppressedFrames << ','
            << stats.nackPreflightSuppressedChunks << ','
            << stats.nackFecGraceSuppressedFrames << ','
            << stats.nackFecGraceSuppressedChunks << ','
            << stats.nackPredictedUsefulFrames << ','
            << stats.nackPredictedUsefulChunks << ','
            << stats.nackDeferredForLikelyArrivalFrames << ','
            << stats.nackDeferredForLikelyArrivalChunks << ','
            << stats.nackSkippedTooLateFrames << ','
            << stats.nackSkippedTooLateChunks << ','
            << stats.nackRequestedChunkBudget << ','
            << EscapeCsv(stats.nackLastShapingReason) << ','
            << EscapeCsv(stats.nackLastSuppressionReason) << ','
            << stats.ackCount << ','
            << stats.lastAckMissingRate << ','
            << stats.ackRetransmittedFrames << ','
            << stats.ackRetransmittedChunks << ','
            << stats.repairCanceledByCompleteAckPackets << ','
            << stats.repairSkippedByTtlPackets << ','
            << stats.repairQueuedButCanceledPackets << ','
            << stats.repairSuppressedByFecLikelyFrames << ','
            << stats.repairSuppressedByFecLikelyPackets << ','
            << stats.repairFecLikelySuppressedCompletedFrames << ','
            << stats.repairFecLikelySuppressedCompletedPackets << ','
            << stats.repairFecLikelySuppressedExpiredFrames << ','
            << stats.repairFecLikelySuppressedExpiredPackets << ','
            << stats.repairFecLikelySuppressedPendingFrames << ','
            << stats.repairFecLikelySuppressedPendingPackets << ','
            << stats.repairFecLikelySuppressionRescueFrames << ','
            << stats.repairFecLikelySuppressionRescuePackets << ','
            << stats.lateRepairSavedPackets << ','
            << stats.ackStaleDroppedFrames << ','
            << stats.ackKeyFrameRequests << ','
            << (stats.ackKeyFramePending ? 1 : 0) << ','
            << (stats.pacingEnabled ? 1 : 0) << ','
            << stats.pacingTargetBitrateBps << ','
            << stats.pacingRepairTargetBitrateBps << ','
            << stats.pacingQueuedPackets << ','
            << stats.pacingHighPriorityQueuedPackets << ','
            << stats.pacingNormalQueuedPackets << ','
            << stats.pacingEnqueuedPackets << ','
            << stats.pacingSentPackets << ','
            << stats.pacingSentBytes << ','
            << stats.pacingRepairSentPackets << ','
            << stats.pacingRepairSentBytes << ','
            << stats.pacingRepairBorrowedPackets << ','
            << stats.pacingRepairBorrowedBytes << ','
            << stats.pacingDroppedPackets << ','
            << stats.pacingDeadlineDroppedPackets << ','
            << stats.pacingHighPriorityDeadlineDroppedPackets << ','
            << stats.pacingNormalDeadlineDroppedPackets << ','
            << stats.pacingOverflowDroppedPackets << ','
            << stats.pacingCurrentQueueDelayMs << ','
            << stats.pacingMaxQueueDelayMs << ','
            << stats.pacingVideoCreditBytes << ','
            << stats.pacingRepairCreditBytes << ','
            << stats.transportFeedbackPackets << ','
            << stats.transportFeedbackPacketStatuses << ','
            << stats.transportFeedbackReceivedPackets << ','
            << stats.transportFeedbackMissingPackets << ','
            << stats.transportFeedbackLossRate << ','
            << stats.transportFeedbackArrivalJitterMs << ','
            << stats.transportFeedbackQueueDelayTrendMs << ','
            << stats.transportFeedbackLastSequence << ','
            << stats.estimatedBandwidthBps << ','
            << stats.deliveryRateBps << ','
            << stats.bandwidthQueueDelayMs << ','
            << stats.bandwidthRttTrendMs << ','
            << stats.bandwidthLossTrend << ','
            << stats.bandwidthJitterTrendMs << ','
            << stats.bandwidthFeedbackSamples << ','
            << stats.deadlineNackSentFrames << ','
            << stats.deadlineNackRecoveredFrames << ','
            << stats.deadlineNackMissingChunks << ','
            << stats.deadlineNackSentH264KeyFrames << ','
            << stats.deadlineNackSentH264LargeFrames << ','
            << stats.deadlineNackSentH264DeltaFrames << ','
            << stats.deadlineNackExpiredDroppedFrames << ','
            << stats.deadlineNackExpiredAfterNackFrames << ','
            << stats.deadlineNackExpiredMissingChunks << ','
            << stats.deadlineNackExpiredH264KeyFrames << ','
            << stats.deadlineNackExpiredH264LargeFrames << ','
            << stats.deadlineNackExpiredH264DeltaFrames << ','
            << (stats.fecEnabled ? 1 : 0) << ','
            << (stats.adaptiveFecEnabled ? 1 : 0) << ','
            << stats.fecGroupChunkCount << ','
            << stats.fecParityPackets << ','
            << stats.fecRecoveredFrames << ','
            << stats.fecRecoveredChunks << ','
            << stats.h264ReassemblerAuRejectedFrames << ','
            << stats.h264ReassemblerHeaderFailures << ','
            << stats.h264ReassemblerPayloadSizeMismatches << ','
            << stats.h264ReassemblerFrameIdMismatches << ','
            << stats.h264ReassemblerCrcMismatches << ','
            << EscapeCsv(stats.h264ReassemblerLastRejectReason) << ','
            << EscapeCsv(stats.adaptiveFecDecisionReason) << ','
            << EscapeCsv(stats.adaptiveFecHoldReason) << ','
            << (stats.adaptiveFecG8ToG4Recovery ? 1 : 0) << ','
            << (stats.adaptiveFecQualityHoldActive ? 1 : 0) << ','
            << (stats.adaptiveFecQualityHoldCanceled ? 1 : 0) << ','
            << (stats.adaptiveFecEmergencyG2Active ? 1 : 0) << ','
            << EscapeCsv(stats.adaptiveFecEarlyOffReason) << ','
            << (stats.adaptiveEnabled ? 1 : 0) << ','
            << EscapeCsv(stats.adaptiveControlMode) << ','
            << EscapeCsv(stats.adaptiveCongestionControlMode) << ','
            << stats.adaptiveTargetJpegQuality << ','
            << stats.adaptiveTargetFps << ','
            << stats.adaptiveTargetBitrateKbps << ','
            << stats.adaptiveBandwidthCeilingKbps << ','
            << stats.adaptiveTargetWidth << ','
            << stats.adaptiveTargetHeight << ','
            << EscapeCsv(stats.sendActualCodec) << ','
            << stats.sendActualEncodeWidth << ','
            << stats.sendActualEncodeHeight << ','
            << stats.sendActualRawFrameBytes << ','
            << stats.sendActualEncodedFrameBytes << ','
            << stats.adaptiveRawFrameBytes << ','
            << stats.adaptiveEncodedFrameBytes << ','
            << stats.adaptiveCompressionRatio << ','
            << stats.captureFps << ','
            << stats.encodeMs << ','
            << stats.sendResizeMs << ','
            << stats.sendNv12PrepareMs << ','
            << stats.sendH264EncodeMs << ','
            << stats.sendH264EncoderRequestedBitrateKbps << ','
            << stats.sendH264EncoderTargetBitrateKbps << ','
            << stats.sendH264EncoderAppliedBitrateKbps << ','
            << stats.h264DynamicBitrateUpdateRequests << ','
            << stats.h264DynamicBitrateUpdateSuccesses << ','
            << stats.h264DynamicBitrateUpdateFailures << ','
            << stats.h264EncoderReinitializations << ','
            << stats.sendPacingTargetBitrateKbps << ','
            << stats.sendH264VideoBudgetScale << ','
            << (stats.pacingBurstGuardActive ? 1 : 0) << ','
            << stats.adaptiveRepairBudgetUtilization << ','
            << stats.adaptiveRepairBorrowedRatio << ','
            << stats.adaptiveRepairSentBytesDelta << ','
            << stats.adaptiveRepairBorrowedBytesDelta << ','
            << (stats.adaptiveRepairBudgetGuardActive ? 1 : 0) << ','
            << (stats.adaptiveRepairVideoBudgetPressure ? 1 : 0) << ','
            << stats.adaptiveRetransmitUsefulRatio << ','
            << stats.adaptiveLateRepairWasteRatio << ','
            << stats.adaptiveRetransmitNotArrivedRatio << ','
            << (stats.adaptiveRetransmitAccountingComplete ? 1 : 0) << ','
            << (stats.adaptiveLateRepairWastePressure ? 1 : 0) << ','
            << (stats.adaptiveRetransmitNotArrivedPressure ? 1 : 0) << ','
            << EscapeCsv(stats.adaptiveRepairDecisionReason) << ','
            << stats.h264AuChunkCount << ','
            << (stats.h264AuIsIdr ? 1 : 0) << ','
            << (stats.h264AuIsDecoderSync ? 1 : 0) << ','
            << EscapeCsv(stats.h264AuProtectionLevel) << ','
            << stats.fecProtectedH264KeyFrames << ','
            << stats.fecProtectedH264LargeFrames << ','
            << stats.h264EncoderDelayFrames << ','
            << stats.h264EncoderDelayMs << ','
            << stats.h264EncoderPendingFrames << ','
            << stats.h264EncodedInputFrameId << ','
            << stats.encodedCameraFrameId << ','
            << stats.encodedCameraSourceTimestamp100ns << ','
            << stats.encodedCameraCaptureCompletedTimeUs << ','
            << stats.encodedCameraFrameAgeMs << ','
            << stats.sendJpegEncodeMs << ','
            << stats.sendPacketizeMs << ','
            << stats.sendFrameIntervalMs << ','
            << (stats.cameraFrameReady ? 1 : 0) << ','
            << stats.cameraFrameId << ','
            << stats.cameraSourceTimestamp100ns << ','
            << stats.cameraCaptureCompletedTimeUs << ','
            << stats.cameraReadSampleMs << ','
            << stats.cameraFrameAgeMs << ','
            << (stats.cameraFrameCacheUsed ? 1 : 0) << ','
            << stats.receiveJpegDecodeMs << ','
            << stats.receiveDecodeWorkerFps << ','
            << stats.receiveDecodeWorkerFrames << ','
            << stats.receiveDecodeOverwrittenFrames << ','
            << stats.receiveDecodeQueueDroppedFrames << ','
            << stats.receiveDecodeRenderOverwriteFrames << ','
            << stats.receiveDecodeFailures << ','
            << stats.receiveFreshnessDroppedFrames << ','
            << stats.receiveH264AuInvalidFrames << ','
            << stats.receiveH264AuCrcMismatches << ','
            << stats.receiveH264AuPayloadSizeMismatches << ','
            << stats.receiveH264AuNalCountMismatches << ','
            << stats.receiveH264AuNoAnnexBNals << ','
            << stats.receiveH264AuIdrFlagMismatches << ','
            << stats.receiveH264AuSpsPpsFlagMismatches << ','
            << stats.receiveH264AuSyncWithoutIdr << ','
            << stats.receiveH264AuIdrWithoutSpsPps << ','
            << stats.receiveH264AuForbiddenZeroBit << ','
            << EscapeCsv(stats.receiveH264AuLastInvalidReason) << ','
            << stats.receiveDecodeInputFrameAgeMs << ','
            << stats.receiveLatestDecodedFrameAgeMs << ','
            << stats.receiveFreshnessDropThresholdMs << ','
            << EscapeCsv(stats.receiveDecodeLastDropReason) << ','
            << stats.receiveUploadBufferWaitMs << ','
            << stats.textureUploadMs << ','
            << stats.presentGpuWaitMs << ','
            << stats.renderFramePacingWaitMs << ','
            << stats.waitableSwapChainWaitMs << ','
            << stats.presentMs << ','
            << stats.presentSyncInterval << ','
            << (stats.lowLatencyPresentMode ? 1 : 0) << ','
            << (stats.waitableSwapChainPacingEnabled ? 1 : 0) << ','
            << (stats.waitableSwapChainAvailable ? 1 : 0) << ','
            << stats.swapChainBufferCount << ','
            << stats.adaptiveLastPacketLossRate << ','
            << stats.adaptiveLastReceiveFps << ','
            << stats.adaptiveLastDecodeFps << ','
            << stats.adaptiveLastJitterMs << ','
            << stats.adaptiveLastDisplayFps << ','
            << stats.adaptiveLastQoeScore << ','
            << EscapeCsv(stats.adaptiveDegradationCause) << ','
            << (stats.adaptiveFecRecoveryWorking ? 1 : 0) << ','
            << (stats.adaptiveFecGuardActive ? 1 : 0) << ','
            << stats.adaptiveFecRecoveryEfficiency << ','
            << stats.adaptiveFecParityPacketDelta << ','
            << stats.adaptiveFecRecoveredFrameDelta << ','
            << stats.adaptiveFecRecoveredChunkDelta << ','
            << (stats.networkCondition.enabled ? 1 : 0) << ','
            << stats.networkCondition.lossRate << ','
            << stats.networkCondition.duplicateRate << ','
            << stats.networkCondition.reorderRate << ','
            << stats.networkCondition.minDelayMs << ','
            << stats.networkCondition.maxDelayMs << ','
            << stats.networkCondition.burstLossLength << ','
            << stats.networkSimulation.submittedPackets << ','
            << stats.networkSimulation.sentPackets << ','
            << stats.networkSimulation.droppedPackets << ','
            << stats.networkSimulation.duplicatedPackets << ','
            << stats.networkSimulation.reorderedPackets
            << '\n';

        file_.flush();
    }

    bool NetworkCsvLogger::IsRunning() const {
        return file_.is_open();
    }

    const std::string& NetworkCsvLogger::FilePath() const {
        return filePath_;
    }

    void NetworkCsvLogger::WriteHeader() {
        if (!file_.is_open() || headerWritten_) {
            return;
        }

        file_
            << "timeSec,"
            << "scenarioName,"
            << "networkRuntimeMode,"
            << "networkModeSendingEnabled,"
            << "networkModeReceivingEnabled,"
            << "networkExperimentActive,"
            << "networkExperimentScenarioName,"
            << "networkExperimentAdaptiveMode,"
            << "networkExperimentRemainingSec,"
            << "networkExperimentStepIndex,"
            << "networkExperimentStepCount,"
            << "frameId,"
            << "receiveFps,"
            << "decodeFps,"
            << "displayFps,"
            << "currentLatencyMs,"
            << "averageLatencyMs,"
            << "currentRttMs,"
            << "averageRttMs,"
            << "currentJitterMs,"
            << "packetLossRate,"
            << "frameDropRate,"
            << "receivedPackets,"
            << "missingPackets,"
            << "duplicatePackets,"
            << "reorderedPackets,"
            << "completedFrames,"
            << "droppedFrames,"
            << "deadlineDroppedFrames,"
            << "outputQueueDroppedFrames,"
            << "outputQueueDropEvents,"
            << "outputQueueDropBurstEvents,"
            << "lastOutputQueueDropFrameCount,"
            << "lastOutputQueueDropQueueSize,"
            << "lastOutputQueueDropOldestAgeMs,"
            << "lastOutputQueueDropNewestAgeMs,"
            << "maxOutputQueueDropOldestAgeMs,"
            << "lastOutputQueueDropReason,"
            << "decodedFrames,"
            << "displayedFrames,"
            << "frameRecoveryOutcomeEvents,"
            << "frameRecoveryCompletedFrames,"
            << "frameRecoveryExpiredFrames,"
            << "frameRecoveryRejectedFrames,"
            << "frameRecoveryNackSentFrames,"
            << "frameRecoveryFecRecoveredFrames,"
            << "frameRecoveryLastFrameId,"
            << "frameRecoveryLastStreamId,"
            << "frameRecoveryLastEvent,"
            << "frameRecoveryLastOutcome,"
            << "frameRecoveryLastCodec,"
            << "frameRecoveryLastKeyFrame,"
            << "frameRecoveryLastLargeFrame,"
            << "frameRecoveryLastChunkCount,"
            << "frameRecoveryLastReceivedChunks,"
            << "frameRecoveryLastMissingChunks,"
            << "frameRecoveryLastFecParityPackets,"
            << "frameRecoveryLastFecRecoveredChunks,"
            << "frameRecoveryLastNackCount,"
            << "frameRecoveryLastPostNackReceivedChunks,"
            << "frameRecoveryLastNackRequestedChunks,"
            << "frameRecoveryLastRetransmitReceivedChunks,"
            << "frameRecoveryLastRetransmitDuplicatePackets,"
            << "frameRecoveryLastPacketSequence,"
            << "frameRecoveryLastPacketChunkIndex,"
            << "frameRecoveryLastRetransmitSequence,"
            << "frameRecoveryLastRetransmitChunkIndex,"
            << "frameRecoveryLastEventPacketSequence,"
            << "frameRecoveryLastEventPacketChunkIndex,"
            << "frameRecoveryLastEventWasRetransmit,"
            << "frameRecoveryLastAgeMs,"
            << "retransmitUsefulChunks,"
            << "retransmitDuplicatePackets,"
            << "retransmitLateAfterCompletedPackets,"
            << "retransmitLateAfterExpiredPackets,"
            << "retransmitLateAfterRejectedPackets,"
            << "retransmitClassifiedPackets,"
            << "retransmitNotArrivedPackets,"
            << "retransmitAccountedPackets,"
            << "retransmitUnclassifiedPackets,"
            << "retransmitCompletedFrames,"
            << "retransmitExpiredFrames,"
            << "retransmitUsefulnessRatio,"
            << "retransmitDuplicateRatio,"
            << "retransmitFinalClassificationRatio,"
            << "retransmitFinalAccountingRatio,"
            << "retransmitExpiredAfterUsefulRatio,"
            << "dynamicNackDeadlineMs,"
            << "dynamicNackUsefulnessRatio,"
            << "dynamicNackDuplicateRatio,"
            << "dynamicNackExpiredAfterRetransmitRatio,"
            << "dynamicNackDecisionReason,"
            << "nackSuppressedFrames,"
            << "nackSuppressedMissingChunks,"
            << "nackPreflightSuppressedFrames,"
            << "nackPreflightSuppressedChunks,"
            << "nackFecGraceSuppressedFrames,"
            << "nackFecGraceSuppressedChunks,"
            << "nackPredictedUsefulFrames,"
            << "nackPredictedUsefulChunks,"
            << "nackDeferredForLikelyArrivalFrames,"
            << "nackDeferredForLikelyArrivalChunks,"
            << "nackSkippedTooLateFrames,"
            << "nackSkippedTooLateChunks,"
            << "nackRequestedChunkBudget,"
            << "nackLastShapingReason,"
            << "nackLastSuppressionReason,"
            << "ackCount,"
            << "lastAckMissingRate,"
            << "ackRetransmittedFrames,"
            << "ackRetransmittedChunks,"
            << "repairCanceledByCompleteAckPackets,"
            << "repairSkippedByTtlPackets,"
            << "repairQueuedButCanceledPackets,"
            << "repairSuppressedByFecLikelyFrames,"
            << "repairSuppressedByFecLikelyPackets,"
            << "repairFecLikelySuppressedCompletedFrames,"
            << "repairFecLikelySuppressedCompletedPackets,"
            << "repairFecLikelySuppressedExpiredFrames,"
            << "repairFecLikelySuppressedExpiredPackets,"
            << "repairFecLikelySuppressedPendingFrames,"
            << "repairFecLikelySuppressedPendingPackets,"
            << "repairFecLikelySuppressionRescueFrames,"
            << "repairFecLikelySuppressionRescuePackets,"
            << "lateRepairSavedPackets,"
            << "ackStaleDroppedFrames,"
            << "ackKeyFrameRequests,"
            << "ackKeyFramePending,"
            << "pacingEnabled,"
            << "pacingTargetBitrateBps,"
            << "pacingRepairTargetBitrateBps,"
            << "pacingQueuedPackets,"
            << "pacingHighPriorityQueuedPackets,"
            << "pacingNormalQueuedPackets,"
            << "pacingEnqueuedPackets,"
            << "pacingSentPackets,"
            << "pacingSentBytes,"
            << "pacingRepairSentPackets,"
            << "pacingRepairSentBytes,"
            << "pacingRepairBorrowedPackets,"
            << "pacingRepairBorrowedBytes,"
            << "pacingDroppedPackets,"
            << "pacingDeadlineDroppedPackets,"
            << "pacingHighPriorityDeadlineDroppedPackets,"
            << "pacingNormalDeadlineDroppedPackets,"
            << "pacingOverflowDroppedPackets,"
            << "pacingCurrentQueueDelayMs,"
            << "pacingMaxQueueDelayMs,"
            << "pacingVideoCreditBytes,"
            << "pacingRepairCreditBytes,"
            << "transportFeedbackPackets,"
            << "transportFeedbackPacketStatuses,"
            << "transportFeedbackReceivedPackets,"
            << "transportFeedbackMissingPackets,"
            << "transportFeedbackLossRate,"
            << "transportFeedbackArrivalJitterMs,"
            << "transportFeedbackQueueDelayTrendMs,"
            << "transportFeedbackLastSequence,"
            << "estimatedBandwidthBps,"
            << "deliveryRateBps,"
            << "bandwidthQueueDelayMs,"
            << "bandwidthRttTrendMs,"
            << "bandwidthLossTrend,"
            << "bandwidthJitterTrendMs,"
            << "bandwidthFeedbackSamples,"
            << "deadlineNackSentFrames,"
            << "deadlineNackRecoveredFrames,"
            << "deadlineNackMissingChunks,"
            << "deadlineNackSentH264KeyFrames,"
            << "deadlineNackSentH264LargeFrames,"
            << "deadlineNackSentH264DeltaFrames,"
            << "deadlineNackExpiredDroppedFrames,"
            << "deadlineNackExpiredAfterNackFrames,"
            << "deadlineNackExpiredMissingChunks,"
            << "deadlineNackExpiredH264KeyFrames,"
            << "deadlineNackExpiredH264LargeFrames,"
            << "deadlineNackExpiredH264DeltaFrames,"
            << "fecEnabled,"
            << "adaptiveFecEnabled,"
            << "fecGroupChunkCount,"
            << "fecParityPackets,"
            << "fecRecoveredFrames,"
            << "fecRecoveredChunks,"
            << "h264ReassemblerAuRejectedFrames,"
            << "h264ReassemblerHeaderFailures,"
            << "h264ReassemblerPayloadSizeMismatches,"
            << "h264ReassemblerFrameIdMismatches,"
            << "h264ReassemblerCrcMismatches,"
            << "h264ReassemblerLastRejectReason,"
            << "adaptiveFecDecisionReason,"
            << "adaptiveFecHoldReason,"
            << "adaptiveFecG8ToG4Recovery,"
            << "adaptiveFecQualityHoldActive,"
            << "adaptiveFecQualityHoldCanceled,"
            << "adaptiveFecEmergencyG2Active,"
            << "adaptiveFecEarlyOffReason,"
            << "adaptiveEnabled,"
            << "adaptiveControlMode,"
            << "adaptiveCongestionControlMode,"
            << "targetJpegQuality,"
            << "targetFps,"
            << "targetBitrateKbps,"
            << "adaptiveBandwidthCeilingKbps,"
            << "targetWidth,"
            << "targetHeight,"
            << "actualCodec,"
            << "actualEncodeWidth,"
            << "actualEncodeHeight,"
            << "actualRawFrameBytes,"
            << "actualEncodedFrameBytes,"
            << "rawFrameBytes,"
            << "encodedFrameBytes,"
            << "compressionRatio,"
            << "captureFps,"
            << "encodeMs,"
            << "resizeMs,"
            << "nv12PrepareMs,"
            << "h264EncodeMs,"
            << "h264EncoderRequestedBitrateKbps,"
            << "h264EncoderTargetBitrateKbps,"
            << "h264EncoderAppliedBitrateKbps,"
            << "h264DynamicBitrateUpdateRequests,"
            << "h264DynamicBitrateUpdateSuccesses,"
            << "h264DynamicBitrateUpdateFailures,"
            << "h264EncoderReinitializations,"
            << "pacingBudgetTargetBitrateKbps,"
            << "h264VideoBudgetScale,"
            << "pacingBurstGuardActive,"
            << "adaptiveRepairBudgetUtilization,"
            << "adaptiveRepairBorrowedRatio,"
            << "adaptiveRepairSentBytesDelta,"
            << "adaptiveRepairBorrowedBytesDelta,"
            << "adaptiveRepairBudgetGuardActive,"
            << "adaptiveRepairVideoBudgetPressure,"
            << "adaptiveRetransmitUsefulRatio,"
            << "adaptiveLateRepairWasteRatio,"
            << "adaptiveRetransmitNotArrivedRatio,"
            << "adaptiveRetransmitAccountingComplete,"
            << "adaptiveLateRepairWastePressure,"
            << "adaptiveRetransmitNotArrivedPressure,"
            << "adaptiveRepairDecisionReason,"
            << "h264AuChunkCount,"
            << "h264AuIsIdr,"
            << "h264AuIsDecoderSync,"
            << "h264AuProtectionLevel,"
            << "fecProtectedH264KeyFrames,"
            << "fecProtectedH264LargeFrames,"
            << "h264EncoderDelayFrames,"
            << "h264EncoderDelayMs,"
            << "h264EncoderPendingFrames,"
            << "h264EncodedInputFrameId,"
            << "encodedCameraFrameId,"
            << "encodedCameraSourceTimestamp100ns,"
            << "encodedCameraCaptureCompletedTimeUs,"
            << "encodedCameraFrameAgeMs,"
            << "jpegEncodeMs,"
            << "packetizeMs,"
            << "sendFrameIntervalMs,"
            << "cameraFrameReady,"
            << "cameraFrameId,"
            << "cameraSourceTimestamp100ns,"
            << "cameraCaptureCompletedTimeUs,"
            << "cameraReadSampleMs,"
            << "cameraFrameAgeMs,"
            << "cameraFrameCacheUsed,"
            << "receiveJpegDecodeMs,"
            << "receiveDecodeWorkerFps,"
            << "receiveDecodeWorkerFrames,"
            << "receiveDecodeOverwrittenFrames,"
            << "receiveDecodeQueueDroppedFrames,"
            << "receiveDecodeRenderOverwriteFrames,"
            << "receiveDecodeFailures,"
            << "receiveFreshnessDroppedFrames,"
            << "receiveH264AuInvalidFrames,"
            << "receiveH264AuCrcMismatches,"
            << "receiveH264AuPayloadSizeMismatches,"
            << "receiveH264AuNalCountMismatches,"
            << "receiveH264AuNoAnnexBNals,"
            << "receiveH264AuIdrFlagMismatches,"
            << "receiveH264AuSpsPpsFlagMismatches,"
            << "receiveH264AuSyncWithoutIdr,"
            << "receiveH264AuIdrWithoutSpsPps,"
            << "receiveH264AuForbiddenZeroBit,"
            << "receiveH264AuLastInvalidReason,"
            << "receiveDecodeInputFrameAgeMs,"
            << "receiveLatestDecodedFrameAgeMs,"
            << "receiveFreshnessDropThresholdMs,"
            << "receiveDecodeLastDropReason,"
            << "receiveUploadBufferWaitMs,"
            << "textureUploadMs,"
            << "presentGpuWaitMs,"
            << "renderFramePacingWaitMs,"
            << "waitableSwapChainWaitMs,"
            << "presentMs,"
            << "presentSyncInterval,"
            << "lowLatencyPresentMode,"
            << "waitableSwapChainPacingEnabled,"
            << "waitableSwapChainAvailable,"
            << "swapChainBufferCount,"
            << "adaptiveInputPacketLossRate,"
            << "adaptiveInputReceiveFps,"
            << "adaptiveInputDecodeFps,"
            << "adaptiveInputJitterMs,"
            << "adaptiveInputDisplayFps,"
            << "adaptiveQoeScore,"
            << "adaptiveDegradationCause,"
            << "adaptiveFecRecoveryWorking,"
            << "adaptiveFecGuardActive,"
            << "adaptiveFecRecoveryEfficiency,"
            << "adaptiveFecParityPacketDelta,"
            << "adaptiveFecRecoveredFrameDelta,"
            << "adaptiveFecRecoveredChunkDelta,"
            << "simEnabled,"
            << "simLossRate,"
            << "simDuplicateRate,"
            << "simReorderRate,"
            << "simMinDelayMs,"
            << "simMaxDelayMs,"
            << "simBurstLossLength,"
            << "simSubmittedPackets,"
            << "simSentPackets,"
            << "simDroppedPackets,"
            << "simDuplicatedPackets,"
            << "simReorderedPackets"
            << '\n';

        headerWritten_ = true;
    }

    std::string NetworkCsvLogger::ResolveScenarioName(
        const NetworkStatsSnapshot& stats) const {
        if (!scenarioName_.empty() && scenarioName_ != "Auto") {
            return scenarioName_;
        }

        const NetworkCondition& condition = stats.networkCondition;

        if (!condition.enabled) {
            return "Baseline";
        }
        if (MatchesCondition(condition, true, 0.10, 0, 0, 0)) {
            return "10% loss";
        }
        if (MatchesCondition(condition, true, 0.0, 0, 50, 0)) {
            return "50ms jitter";
        }
        if (MatchesCondition(condition, true, 0.0, 100, 100, 0)) {
            return "100ms delay";
        }
        if (MatchesCondition(condition, true, 0.03, 0, 0, 8)) {
            return "Burst loss";
        }

        std::ostringstream oss;
        oss << "Custom"
            << "_loss" << Percent(condition.lossRate)
            << "_delay" << condition.minDelayMs
            << "-" << condition.maxDelayMs
            << "_dup" << Percent(condition.duplicateRate)
            << "_reorder" << Percent(condition.reorderRate)
            << "_burst" << condition.burstLossLength;

        return oss.str();
    }

} // namespace net
