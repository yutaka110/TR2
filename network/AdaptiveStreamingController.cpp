#include "AdaptiveStreamingController.h"

#include <algorithm>
#include <cmath>

namespace net {

    const char* ToString(AdaptiveDegradationCause cause) {
        switch (cause) {
        case AdaptiveDegradationCause::PacketLoss:
            return "Loss";
        case AdaptiveDegradationCause::Jitter:
            return "Jitter";
        case AdaptiveDegradationCause::Rtt:
            return "RTT";
        case AdaptiveDegradationCause::Bandwidth:
            return "Bandwidth";
        case AdaptiveDegradationCause::DecodeLoad:
            return "DecodeLoad";
        case AdaptiveDegradationCause::DisplayLoad:
            return "DisplayLoad";
        case AdaptiveDegradationCause::FrameFreshness:
            return "FrameFreshness";
        case AdaptiveDegradationCause::RecoveryDeadline:
            return "RecoveryDeadline";
        case AdaptiveDegradationCause::PacingQueue:
            return "PacingQueue";
        case AdaptiveDegradationCause::None:
        default:
            return "None";
        }
    }

    const char* ToString(AdaptiveControlMode mode) {
        switch (mode) {
        case AdaptiveControlMode::FixedQuality:
            return "Fixed Quality";
        case AdaptiveControlMode::LossReactive:
            return "Loss Reactive";
        case AdaptiveControlMode::QoeDeadlineAdaptive:
        default:
            return "QoE/Deadline Adaptive";
        }
    }

    const char* ToString(CongestionControlMode mode) {
        switch (mode) {
        case CongestionControlMode::LossBased:
            return "Loss Based";
        case CongestionControlMode::DelayBased:
            return "Delay Based";
        case CongestionControlMode::Hybrid:
        default:
            return "Hybrid";
        }
    }

    AdaptiveStreamingController::AdaptiveStreamingController() {
        Reset();
    }

    void AdaptiveStreamingController::Reset() {
        state_ = AdaptiveStreamingState{};
        state_.controlMode = controlMode_;
        state_.congestionControlMode = ResolveActiveCongestionControlMode();
        InitializeTargetsForMode();

        state_.qualityChanged = false;
        state_.fpsChanged = false;
        state_.bitrateChanged = false;
        state_.resolutionChanged = false;
        state_.lastAdaptiveFecG8ToG4Recovery = false;
        state_.lastAdaptiveFecQualityHoldActive = false;
        state_.lastAdaptiveFecQualityHoldCanceled = false;

        stableTimeSec_ = 0.0;
        badTimeSec_ = 0.0;
        lossOnlyBadTimeSec_ = 0.0;
        cooldownSec_ = 0.0;
        observedTimeSec_ = 0.0;
        degradationCauseHoldSec_ = 0.0;
        qoeHoldSec_ = 0.0;
        heldQoeScore_ = 0.0;
        heldDegradationCause_ = AdaptiveDegradationCause::None;
        hasDropCounters_ = false;
        lastDeadlineDroppedFrames_ = 0;
        lastOutputQueueDroppedFrames_ = 0;
        hasNackCounters_ = false;
        lastDeadlineNackSentFrames_ = 0;
        lastDeadlineNackMissingChunks_ = 0;
        lastDeadlineNackExpiredDroppedFrames_ = 0;
        lastDeadlineNackExpiredAfterNackFrames_ = 0;
        lastAckStaleDroppedFrames_ = 0;
        lastAckKeyFrameRequests_ = 0;
        hasFreshnessCounters_ = false;
        lastReceiveFreshnessDroppedFrames_ = 0;
        hasReceiveDecodeDropCounters_ = false;
        lastReceiveDecodeQueueDroppedFrames_ = 0;
        lastReceiveDecodeRenderOverwriteFrames_ = 0;
        hasPacingCounters_ = false;
        lastPacingDeadlineDroppedPackets_ = 0;
        lastPacingHighPriorityDeadlineDroppedPackets_ = 0;
        hasRepairCounters_ = false;
        lastPacingRepairSentBytes_ = 0;
        lastPacingRepairBorrowedBytes_ = 0;
        hasRetransmitOutcomeCounters_ = false;
        lastRetransmitUsefulChunks_ = 0;
        lastRetransmitDuplicatePackets_ = 0;
        lastRetransmitLateAfterCompletedPackets_ = 0;
        lastRetransmitLateAfterExpiredPackets_ = 0;
        lastRetransmitLateAfterRejectedPackets_ = 0;
        lastRetransmitNotArrivedPackets_ = 0;
        lastRetransmitAccountedPackets_ = 0;
        lastNackFecGraceSuppressedFrames_ = 0;
        lastNackFecGraceSuppressedChunks_ = 0;
        hasFecCounters_ = false;
        lastFecParityPackets_ = 0;
        lastFecRecoveredFrames_ = 0;
        lastFecRecoveredChunks_ = 0;
        fecRecoveryGuardSec_ = 0.0;
        lastEffectiveFecRecoveryEfficiency_ = 0.0;
        fecRecoveryEvidenceSec_ = 0.0;
        recoveryDeadlineFallbackSamples_ = 0;
        nackExpiredRisingSamples_ = 0;
        nackExpiredGuardReleaseSec_ = 0.0;
        postNackExpiredGuardRearmSec_ = 0.0;
        hasAdaptiveFecGroupSample_ = false;
        lastAdaptiveFecSampleEnabled_ = false;
        lastAdaptiveFecSampleGroupChunkCount_ = 0;
        postG8ToG4QualityHoldSec_ = 0.0;
        fecBurstTailRecoverySec_ = 0.0;
        pacingBurstGuardSec_ = 0.0;
        pacingBurstPressureSec_ = 0.0;
        pacingBurstVideoBudgetScale_ = 1.0;
        repairBudgetGuardSec_ = 0.0;
        repairBorrowPressureSec_ = 0.0;
        retransmitNotArrivedPressureSec_ = 0.0;
        lateRepairWasteGuardSec_ = 0.0;
        repairTelemetryHoldSec_ = 0.0;
        nackExpiredBitrateOnlyDecreaseSamples_ = 0;
        nackExpiredBitrateOnlyDecreaseBudget_ = 2;
        activeBandwidthCeilingKbps_ = kMaxBitrateKbps;
        state_.bandwidthCeilingKbps = activeBandwidthCeilingKbps_;
    }

    void AdaptiveStreamingController::SetEnabled(bool enabled) {
        enabled_ = enabled;
    }

    bool AdaptiveStreamingController::IsEnabled() const {
        return enabled_;
    }

    void AdaptiveStreamingController::SetControlMode(AdaptiveControlMode mode) {
        if (controlMode_ == mode) {
            return;
        }

        controlMode_ = mode;
        Reset();
    }

    AdaptiveControlMode AdaptiveStreamingController::GetControlMode() const {
        return controlMode_;
    }

    void AdaptiveStreamingController::SetCongestionControlMode(
        CongestionControlMode mode
    ) {
        if (congestionControlMode_ == mode) {
            return;
        }

        congestionControlMode_ = mode;
        Reset();
    }

    CongestionControlMode
    AdaptiveStreamingController::GetCongestionControlMode() const {
        return congestionControlMode_;
    }

    void AdaptiveStreamingController::Update(
        const AdaptiveStreamingInput& input,
        double deltaTimeSec
    ) {
        state_.qualityChanged = false;
        state_.fpsChanged = false;
        state_.bitrateChanged = false;
        state_.resolutionChanged = false;

        const bool suppressPacingDropForQuality =
            ShouldSuppressPacingDropForQuality(input);
        const double effectiveAckMissingRate =
            EffectiveAckMissingRate(input);
        const double effectivePacketLossRate =
            EffectivePacketLossRate(input);

        state_.lastAckMissingRate = effectiveAckMissingRate;
        state_.lastPacketLossRate = effectivePacketLossRate;
        state_.lastRttMs = input.rttMs;
        state_.lastLatencyMs = input.latencyMs;
        state_.lastJitterMs = input.jitterMs;
        state_.lastReceiveFps = input.receiveFps;
        state_.lastDecodeFps = input.decodeFps;
        state_.lastDisplayFps = input.displayFps;
        state_.lastDeadlineDroppedFrames = input.deadlineDroppedFrames;
        state_.lastOutputQueueDroppedFrames = input.outputQueueDroppedFrames;
        state_.lastDeadlineNackSentFrames = input.deadlineNackSentFrames;
        state_.lastDeadlineNackMissingChunks = input.deadlineNackMissingChunks;
        state_.lastDeadlineNackExpiredDroppedFrames =
            input.deadlineNackExpiredDroppedFrames;
        state_.lastDeadlineNackExpiredAfterNackFrames =
            input.deadlineNackExpiredAfterNackFrames;
        state_.lastAckStaleDroppedFrames = input.ackStaleDroppedFrames;
        state_.lastAckKeyFrameRequests = input.ackKeyFrameRequests;
        state_.lastReceiveFreshnessDroppedFrames =
            input.receiveFreshnessDroppedFrames;
        state_.lastReceiveDecodeInputFrameAgeMs =
            input.receiveDecodeInputFrameAgeMs;
        state_.lastReceiveLatestDecodedFrameAgeMs =
            input.receiveLatestDecodedFrameAgeMs;
        state_.lastReceiveFreshnessDropThresholdMs =
            input.receiveFreshnessDropThresholdMs;
        state_.lastEstimatedBandwidthKbps =
            static_cast<int>(input.estimatedBandwidthBps / 1000u);
        state_.lastDeliveryRateKbps =
            static_cast<int>(input.deliveryRateBps / 1000u);
        state_.lastBandwidthQueueDelayMs = input.bandwidthQueueDelayMs;
        state_.lastBandwidthRttTrendMs = input.bandwidthRttTrendMs;
        state_.lastBandwidthLossTrend = input.bandwidthLossTrend;
        state_.lastBandwidthJitterTrendMs = input.bandwidthJitterTrendMs;
        state_.lastFecEnabled = input.fecEnabled;
        state_.lastAdaptiveFecEnabled = input.adaptiveFecEnabled;
        state_.lastFecGroupChunkCount = input.fecGroupChunkCount;
        state_.lastFecParityPackets = input.fecParityPackets;
        state_.lastFecRecoveredFrames = input.fecRecoveredFrames;
        state_.lastFecRecoveredChunks = input.fecRecoveredChunks;
        const bool adaptiveFecG8ToG4Recovery =
            input.adaptiveFecEnabled &&
            input.fecEnabled &&
            input.fecGroupChunkCount == 4 &&
            hasAdaptiveFecGroupSample_ &&
            lastAdaptiveFecSampleEnabled_ &&
            lastAdaptiveFecSampleGroupChunkCount_ == 8;
        if (adaptiveFecG8ToG4Recovery) {
            postG8ToG4QualityHoldSec_ =
                (std::max)(postG8ToG4QualityHoldSec_, 1.2);
            state_.lastAdaptiveFecG8ToG4Recovery = true;
        }
        else if (postG8ToG4QualityHoldSec_ > 0.0) {
            postG8ToG4QualityHoldSec_ =
                (std::max)(0.0, postG8ToG4QualityHoldSec_ - deltaTimeSec);
        }
        hasAdaptiveFecGroupSample_ = true;
        lastAdaptiveFecSampleEnabled_ =
            input.adaptiveFecEnabled && input.fecEnabled;
        lastAdaptiveFecSampleGroupChunkCount_ = input.fecGroupChunkCount;
        activeBandwidthCeilingKbps_ = CalculateBandwidthCeilingKbps(input);
        state_.bandwidthCeilingKbps = activeBandwidthCeilingKbps_;
        state_.controlMode = controlMode_;
        state_.congestionControlMode = ResolveActiveCongestionControlMode();

        observedTimeSec_ += (std::max)(0.0, deltaTimeSec);

        uint64_t deadlineDropDelta = 0;
        uint64_t outputQueueDropDelta = 0;
        uint64_t deadlineNackDelta = 0;
        uint64_t deadlineNackMissingChunkDelta = 0;
        uint64_t recoveryDeadlineDropDelta = 0;
        uint64_t retransmitStaleDropDelta = 0;
        uint64_t freshnessDropDelta = 0;
        uint64_t receiveDecodeQueueDropDelta = 0;
        uint64_t receiveDecodeRenderOverwriteDelta = 0;
        uint64_t pacingDeadlineDropDelta = 0;
        uint64_t pacingHighPriorityDeadlineDropDelta = 0;

        if (hasDropCounters_) {
            if (input.deadlineDroppedFrames >= lastDeadlineDroppedFrames_) {
                deadlineDropDelta =
                    input.deadlineDroppedFrames - lastDeadlineDroppedFrames_;
            }
            if (input.outputQueueDroppedFrames >= lastOutputQueueDroppedFrames_) {
                outputQueueDropDelta =
                    input.outputQueueDroppedFrames - lastOutputQueueDroppedFrames_;
            }
        }

        hasDropCounters_ = true;
        lastDeadlineDroppedFrames_ = input.deadlineDroppedFrames;
        lastOutputQueueDroppedFrames_ = input.outputQueueDroppedFrames;

        if (hasNackCounters_) {
            if (input.deadlineNackSentFrames >= lastDeadlineNackSentFrames_) {
                deadlineNackDelta =
                    input.deadlineNackSentFrames - lastDeadlineNackSentFrames_;
            }
            if (input.deadlineNackMissingChunks >= lastDeadlineNackMissingChunks_) {
                deadlineNackMissingChunkDelta =
                    input.deadlineNackMissingChunks - lastDeadlineNackMissingChunks_;
            }
            if (input.deadlineNackExpiredDroppedFrames >=
                lastDeadlineNackExpiredDroppedFrames_) {
                recoveryDeadlineDropDelta +=
                    input.deadlineNackExpiredDroppedFrames -
                    lastDeadlineNackExpiredDroppedFrames_;
            }
            if (input.ackStaleDroppedFrames >= lastAckStaleDroppedFrames_) {
                retransmitStaleDropDelta =
                    input.ackStaleDroppedFrames - lastAckStaleDroppedFrames_;
            }
        }

        hasNackCounters_ = true;
        lastDeadlineNackSentFrames_ = input.deadlineNackSentFrames;
        lastDeadlineNackMissingChunks_ = input.deadlineNackMissingChunks;
        lastDeadlineNackExpiredDroppedFrames_ =
            input.deadlineNackExpiredDroppedFrames;
        lastDeadlineNackExpiredAfterNackFrames_ =
            input.deadlineNackExpiredAfterNackFrames;
        lastAckStaleDroppedFrames_ = input.ackStaleDroppedFrames;
        lastAckKeyFrameRequests_ = input.ackKeyFrameRequests;

        if (hasFreshnessCounters_ &&
            input.receiveFreshnessDroppedFrames >=
            lastReceiveFreshnessDroppedFrames_) {
            freshnessDropDelta =
                input.receiveFreshnessDroppedFrames -
                lastReceiveFreshnessDroppedFrames_;
        }

        hasFreshnessCounters_ = true;
        lastReceiveFreshnessDroppedFrames_ =
            input.receiveFreshnessDroppedFrames;

        if (hasReceiveDecodeDropCounters_) {
            if (input.receiveDecodeQueueDroppedFrames >=
                lastReceiveDecodeQueueDroppedFrames_) {
                receiveDecodeQueueDropDelta =
                    input.receiveDecodeQueueDroppedFrames -
                    lastReceiveDecodeQueueDroppedFrames_;
            }
            if (input.receiveDecodeRenderOverwriteFrames >=
                lastReceiveDecodeRenderOverwriteFrames_) {
                receiveDecodeRenderOverwriteDelta =
                    input.receiveDecodeRenderOverwriteFrames -
                    lastReceiveDecodeRenderOverwriteFrames_;
            }
        }

        hasReceiveDecodeDropCounters_ = true;
        lastReceiveDecodeQueueDroppedFrames_ =
            input.receiveDecodeQueueDroppedFrames;
        lastReceiveDecodeRenderOverwriteFrames_ =
            input.receiveDecodeRenderOverwriteFrames;

        if (hasPacingCounters_ &&
            input.pacingDeadlineDroppedPackets >=
            lastPacingDeadlineDroppedPackets_) {
            pacingDeadlineDropDelta =
                input.pacingDeadlineDroppedPackets -
                lastPacingDeadlineDroppedPackets_;
        }
        if (hasPacingCounters_ &&
            input.pacingHighPriorityDeadlineDroppedPackets >=
            lastPacingHighPriorityDeadlineDroppedPackets_) {
            pacingHighPriorityDeadlineDropDelta =
                input.pacingHighPriorityDeadlineDroppedPackets -
                lastPacingHighPriorityDeadlineDroppedPackets_;
        }

        hasPacingCounters_ = true;
        lastPacingDeadlineDroppedPackets_ =
            input.pacingDeadlineDroppedPackets;
        lastPacingHighPriorityDeadlineDroppedPackets_ =
            input.pacingHighPriorityDeadlineDroppedPackets;

        uint64_t repairSentBytesDelta = 0;
        uint64_t repairBorrowedBytesDelta = 0;
        if (hasRepairCounters_) {
            if (input.pacingRepairSentBytes >= lastPacingRepairSentBytes_) {
                repairSentBytesDelta =
                    input.pacingRepairSentBytes -
                    lastPacingRepairSentBytes_;
            }
            if (input.pacingRepairBorrowedBytes >=
                lastPacingRepairBorrowedBytes_) {
                repairBorrowedBytesDelta =
                    input.pacingRepairBorrowedBytes -
                    lastPacingRepairBorrowedBytes_;
            }
        }
        hasRepairCounters_ = true;
        lastPacingRepairSentBytes_ = input.pacingRepairSentBytes;
        lastPacingRepairBorrowedBytes_ = input.pacingRepairBorrowedBytes;

        const double positiveDeltaSec =
            (std::max)(0.0, deltaTimeSec);
        const double repairSentBps =
            positiveDeltaSec > 0.0
            ? static_cast<double>(repairSentBytesDelta) * 8.0 /
                positiveDeltaSec
            : 0.0;
        const double repairBudgetUtilization =
            input.pacingRepairTargetBitrateBps > 0
            ? repairSentBps /
                static_cast<double>(input.pacingRepairTargetBitrateBps)
            : 0.0;
        const double repairBorrowedRatio =
            repairSentBytesDelta > 0
            ? static_cast<double>(repairBorrowedBytesDelta) /
                static_cast<double>(repairSentBytesDelta)
            : 0.0;
        if (repairSentBytesDelta > 0) {
            state_.lastRepairBudgetUtilization =
                std::clamp(repairBudgetUtilization, 0.0, 4.0);
            state_.lastRepairBorrowedRatio =
                std::clamp(repairBorrowedRatio, 0.0, 1.0);
            state_.lastRepairSentBytesDelta = repairSentBytesDelta;
            state_.lastRepairBorrowedBytesDelta = repairBorrowedBytesDelta;
            repairTelemetryHoldSec_ = 1.2;
        }
        else if (repairTelemetryHoldSec_ > 0.0) {
            repairTelemetryHoldSec_ =
                (std::max)(0.0, repairTelemetryHoldSec_ - positiveDeltaSec);
        }
        else {
            state_.lastRepairBudgetUtilization = 0.0;
            state_.lastRepairBorrowedRatio = 0.0;
            state_.lastRepairSentBytesDelta = 0;
            state_.lastRepairBorrowedBytesDelta = 0;
        }

        uint64_t retransmitUsefulDelta = 0;
        uint64_t retransmitDuplicateDelta = 0;
        uint64_t retransmitLateCompletedDelta = 0;
        uint64_t retransmitLateExpiredDelta = 0;
        uint64_t retransmitLateRejectedDelta = 0;
        uint64_t retransmitNotArrivedDelta = 0;
        uint64_t retransmitAccountedDelta = 0;
        uint64_t nackFecGraceSuppressedFrameDelta = 0;
        uint64_t nackFecGraceSuppressedChunkDelta = 0;
        if (hasRetransmitOutcomeCounters_) {
            if (input.retransmitUsefulChunks >= lastRetransmitUsefulChunks_) {
                retransmitUsefulDelta =
                    input.retransmitUsefulChunks - lastRetransmitUsefulChunks_;
            }
            if (input.retransmitDuplicatePackets >=
                lastRetransmitDuplicatePackets_) {
                retransmitDuplicateDelta =
                    input.retransmitDuplicatePackets -
                    lastRetransmitDuplicatePackets_;
            }
            if (input.retransmitLateAfterCompletedPackets >=
                lastRetransmitLateAfterCompletedPackets_) {
                retransmitLateCompletedDelta =
                    input.retransmitLateAfterCompletedPackets -
                    lastRetransmitLateAfterCompletedPackets_;
            }
            if (input.retransmitLateAfterExpiredPackets >=
                lastRetransmitLateAfterExpiredPackets_) {
                retransmitLateExpiredDelta =
                    input.retransmitLateAfterExpiredPackets -
                    lastRetransmitLateAfterExpiredPackets_;
            }
            if (input.retransmitLateAfterRejectedPackets >=
                lastRetransmitLateAfterRejectedPackets_) {
                retransmitLateRejectedDelta =
                    input.retransmitLateAfterRejectedPackets -
                    lastRetransmitLateAfterRejectedPackets_;
            }
            if (input.retransmitNotArrivedPackets >=
                lastRetransmitNotArrivedPackets_) {
                retransmitNotArrivedDelta =
                    input.retransmitNotArrivedPackets -
                    lastRetransmitNotArrivedPackets_;
            }
            if (input.retransmitAccountedPackets >=
                lastRetransmitAccountedPackets_) {
                retransmitAccountedDelta =
                    input.retransmitAccountedPackets -
                    lastRetransmitAccountedPackets_;
            }
            if (input.nackFecGraceSuppressedFrames >=
                lastNackFecGraceSuppressedFrames_) {
                nackFecGraceSuppressedFrameDelta =
                    input.nackFecGraceSuppressedFrames -
                    lastNackFecGraceSuppressedFrames_;
            }
            if (input.nackFecGraceSuppressedChunks >=
                lastNackFecGraceSuppressedChunks_) {
                nackFecGraceSuppressedChunkDelta =
                    input.nackFecGraceSuppressedChunks -
                    lastNackFecGraceSuppressedChunks_;
            }
        }
        hasRetransmitOutcomeCounters_ = true;
        lastRetransmitUsefulChunks_ = input.retransmitUsefulChunks;
        lastRetransmitDuplicatePackets_ = input.retransmitDuplicatePackets;
        lastRetransmitLateAfterCompletedPackets_ =
            input.retransmitLateAfterCompletedPackets;
        lastRetransmitLateAfterExpiredPackets_ =
            input.retransmitLateAfterExpiredPackets;
        lastRetransmitLateAfterRejectedPackets_ =
            input.retransmitLateAfterRejectedPackets;
        lastRetransmitNotArrivedPackets_ = input.retransmitNotArrivedPackets;
        lastRetransmitAccountedPackets_ = input.retransmitAccountedPackets;
        lastNackFecGraceSuppressedFrames_ =
            input.nackFecGraceSuppressedFrames;
        lastNackFecGraceSuppressedChunks_ =
            input.nackFecGraceSuppressedChunks;

        const uint64_t retransmitLateWasteDelta =
            retransmitDuplicateDelta +
            retransmitLateCompletedDelta +
            retransmitLateExpiredDelta +
            retransmitLateRejectedDelta;
        const uint64_t retransmitObservedDelta =
            retransmitUsefulDelta +
            retransmitLateWasteDelta +
            retransmitNotArrivedDelta;
        const uint64_t retransmitCumulativeLateWaste =
            input.retransmitDuplicatePackets +
            input.retransmitLateAfterCompletedPackets +
            input.retransmitLateAfterExpiredPackets +
            input.retransmitLateAfterRejectedPackets;
        const uint64_t retransmitCumulativeObserved =
            input.retransmitUsefulChunks +
            retransmitCumulativeLateWaste +
            input.retransmitNotArrivedPackets;
        const double retransmitUsefulRatio =
            retransmitObservedDelta > 0
            ? static_cast<double>(retransmitUsefulDelta) /
                static_cast<double>(retransmitObservedDelta)
            : 0.0;
        const double lateRepairWasteRatio =
            retransmitObservedDelta > 0
            ? static_cast<double>(retransmitLateWasteDelta) /
                static_cast<double>(retransmitObservedDelta)
            : 0.0;
        const double retransmitNotArrivedRatio =
            retransmitObservedDelta > 0
            ? static_cast<double>(retransmitNotArrivedDelta) /
                static_cast<double>(retransmitObservedDelta)
            : 0.0;
        const double cumulativeRetransmitUsefulRatio =
            retransmitCumulativeObserved > 0
            ? static_cast<double>(input.retransmitUsefulChunks) /
                static_cast<double>(retransmitCumulativeObserved)
            : 0.0;
        const double cumulativeLateRepairWasteRatio =
            retransmitCumulativeObserved > 0
            ? static_cast<double>(retransmitCumulativeLateWaste) /
                static_cast<double>(retransmitCumulativeObserved)
            : 0.0;
        const double cumulativeRetransmitNotArrivedRatio =
            retransmitCumulativeObserved > 0
            ? static_cast<double>(input.retransmitNotArrivedPackets) /
                static_cast<double>(retransmitCumulativeObserved)
            : 0.0;
        const double repairUsefulRatioForDecision =
            retransmitObservedDelta > 0
            ? retransmitUsefulRatio
            : cumulativeRetransmitUsefulRatio;
        const double lateWasteRatioForDecision =
            retransmitObservedDelta > 0
            ? lateRepairWasteRatio
            : cumulativeLateRepairWasteRatio;
        const double notArrivedRatioForDecision =
            retransmitObservedDelta > 0
            ? retransmitNotArrivedRatio
            : cumulativeRetransmitNotArrivedRatio;
        const uint64_t lateWasteCountForDecision =
            retransmitLateWasteDelta > 0
            ? retransmitLateWasteDelta
            : retransmitCumulativeLateWaste;
        const uint64_t notArrivedCountForDecision =
            retransmitNotArrivedDelta > 0
            ? retransmitNotArrivedDelta
            : input.retransmitNotArrivedPackets;
        const bool retransmitAccountingComplete =
            input.retransmitFinalAccountingRatio >= 0.999 ||
            (retransmitObservedDelta > 0 &&
                retransmitAccountedDelta >= retransmitObservedDelta);
        if (retransmitObservedDelta > 0 ||
            retransmitCumulativeObserved > 0) {
            state_.lastRetransmitUsefulRatio =
                std::clamp(repairUsefulRatioForDecision, 0.0, 1.0);
            state_.lastLateRepairWasteRatio =
                std::clamp(lateWasteRatioForDecision, 0.0, 1.0);
            state_.lastRetransmitNotArrivedRatio =
                std::clamp(notArrivedRatioForDecision, 0.0, 1.0);
        }
        else {
            state_.lastRetransmitUsefulRatio = 0.0;
            state_.lastLateRepairWasteRatio = 0.0;
            state_.lastRetransmitNotArrivedRatio = 0.0;
        }
        state_.lastRetransmitAccountingComplete =
            retransmitAccountingComplete;
        state_.lastLateRepairWastePressure = false;
        state_.lastRetransmitNotArrivedPressure = false;
        state_.lastRepairDecisionReason.clear();

        uint64_t fecParityPacketDelta = 0;
        uint64_t fecRecoveredFrameDelta = 0;
        uint64_t fecRecoveredChunkDelta = 0;
        if (hasFecCounters_) {
            if (input.fecParityPackets >= lastFecParityPackets_) {
                fecParityPacketDelta =
                    input.fecParityPackets - lastFecParityPackets_;
            }
            if (input.fecRecoveredFrames >= lastFecRecoveredFrames_) {
                fecRecoveredFrameDelta =
                    input.fecRecoveredFrames - lastFecRecoveredFrames_;
            }
            if (input.fecRecoveredChunks >= lastFecRecoveredChunks_) {
                fecRecoveredChunkDelta =
                    input.fecRecoveredChunks - lastFecRecoveredChunks_;
            }
        }

        hasFecCounters_ = true;
        lastFecParityPackets_ = input.fecParityPackets;
        lastFecRecoveredFrames_ = input.fecRecoveredFrames;
        lastFecRecoveredChunks_ = input.fecRecoveredChunks;

        const uint64_t fecRecoveredUnits =
            (std::max)(fecRecoveredFrameDelta, fecRecoveredChunkDelta);
        const double fecRecoveryEfficiency =
            fecParityPacketDelta > 0
            ? static_cast<double>(fecRecoveredUnits) /
                static_cast<double>(fecParityPacketDelta)
            : (fecRecoveredUnits > 0 ? 1.0 : 0.0);
        const bool fecRecoveryWorkingNow =
            fecRecoveredUnits > 0 &&
            (fecParityPacketDelta == 0 || fecRecoveryEfficiency >= 0.25);
        if (fecRecoveryWorkingNow) {
            fecRecoveryGuardSec_ = (std::max)(fecRecoveryGuardSec_, 2.5);
            fecRecoveryEvidenceSec_ =
                (std::max)(fecRecoveryEvidenceSec_, 3.5);
            lastEffectiveFecRecoveryEfficiency_ =
                (std::max)(
                    lastEffectiveFecRecoveryEfficiency_,
                    fecRecoveryEfficiency);
        }
        else {
            if (fecRecoveryGuardSec_ > 0.0) {
                fecRecoveryGuardSec_ =
                    (std::max)(0.0, fecRecoveryGuardSec_ - deltaTimeSec);
            }
            if (fecRecoveryEvidenceSec_ > 0.0) {
                fecRecoveryEvidenceSec_ =
                    (std::max)(0.0, fecRecoveryEvidenceSec_ - deltaTimeSec);
                if (fecRecoveryEvidenceSec_ <= 0.0) {
                    lastEffectiveFecRecoveryEfficiency_ = 0.0;
                }
            }
        }
        if (fecRecoveryWorkingNow) {
            state_.lastFecParityPacketDelta = fecParityPacketDelta;
            state_.lastFecRecoveredFrameDelta = fecRecoveredFrameDelta;
            state_.lastFecRecoveredChunkDelta = fecRecoveredChunkDelta;
        }
        else if (fecRecoveryGuardSec_ <= 0.0) {
            state_.lastFecParityPacketDelta = 0;
            state_.lastFecRecoveredFrameDelta = 0;
            state_.lastFecRecoveredChunkDelta = 0;
        }
        state_.lastFecRecoveryEfficiency = fecRecoveryEfficiency;
        const bool fecRecoveryGuardTimerActive = fecRecoveryGuardSec_ > 0.0;
        const bool fecRecoveryEvidenceActive = fecRecoveryEvidenceSec_ > 0.0;
        state_.lastFecRecoveryGuardActive = false;
        state_.lastFecRecoveryWorking =
            fecRecoveryWorkingNow || fecRecoveryGuardTimerActive;

        const uint64_t qualityDeadlineDropDelta =
            suppressPacingDropForQuality ? 0 : deadlineDropDelta;
        const uint64_t qualityDeadlineNackDelta =
            suppressPacingDropForQuality ? 0 : deadlineNackDelta;
        const uint64_t qualityDeadlineNackMissingChunkDelta =
            suppressPacingDropForQuality ? 0 : deadlineNackMissingChunkDelta;

        const double rawQoeScore =
            CalculateQoeScore(
                input,
                qualityDeadlineDropDelta,
                outputQueueDropDelta,
                recoveryDeadlineDropDelta,
                retransmitStaleDropDelta,
                freshnessDropDelta);
        const AdaptiveDegradationCause qualityDegradationCause =
            DetermineDegradationCause(
                input,
                qualityDeadlineDropDelta,
                outputQueueDropDelta,
                qualityDeadlineNackDelta,
                qualityDeadlineNackMissingChunkDelta,
                recoveryDeadlineDropDelta,
                retransmitStaleDropDelta,
                freshnessDropDelta);
        const AdaptiveDegradationCause rawDegradationCause =
            qualityDegradationCause == AdaptiveDegradationCause::None &&
            HasPacingDropPressure(input, pacingDeadlineDropDelta)
            ? AdaptiveDegradationCause::PacingQueue
            : qualityDegradationCause;

        if (rawQoeScore > 0.0) {
            heldQoeScore_ = rawQoeScore;
            qoeHoldSec_ = 1.25;
        }
        else if (qoeHoldSec_ > 0.0) {
            qoeHoldSec_ = (std::max)(0.0, qoeHoldSec_ - deltaTimeSec);
            if (qoeHoldSec_ <= 0.0) {
                heldQoeScore_ = 0.0;
            }
        }
        else {
            heldQoeScore_ = 0.0;
        }

        if (rawDegradationCause != AdaptiveDegradationCause::None) {
            heldDegradationCause_ = rawDegradationCause;
            degradationCauseHoldSec_ = 1.25;
        }
        else if (degradationCauseHoldSec_ > 0.0) {
            degradationCauseHoldSec_ =
                (std::max)(0.0, degradationCauseHoldSec_ - deltaTimeSec);
            if (degradationCauseHoldSec_ <= 0.0) {
                heldDegradationCause_ = AdaptiveDegradationCause::None;
            }
        }
        else {
            heldDegradationCause_ = AdaptiveDegradationCause::None;
        }

        const double qoeScore = (std::max)(rawQoeScore, heldQoeScore_);
        state_.lastQoeScore = qoeScore;
        state_.lastDegradationCause = heldDegradationCause_;

        if (!enabled_ ||
            controlMode_ == AdaptiveControlMode::FixedQuality) {
            const bool fixedPacingQueuePressure =
                enabled_ &&
                controlMode_ == AdaptiveControlMode::FixedQuality &&
                input.pacingEnabled &&
                (input.pacingCurrentQueueDelayMs >= 35.0 ||
                    (input.pacingMaxQueueDelayMs >= 90.0 &&
                        input.pacingCurrentQueueDelayMs >= 18.0) ||
                    input.latencyMs >= 80.0);
            if (fixedPacingQueuePressure) {
                const bool hardFixedPacingQueuePressure =
                    input.pacingCurrentQueueDelayMs >= 70.0 ||
                    (input.pacingMaxQueueDelayMs >= 110.0 &&
                        input.pacingCurrentQueueDelayMs >= 30.0) ||
                    input.latencyMs >= 90.0;
                pacingBurstPressureSec_ += positiveDeltaSec;
                pacingBurstGuardSec_ =
                    (std::max)(
                        pacingBurstGuardSec_,
                        hardFixedPacingQueuePressure ? 1.25 : 0.80);
                pacingBurstVideoBudgetScale_ =
                    (std::min)(
                        pacingBurstVideoBudgetScale_,
                        hardFixedPacingQueuePressure ? 0.68 : 0.82);
                state_.h264VideoBudgetScale =
                    std::clamp(pacingBurstVideoBudgetScale_, 0.55, 1.0);
                state_.lastPacingBurstGuardActive = true;
                state_.lastRepairBudgetGuardActive = true;
                state_.lastRepairVideoBudgetPressure = false;
                state_.lastLateRepairWastePressure = false;
                state_.lastRetransmitNotArrivedPressure = false;
                state_.lastRepairDecisionReason =
                    hardFixedPacingQueuePressure
                    ? "fixed-pacing-queue-hard-pressure"
                    : "fixed-pacing-queue-pressure";
                stableTimeSec_ = 0.0;
                badTimeSec_ = 0.0;
                lossOnlyBadTimeSec_ = 0.0;
                return;
            }

            stableTimeSec_ = 0.0;
            badTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
            pacingBurstPressureSec_ =
                (std::max)(0.0, pacingBurstPressureSec_ - positiveDeltaSec);
            const bool fixedReceiveQueuePressure =
                outputQueueDropDelta > 0 ||
                freshnessDropDelta > 0 ||
                receiveDecodeQueueDropDelta > 0 ||
                receiveDecodeRenderOverwriteDelta > 0;
            if (pacingBurstGuardSec_ > 0.0) {
                pacingBurstGuardSec_ =
                    (std::max)(0.0, pacingBurstGuardSec_ - positiveDeltaSec);
            }
            else {
                const double fixedRecoveryRate =
                    fixedReceiveQueuePressure ? 0.10 : 0.25;
                pacingBurstVideoBudgetScale_ =
                    (std::min)(
                        1.0,
                        pacingBurstVideoBudgetScale_ +
                            positiveDeltaSec * fixedRecoveryRate);
            }
            repairBudgetGuardSec_ = 0.0;
            repairBorrowPressureSec_ = 0.0;
            retransmitNotArrivedPressureSec_ = 0.0;
            lateRepairWasteGuardSec_ = 0.0;
            repairTelemetryHoldSec_ = 0.0;
            state_.h264VideoBudgetScale =
                std::clamp(pacingBurstVideoBudgetScale_, 0.55, 1.0);
            state_.lastPacingBurstGuardActive =
                pacingBurstGuardSec_ > 0.0 || pacingBurstVideoBudgetScale_ < 0.999;
            state_.lastRepairBudgetGuardActive = false;
            state_.lastRepairVideoBudgetPressure = false;
            state_.lastLateRepairWastePressure = false;
            state_.lastRetransmitNotArrivedPressure = false;
            state_.lastRepairDecisionReason =
                state_.lastPacingBurstGuardActive
                ? (
                    fixedReceiveQueuePressure
                    ? "fixed-pacing-receive-queue-hold"
                    : "fixed-pacing-queue-hold")
                : "disabled";
            return;
        }

        if (controlMode_ == AdaptiveControlMode::LossReactive) {
            pacingBurstGuardSec_ = 0.0;
            pacingBurstPressureSec_ = 0.0;
            pacingBurstVideoBudgetScale_ = 1.0;
            repairBudgetGuardSec_ = 0.0;
            repairBorrowPressureSec_ = 0.0;
            retransmitNotArrivedPressureSec_ = 0.0;
            lateRepairWasteGuardSec_ = 0.0;
            repairTelemetryHoldSec_ = 0.0;
            state_.h264VideoBudgetScale = 1.0;
            state_.lastPacingBurstGuardActive = false;
            state_.lastRepairBudgetGuardActive = false;
            state_.lastRepairVideoBudgetPressure = false;
            state_.lastLateRepairWastePressure = false;
            state_.lastRetransmitNotArrivedPressure = false;
            state_.lastRepairDecisionReason = "loss-reactive";
            UpdateLossReactive(
                input,
                deltaTimeSec,
                qualityDeadlineNackDelta,
                qualityDeadlineNackMissingChunkDelta);
            return;
        }

        const bool repairActive =
            input.pacingEnabled &&
            input.pacingRepairTargetBitrateBps > 0 &&
            repairSentBytesDelta > 0;
        const bool retransmitOutcomeActive =
            (retransmitObservedDelta >= 8 ||
                retransmitCumulativeObserved >= 32) &&
            retransmitAccountingComplete;
        const bool retransmitNotArrivedPressure =
            retransmitOutcomeActive &&
            notArrivedCountForDecision >= 8 &&
            notArrivedRatioForDecision >= 0.18;
        const bool hardRetransmitNotArrivedPressure =
            retransmitNotArrivedPressure &&
            (notArrivedRatioForDecision >= 0.32 ||
                notArrivedCountForDecision >= 32);
        const bool lateRepairWastePressure =
            retransmitOutcomeActive &&
            !retransmitNotArrivedPressure &&
            lateWasteCountForDecision >= 8 &&
            lateWasteRatioForDecision >= 0.35 &&
            repairUsefulRatioForDecision < 0.65;
        const bool retransmitUsefulStable =
            retransmitOutcomeActive &&
            repairUsefulRatioForDecision >= 0.50 &&
            notArrivedRatioForDecision <= 0.12 &&
            pacingHighPriorityDeadlineDropDelta == 0 &&
            pacingDeadlineDropDelta == 0;
        const bool repairBudgetHealthy =
            (repairActive || retransmitUsefulStable) &&
            state_.lastRepairBorrowedRatio <= 0.15 &&
            state_.lastRepairBudgetUtilization <= 1.25 &&
            pacingHighPriorityDeadlineDropDelta == 0 &&
            pacingDeadlineDropDelta == 0;
        const bool repairVideoBudgetPressure =
            (repairActive &&
                (state_.lastRepairBorrowedRatio >= 0.20 ||
                repairBorrowedBytesDelta >= 32768 ||
                pacingHighPriorityDeadlineDropDelta > 0 ||
                (state_.lastRepairBudgetUtilization >= 1.15 &&
                    repairBorrowedBytesDelta > 0))) ||
            retransmitNotArrivedPressure;

        if (repairBudgetHealthy) {
            repairBudgetGuardSec_ =
                (std::max)(repairBudgetGuardSec_, 1.0);
            if (state_.lastRepairDecisionReason.empty()) {
                state_.lastRepairDecisionReason =
                    retransmitUsefulStable
                    ? "retransmit-useful-stable"
                    : "repair-budget-healthy";
            }
        }

        if (lateRepairWastePressure) {
            lateRepairWasteGuardSec_ =
                (std::max)(lateRepairWasteGuardSec_, 1.25);
            state_.lastLateRepairWastePressure = true;
            state_.lastRepairBudgetGuardActive = true;
            state_.lastRepairDecisionReason =
                nackFecGraceSuppressedChunkDelta > 0 ||
                nackFecGraceSuppressedFrameDelta > 0
                ? "late-repair-waste-fec-grace"
                : "late-repair-waste-guard";
            pacingBurstVideoBudgetScale_ =
                (std::min)(1.0, pacingBurstVideoBudgetScale_ + 0.02);
        }

        if (repairVideoBudgetPressure) {
            const bool hardRepairPressure =
                hardRetransmitNotArrivedPressure ||
                state_.lastRepairBorrowedRatio >= 0.45 ||
                repairBorrowedBytesDelta >= 96u * 1024u ||
                pacingHighPriorityDeadlineDropDelta > 0;

            repairBorrowPressureSec_ += positiveDeltaSec;
            if (retransmitNotArrivedPressure) {
                retransmitNotArrivedPressureSec_ += positiveDeltaSec;
            }
            repairBudgetGuardSec_ = 0.0;
            lateRepairWasteGuardSec_ = 0.0;
            pacingBurstVideoBudgetScale_ =
                (std::min)(
                    pacingBurstVideoBudgetScale_,
                    hardRepairPressure ? 0.80 : 0.90);
            state_.h264VideoBudgetScale =
                std::clamp(pacingBurstVideoBudgetScale_, 0.60, 1.0);
            state_.lastPacingBurstGuardActive = true;
            state_.lastRepairBudgetGuardActive = false;
            state_.lastRepairVideoBudgetPressure = true;
            state_.lastRetransmitNotArrivedPressure =
                retransmitNotArrivedPressure;
            state_.lastRepairDecisionReason =
                retransmitNotArrivedPressure
                ? "retransmit-not-arrived-video-budget-pressure"
                : "repair-budget-borrowed-video-budget-pressure";

            const bool persistentRepairPressure =
                (hardRepairPressure && repairBorrowPressureSec_ >= 1.0) ||
                repairBorrowPressureSec_ >= 1.8 ||
                retransmitNotArrivedPressureSec_ >= 1.2;
            if (persistentRepairPressure) {
                ApplyAimdBitrateOnlyDecrease(
                    input,
                    AdaptiveDegradationCause::PacingQueue,
                    hardRepairPressure);
                repairBorrowPressureSec_ = 0.0;
                retransmitNotArrivedPressureSec_ = 0.0;
                cooldownSec_ = hardRepairPressure ? 0.80 : 0.60;
            }

            badTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
            stableTimeSec_ = 0.0;
            return;
        }

        repairBorrowPressureSec_ =
            (std::max)(0.0, repairBorrowPressureSec_ - positiveDeltaSec);
        retransmitNotArrivedPressureSec_ =
            (std::max)(
                0.0,
                retransmitNotArrivedPressureSec_ - positiveDeltaSec);
        if (repairBudgetGuardSec_ > 0.0) {
            repairBudgetGuardSec_ =
                (std::max)(0.0, repairBudgetGuardSec_ - positiveDeltaSec);
        }
        if (lateRepairWasteGuardSec_ > 0.0) {
            lateRepairWasteGuardSec_ =
                (std::max)(0.0, lateRepairWasteGuardSec_ - positiveDeltaSec);
        }
        state_.lastRepairBudgetGuardActive =
            repairBudgetGuardSec_ > 0.0 || lateRepairWasteGuardSec_ > 0.0;
        state_.lastRepairVideoBudgetPressure = false;
        state_.lastLateRepairWastePressure =
            state_.lastLateRepairWastePressure ||
            lateRepairWasteGuardSec_ > 0.0;
        state_.lastRetransmitNotArrivedPressure = false;
        if (state_.lastRepairDecisionReason.empty() &&
            lateRepairWasteGuardSec_ > 0.0) {
            state_.lastRepairDecisionReason = "late-repair-waste-hold";
        }

        const bool pacingBurstPressure =
            HasPacingDropPressure(input, pacingDeadlineDropDelta);
        if (pacingBurstPressure &&
            (pacingDeadlineDropDelta >= 8 ||
                input.pacingCurrentQueueDelayMs >= 15.0 ||
                input.pacingMaxQueueDelayMs >= 60.0)) {
            const bool hardPacingBurst =
                pacingDeadlineDropDelta >= 32 ||
                input.pacingCurrentQueueDelayMs >= 30.0 ||
                input.pacingMaxQueueDelayMs >= 100.0;

            pacingBurstPressureSec_ += positiveDeltaSec;
            pacingBurstGuardSec_ =
                (std::max)(pacingBurstGuardSec_, hardPacingBurst ? 1.20 : 0.75);
            pacingBurstVideoBudgetScale_ =
                (std::min)(
                    pacingBurstVideoBudgetScale_,
                    hardPacingBurst ? 0.72 : 0.84);
            state_.h264VideoBudgetScale =
                std::clamp(pacingBurstVideoBudgetScale_, 0.55, 1.0);
            state_.lastPacingBurstGuardActive = true;
            if (state_.lastRepairDecisionReason.empty()) {
                state_.lastRepairDecisionReason = "pacing-burst-pressure";
            }

            const bool persistentPacingPressure =
                (hardPacingBurst && pacingBurstPressureSec_ >= 1.0) ||
                pacingBurstPressureSec_ >= 1.6;
            if (persistentPacingPressure) {
                ApplyAimdBitrateOnlyDecrease(
                    input,
                    AdaptiveDegradationCause::PacingQueue,
                    hardPacingBurst);
                pacingBurstPressureSec_ = 0.0;
                cooldownSec_ = hardPacingBurst ? 0.75 : 0.55;
            }
            badTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
            stableTimeSec_ = 0.0;
            return;
        }

        pacingBurstPressureSec_ =
            (std::max)(0.0, pacingBurstPressureSec_ - positiveDeltaSec * 0.75);
        if (pacingBurstGuardSec_ > 0.0) {
            pacingBurstGuardSec_ =
                (std::max)(0.0, pacingBurstGuardSec_ - positiveDeltaSec);
        }
        else {
            pacingBurstVideoBudgetScale_ =
                (std::min)(
                    1.0,
                    pacingBurstVideoBudgetScale_ + positiveDeltaSec * 0.20);
        }
        state_.h264VideoBudgetScale =
            std::clamp(pacingBurstVideoBudgetScale_, 0.55, 1.0);
        state_.lastPacingBurstGuardActive =
            pacingBurstGuardSec_ > 0.0 || pacingBurstVideoBudgetScale_ < 0.999;
        if (state_.lastRepairDecisionReason.empty()) {
            state_.lastRepairDecisionReason = "none";
        }

        if (cooldownSec_ > 0.0) {
            cooldownSec_ = (std::max)(0.0, cooldownSec_ - deltaTimeSec);
            return;
        }

        const bool displayHealthy =
            input.displayedFrames < 10 ||
            input.displayFps <= 0.0 ||
            input.displayFps >=
            static_cast<double>(state_.targetFps) * 0.85;
        const bool fecGuardDisplayHealthy =
            input.displayedFrames < 10 ||
            input.displayFps <= 0.0 ||
            input.displayFps >=
            static_cast<double>(state_.targetFps) * 0.70;
        const bool fecRecoveryCoversDeadline =
            recoveryDeadlineDropDelta == 0 ||
            (fecRecoveredFrameDelta >= recoveryDeadlineDropDelta &&
                recoveryDeadlineDropDelta <= 5);
        const bool recoveryDeadlineMiss =
            recoveryDeadlineDropDelta > 0 ||
            retransmitStaleDropDelta > 0;
        if (recoveryDeadlineDropDelta > 0) {
            nackExpiredRisingSamples_++;
            postNackExpiredGuardRearmSec_ = 0.0;
            const uint32_t bitrateOnlyBudget =
                recoveryDeadlineDropDelta >= 4 ||
                nackExpiredRisingSamples_ >= 2
                ? 1u
                : 2u;
            nackExpiredBitrateOnlyDecreaseBudget_ =
                (std::min)(
                    nackExpiredBitrateOnlyDecreaseBudget_,
                    bitrateOnlyBudget);
            const double releaseSec =
                recoveryDeadlineDropDelta >= 4 ||
                nackExpiredRisingSamples_ >= 2
                ? 2.0
                : 1.25;
            nackExpiredGuardReleaseSec_ =
                (std::max)(nackExpiredGuardReleaseSec_, releaseSec);
        }
        else if (nackExpiredGuardReleaseSec_ > 0.0) {
            nackExpiredGuardReleaseSec_ =
                (std::max)(0.0, nackExpiredGuardReleaseSec_ - deltaTimeSec);
            if (nackExpiredGuardReleaseSec_ <= 0.0) {
                nackExpiredRisingSamples_ = 0;
                nackExpiredBitrateOnlyDecreaseSamples_ = 0;
                nackExpiredBitrateOnlyDecreaseBudget_ = 2;
                const bool efficientRecentFecRecovery =
                    fecRecoveryEvidenceActive &&
                    lastEffectiveFecRecoveryEfficiency_ >= 0.30;
                if (input.adaptiveFecEnabled &&
                    efficientRecentFecRecovery) {
                    postNackExpiredGuardRearmSec_ =
                        (std::max)(postNackExpiredGuardRearmSec_, 0.6);
                }
            }
        }
        else if (postNackExpiredGuardRearmSec_ > 0.0) {
            postNackExpiredGuardRearmSec_ =
                (std::max)(
                    0.0,
                    postNackExpiredGuardRearmSec_ - deltaTimeSec);
        }
        else {
            nackExpiredRisingSamples_ = 0;
            nackExpiredBitrateOnlyDecreaseSamples_ = 0;
            nackExpiredBitrateOnlyDecreaseBudget_ = 2;
        }
        const bool nackExpiredStillRising =
            nackExpiredRisingSamples_ >= 2;
        const bool nackExpiredBurst =
            recoveryDeadlineDropDelta >= 4;
        if (postG8ToG4QualityHoldSec_ > 0.0 &&
            recoveryDeadlineDropDelta >= 5) {
            postG8ToG4QualityHoldSec_ = 0.0;
            state_.lastAdaptiveFecQualityHoldCanceled = true;
        }
        const bool nackExpiredGuardReleaseActive =
            nackExpiredGuardReleaseSec_ > 0.0;
        const bool postNackExpiredGuardRearmActive =
            postNackExpiredGuardRearmSec_ > 0.0;
        const bool fecGuardDeadlineHealthy =
            !nackExpiredGuardReleaseActive &&
            !nackExpiredStillRising &&
            !nackExpiredBurst;
        if (recoveryDeadlineMiss && !fecRecoveryCoversDeadline) {
            recoveryDeadlineFallbackSamples_++;
        }
        else if (!recoveryDeadlineMiss || fecRecoveryCoversDeadline) {
            recoveryDeadlineFallbackSamples_ = 0;
        }
        const bool recoveryFallbackPending =
            input.adaptiveFecEnabled &&
            recoveryDeadlineMiss &&
            !fecRecoveryCoversDeadline &&
            fecGuardDeadlineHealthy &&
            recoveryDeadlineFallbackSamples_ < 2 &&
            qualityDeadlineDropDelta == 0 &&
            outputQueueDropDelta == 0 &&
            freshnessDropDelta == 0;
        const bool fecQualityGuard =
            (state_.lastFecRecoveryWorking ||
                postNackExpiredGuardRearmActive) &&
            fecRecoveryCoversDeadline &&
            fecGuardDeadlineHealthy &&
            qualityDeadlineDropDelta == 0 &&
            outputQueueDropDelta == 0 &&
            retransmitStaleDropDelta == 0 &&
            freshnessDropDelta == 0 &&
            input.latencyMs < 90.0 &&
            fecGuardDisplayHealthy;
        const bool fecBurstTailRecovery =
            input.adaptiveFecEnabled &&
            input.fecEnabled &&
            fecRecoveryEvidenceActive &&
            lastEffectiveFecRecoveryEfficiency_ >= 0.30 &&
            recoveryDeadlineDropDelta == 0 &&
            retransmitStaleDropDelta == 0 &&
            qualityDeadlineDropDelta == 0 &&
            outputQueueDropDelta == 0 &&
            freshnessDropDelta == 0 &&
            input.latencyMs < 60.0 &&
            displayHealthy &&
            (nackExpiredRisingSamples_ > 0 ||
                nackExpiredGuardReleaseActive ||
                postNackExpiredGuardRearmActive);
        state_.lastFecRecoveryGuardActive = fecQualityGuard;
        const bool postG8ToG4QualityHoldActive =
            postG8ToG4QualityHoldSec_ > 0.0 &&
            input.adaptiveFecEnabled &&
            input.fecEnabled &&
            input.fecGroupChunkCount == 4;
        state_.lastAdaptiveFecQualityHoldActive =
            postG8ToG4QualityHoldActive;
        const bool repairQualityGuard =
            state_.lastRepairBudgetGuardActive &&
            qualityDeadlineDropDelta == 0 &&
            outputQueueDropDelta == 0 &&
            retransmitStaleDropDelta == 0 &&
            freshnessDropDelta == 0 &&
            input.pacingCurrentQueueDelayMs < 30.0 &&
            input.pacingMaxQueueDelayMs < 100.0 &&
            displayHealthy;
        state_.lastRepairBudgetGuardActive = repairQualityGuard;
        const bool smoothGuardReleaseDecrease =
            postG8ToG4QualityHoldActive ||
            (nackExpiredGuardReleaseActive &&
                input.adaptiveFecEnabled &&
                nackExpiredBitrateOnlyDecreaseSamples_ <
                    nackExpiredBitrateOnlyDecreaseBudget_);

        const bool hardQoeProblem =
            qoeScore >= 2.0 &&
            !recoveryFallbackPending &&
            !(fecQualityGuard && qoeScore < 3.0) &&
            !(fecBurstTailRecovery && qoeScore < 3.0) &&
            !(repairQualityGuard && qoeScore < 3.0);
        const bool moderateQoeProblem =
            qoeScore >= 1.0 &&
            !recoveryFallbackPending &&
            !fecQualityGuard &&
            !fecBurstTailRecovery &&
            !repairQualityGuard;
        const bool congestionPressure =
            HasCongestionPressure(
                input,
                qualityDeadlineNackDelta,
                qualityDeadlineNackMissingChunkDelta);
        const bool bandwidthPressure = HasBandwidthPressure(input);
        const bool guardedCongestionPressure =
            congestionPressure &&
            !recoveryFallbackPending &&
            !fecQualityGuard &&
            !fecBurstTailRecovery &&
            !repairQualityGuard;
        const bool guardedBandwidthPressure =
            bandwidthPressure &&
            !recoveryFallbackPending &&
            !fecQualityGuard &&
            !fecBurstTailRecovery &&
            !repairQualityGuard;
        const bool lossOnlyPressure =
            !moderateQoeProblem &&
            guardedCongestionPressure;

        const bool stableNetwork =
            qoeScore <= 0.25 &&
            effectiveAckMissingRate <= 0.02 &&
            effectivePacketLossRate <= 0.02 &&
            input.rttMs <= 100.0 &&
            input.latencyMs <= 100.0 &&
            displayHealthy &&
            IsCongestionRecoveryAllowed(
                input,
                qualityDeadlineNackDelta,
                qualityDeadlineNackMissingChunkDelta);

        if (moderateQoeProblem) {
            badTimeSec_ += deltaTimeSec;
            lossOnlyBadTimeSec_ = 0.0;
            stableTimeSec_ = 0.0;
        }
        else if (lossOnlyPressure) {
            lossOnlyBadTimeSec_ += deltaTimeSec;
            badTimeSec_ = 0.0;
            stableTimeSec_ = 0.0;
        }
        else if (stableNetwork) {
            stableTimeSec_ += deltaTimeSec;
            fecBurstTailRecoverySec_ = 0.0;
            badTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
        }
        else if (fecBurstTailRecovery) {
            fecBurstTailRecoverySec_ += deltaTimeSec;
            badTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
            stableTimeSec_ = 0.0;
        }
        else {
            badTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
            stableTimeSec_ = 0.0;
            fecBurstTailRecoverySec_ = 0.0;
        }

        if (hardQoeProblem && badTimeSec_ >= 0.6) {
            if (smoothGuardReleaseDecrease) {
                ApplyAimdBitrateOnlyDecrease(
                    input,
                    state_.lastDegradationCause,
                    true);
                nackExpiredBitrateOnlyDecreaseSamples_++;
            }
            else {
                ApplyAimdDecrease(input, state_.lastDegradationCause, true);
            }
            badTimeSec_ = 0.0;
            cooldownSec_ = smoothGuardReleaseDecrease ? 0.9 : 1.2;
        }
        else if (moderateQoeProblem && badTimeSec_ >= 1.8) {
            if (smoothGuardReleaseDecrease) {
                ApplyAimdBitrateOnlyDecrease(
                    input,
                    state_.lastDegradationCause,
                    false);
                nackExpiredBitrateOnlyDecreaseSamples_++;
            }
            else {
                ApplyAimdDecrease(input, state_.lastDegradationCause, false);
            }
            badTimeSec_ = 0.0;
            cooldownSec_ = smoothGuardReleaseDecrease ? 1.1 : 1.6;
        }
        else if (guardedBandwidthPressure && lossOnlyBadTimeSec_ >= 2.0) {
            ApplyAimdDecrease(
                input,
                AdaptiveDegradationCause::Bandwidth,
                false);
            lossOnlyBadTimeSec_ = 0.0;
            cooldownSec_ = 1.4;
        }
        else if (lossOnlyPressure && lossOnlyBadTimeSec_ >= 4.0) {
            ApplyAimdDecrease(
                input,
                guardedBandwidthPressure
                ? AdaptiveDegradationCause::Bandwidth
                : AdaptiveDegradationCause::PacketLoss,
                false);
            lossOnlyBadTimeSec_ = 0.0;
            cooldownSec_ = 2.0;
        }
        else if (fecBurstTailRecovery && fecBurstTailRecoverySec_ >= 1.0) {
            ApplyAimdIncrease(350);
            fecBurstTailRecoverySec_ = 0.0;
            cooldownSec_ = 0.8;
        }
        else if (stableNetwork && stableTimeSec_ >= 3.0) {
            ApplyAimdIncrease(100);
            stableTimeSec_ = 0.0;
            cooldownSec_ = 1.2;
        }
    }

    AdaptiveStreamingState AdaptiveStreamingController::GetState() const {
        return state_;
    }

    void AdaptiveStreamingController::ReportEncodedFrame(
        size_t rawBytes,
        size_t encodedBytes
    ) {
        state_.lastRawFrameBytes = rawBytes;
        state_.lastEncodedFrameBytes = encodedBytes;

        if (rawBytes > 0 && encodedBytes > 0) {
            state_.lastCompressionRatio =
                static_cast<double>(encodedBytes) /
                static_cast<double>(rawBytes);
        }
        else {
            state_.lastCompressionRatio = 0.0;
        }
    }

    void AdaptiveStreamingController::InitializeTargetsForMode() {
        if (controlMode_ == AdaptiveControlMode::FixedQuality) {
            state_.targetJpegQuality = 85;
            state_.targetFps = 30;
            state_.targetBitrateKbps = 9500;
            state_.targetWidth = 640;
            state_.targetHeight = 360;
            return;
        }

        state_.targetBitrateKbps = 6000;
        DeriveTargetsFromBitrate();
    }

    void AdaptiveStreamingController::UpdateLossReactive(
        const AdaptiveStreamingInput& input,
        double deltaTimeSec,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta
    ) {
        if (cooldownSec_ > 0.0) {
            cooldownSec_ = (std::max)(0.0, cooldownSec_ - deltaTimeSec);
            return;
        }

        const bool lossPressure =
            HasLossPressure(input, deadlineNackDelta, deadlineNackMissingChunkDelta);

        const double lossRate = (std::max)(
            (std::max)(
                EffectiveAckMissingRate(input),
                EffectivePacketLossRate(input)),
            EffectiveBandwidthLossTrend(input)
        );
        const bool hardLossPressure =
            lossRate >= 0.10 ||
            deadlineNackMissingChunkDelta >= 3;

        const bool stableLoss =
            EffectiveAckMissingRate(input) <= 0.02 &&
            EffectivePacketLossRate(input) <= 0.02 &&
            deadlineNackDelta == 0 &&
            deadlineNackMissingChunkDelta == 0 &&
            IsCongestionRecoveryAllowed(
                input,
                deadlineNackDelta,
                deadlineNackMissingChunkDelta);

        state_.lastDegradationCause =
            lossPressure
            ? AdaptiveDegradationCause::PacketLoss
            : AdaptiveDegradationCause::None;

        if (lossPressure) {
            lossOnlyBadTimeSec_ += deltaTimeSec;
            stableTimeSec_ = 0.0;
            badTimeSec_ = 0.0;
        }
        else if (stableLoss) {
            stableTimeSec_ += deltaTimeSec;
            lossOnlyBadTimeSec_ = 0.0;
            badTimeSec_ = 0.0;
        }
        else {
            stableTimeSec_ = 0.0;
            lossOnlyBadTimeSec_ = 0.0;
            badTimeSec_ = 0.0;
        }

        if (hardLossPressure && lossOnlyBadTimeSec_ >= 0.8) {
            ApplyAimdDecrease(
                input,
                AdaptiveDegradationCause::PacketLoss,
                true);
            lossOnlyBadTimeSec_ = 0.0;
            cooldownSec_ = 1.2;
        }
        else if (lossPressure && lossOnlyBadTimeSec_ >= 3.0) {
            ApplyAimdDecrease(
                input,
                AdaptiveDegradationCause::PacketLoss,
                false);
            lossOnlyBadTimeSec_ = 0.0;
            cooldownSec_ = 2.0;
        }
        else if (stableLoss && stableTimeSec_ >= 4.0) {
            ApplyAimdIncrease(100);
            stableTimeSec_ = 0.0;
            cooldownSec_ = 1.5;
        }
    }

    void AdaptiveStreamingController::ApplyAimdMultiplicativeDecrease(double factor) {
        const int oldQuality = state_.targetJpegQuality;
        const int oldFps = state_.targetFps;
        const int oldBitrate = state_.targetBitrateKbps;
        const int oldWidth = state_.targetWidth;
        const int oldHeight = state_.targetHeight;

        const int nextBitrate = static_cast<int>(
            std::lround(static_cast<double>(state_.targetBitrateKbps) * factor)
        );
        state_.targetBitrateKbps = ClampBitrate(nextBitrate);
        DeriveTargetsFromBitrate();

        state_.qualityChanged = oldQuality != state_.targetJpegQuality;
        state_.fpsChanged = oldFps != state_.targetFps;
        state_.bitrateChanged = oldBitrate != state_.targetBitrateKbps;
        state_.resolutionChanged =
            oldWidth != state_.targetWidth ||
            oldHeight != state_.targetHeight;
    }

    void AdaptiveStreamingController::ApplyAimdDecrease(
        const AdaptiveStreamingInput& input,
        AdaptiveDegradationCause cause,
        bool hardProblem
    ) {
        if (cause == AdaptiveDegradationCause::PacingQueue) {
            return;
        }

        const double factor =
            CalculateAimdDecreaseFactor(input, cause, hardProblem);

        ApplyAimdMultiplicativeDecrease(factor);

        if (cause == AdaptiveDegradationCause::DecodeLoad ||
            cause == AdaptiveDegradationCause::DisplayLoad ||
            cause == AdaptiveDegradationCause::FrameFreshness ||
            cause == AdaptiveDegradationCause::Rtt) {
            const int oldFps = state_.targetFps;
            const int fpsStep = hardProblem ? 4 : 2;
            state_.targetFps = ClampFps(state_.targetFps - fpsStep);
            state_.fpsChanged = state_.fpsChanged || oldFps != state_.targetFps;
        }

        ApplyAdaptiveFecRecoveryQualityFloor(input);
    }

    void AdaptiveStreamingController::ApplyAimdBitrateOnlyDecrease(
        const AdaptiveStreamingInput& input,
        AdaptiveDegradationCause cause,
        bool hardProblem
    ) {
        const int oldQuality = state_.targetJpegQuality;
        const int oldFps = state_.targetFps;
        const int oldBitrate = state_.targetBitrateKbps;
        const int oldWidth = state_.targetWidth;
        const int oldHeight = state_.targetHeight;

        const double rawFactor =
            CalculateAimdDecreaseFactor(input, cause, hardProblem);
        const double smoothFactor =
            (std::max)(rawFactor, hardProblem ? 0.84 : 0.90);
        const int nextBitrate = static_cast<int>(
            std::lround(
                static_cast<double>(state_.targetBitrateKbps) *
                smoothFactor)
        );

        state_.targetBitrateKbps = ClampBitrate(nextBitrate);
        state_.qualityChanged = oldQuality != state_.targetJpegQuality;
        state_.fpsChanged = oldFps != state_.targetFps;
        state_.bitrateChanged = oldBitrate != state_.targetBitrateKbps;
        state_.resolutionChanged =
            oldWidth != state_.targetWidth ||
            oldHeight != state_.targetHeight;

        ApplyAdaptiveFecRecoveryQualityFloor(input);
    }

    void AdaptiveStreamingController::ApplyAdaptiveFecRecoveryQualityFloor(
        const AdaptiveStreamingInput& input
    ) {
        if (!input.adaptiveFecEnabled ||
            !input.fecEnabled ||
            !state_.lastFecRecoveryWorking) {
            return;
        }

        constexpr int kRecoveryFloorBitrateKbps = 2600;
        constexpr int kRecoveryFloorFps = 15;
        constexpr int kRecoveryFloorQuality = 65;
        if (state_.targetBitrateKbps >= kRecoveryFloorBitrateKbps &&
            state_.targetFps >= kRecoveryFloorFps &&
            state_.targetJpegQuality >= kRecoveryFloorQuality) {
            return;
        }

        const int oldQuality = state_.targetJpegQuality;
        const int oldFps = state_.targetFps;
        const int oldBitrate = state_.targetBitrateKbps;
        const int oldWidth = state_.targetWidth;
        const int oldHeight = state_.targetHeight;

        state_.targetBitrateKbps =
            ClampBitrate(
                (std::max)(state_.targetBitrateKbps, kRecoveryFloorBitrateKbps));
        DeriveTargetsFromBitrate();
        state_.targetFps =
            ClampFps((std::max)(state_.targetFps, kRecoveryFloorFps));
        state_.targetJpegQuality =
            ClampQuality(
                (std::max)(state_.targetJpegQuality, kRecoveryFloorQuality));

        state_.qualityChanged =
            state_.qualityChanged || oldQuality != state_.targetJpegQuality;
        state_.fpsChanged =
            state_.fpsChanged || oldFps != state_.targetFps;
        state_.bitrateChanged =
            state_.bitrateChanged || oldBitrate != state_.targetBitrateKbps;
        state_.resolutionChanged =
            state_.resolutionChanged ||
            oldWidth != state_.targetWidth ||
            oldHeight != state_.targetHeight;
    }

    void AdaptiveStreamingController::ApplyAimdIncrease(int bitrateKbps) {
        const int oldQuality = state_.targetJpegQuality;
        const int oldFps = state_.targetFps;
        const int oldBitrate = state_.targetBitrateKbps;
        const int oldWidth = state_.targetWidth;
        const int oldHeight = state_.targetHeight;

        int nextBitrate =
            ClampBitrate(state_.targetBitrateKbps + bitrateKbps);
        if (activeBandwidthCeilingKbps_ < kMaxBitrateKbps) {
            nextBitrate = (std::min)(
                nextBitrate,
                (std::max)(state_.targetBitrateKbps, activeBandwidthCeilingKbps_)
            );
        }
        state_.targetBitrateKbps = nextBitrate;
        DeriveTargetsFromBitrate();

        state_.qualityChanged = oldQuality != state_.targetJpegQuality;
        state_.fpsChanged = oldFps != state_.targetFps;
        state_.bitrateChanged = oldBitrate != state_.targetBitrateKbps;
        state_.resolutionChanged =
            oldWidth != state_.targetWidth ||
            oldHeight != state_.targetHeight;
    }

    void AdaptiveStreamingController::DeriveTargetsFromBitrate() {
        const int bitrate = state_.targetBitrateKbps;

        if (bitrate <= 900) {
            state_.targetWidth = 160;
            state_.targetHeight = 90;
            state_.targetFps = 8;
            state_.targetJpegQuality = 40;
        }
        else if (bitrate <= 1400) {
            state_.targetWidth = 240;
            state_.targetHeight = 135;
            state_.targetFps = 10;
            state_.targetJpegQuality = 45;
        }
        else if (bitrate <= 2200) {
            state_.targetWidth = 320;
            state_.targetHeight = 180;
            state_.targetFps = 12;
            state_.targetJpegQuality = 55;
        }
        else if (bitrate <= 3500) {
            state_.targetWidth = 426;
            state_.targetHeight = 240;
            state_.targetFps = 15;
            state_.targetJpegQuality = 65;
        }
        else if (bitrate <= 5000) {
            state_.targetWidth = 480;
            state_.targetHeight = 270;
            state_.targetFps = 15;
            state_.targetJpegQuality = 70;
        }
        else if (bitrate <= 7500) {
            state_.targetWidth = 640;
            state_.targetHeight = 360;
            state_.targetFps = 20;
            state_.targetJpegQuality = 78;
        }
        else if (bitrate <= 9500) {
            state_.targetWidth = 640;
            state_.targetHeight = 360;
            state_.targetFps = 24;
            state_.targetJpegQuality = 85;
        }
        else {
            state_.targetWidth = 640;
            state_.targetHeight = 360;
            state_.targetFps = 30;
            state_.targetJpegQuality = 90;
        }

        state_.targetWidth = ClampWidth(state_.targetWidth);
        state_.targetHeight = ClampHeight(state_.targetHeight);
        state_.targetFps = ClampFps(state_.targetFps);
        state_.targetJpegQuality = ClampQuality(state_.targetJpegQuality);
    }

    CongestionControlMode
    AdaptiveStreamingController::ResolveActiveCongestionControlMode() const {
        if (controlMode_ == AdaptiveControlMode::LossReactive) {
            return CongestionControlMode::LossBased;
        }

        return congestionControlMode_;
    }

    bool AdaptiveStreamingController::HasLossPressure(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta
    ) const {
        const double lossRate = (std::max)(
            (std::max)(
                EffectiveAckMissingRate(input),
                EffectivePacketLossRate(input)),
            EffectiveBandwidthLossTrend(input)
        );

        return lossRate >= 0.03 ||
            deadlineNackDelta > 0 ||
            deadlineNackMissingChunkDelta > 0;
    }

    double AdaptiveStreamingController::EffectiveAckMissingRate(
        const AdaptiveStreamingInput& input
    ) const {
        return ShouldSuppressPacingDropForQuality(input)
            ? 0.0
            : input.ackMissingRate;
    }

    double AdaptiveStreamingController::EffectivePacketLossRate(
        const AdaptiveStreamingInput& input
    ) const {
        return ShouldSuppressPacingDropForQuality(input)
            ? 0.0
            : input.packetLossRate;
    }

    double AdaptiveStreamingController::EffectiveBandwidthLossTrend(
        const AdaptiveStreamingInput& input
    ) const {
        return ShouldSuppressPacingDropForQuality(input)
            ? 0.0
            : input.bandwidthLossTrend;
    }

    bool AdaptiveStreamingController::ShouldSuppressPacingDropForQuality(
        const AdaptiveStreamingInput& input
    ) const {
        return input.pacingEnabled &&
            input.pacingDeadlineDroppedPackets > 0 &&
            !input.networkConditionEnabled &&
            !input.networkExperimentActive;
    }

    bool AdaptiveStreamingController::HasPacingDropPressure(
        const AdaptiveStreamingInput& input,
        uint64_t pacingDeadlineDropDelta
    ) const {
        return input.pacingEnabled &&
            (pacingDeadlineDropDelta > 0 ||
                input.pacingCurrentQueueDelayMs >= 30.0 ||
                (input.pacingMaxQueueDelayMs >= 100.0 &&
                    input.pacingCurrentQueueDelayMs >= 20.0));
    }

    bool AdaptiveStreamingController::HasDelayPressure(
        const AdaptiveStreamingInput& input
    ) const {
        return input.bandwidthQueueDelayMs >= 30.0 ||
            input.bandwidthRttTrendMs >= 10.0 ||
            input.bandwidthJitterTrendMs >= 20.0 ||
            input.rttMs >= 140.0 ||
            input.jitterMs >= 35.0 ||
            input.latencyMs >= 130.0;
    }

    bool AdaptiveStreamingController::HasCongestionPressure(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta
    ) const {
        const CongestionControlMode mode =
            ResolveActiveCongestionControlMode();
        const bool lossPressure =
            HasLossPressure(input, deadlineNackDelta, deadlineNackMissingChunkDelta);
        const bool delayPressure = HasDelayPressure(input);
        const bool bandwidthPressure = HasBandwidthPressure(input);

        switch (mode) {
        case CongestionControlMode::LossBased:
            return lossPressure;
        case CongestionControlMode::DelayBased:
            return delayPressure || bandwidthPressure;
        case CongestionControlMode::Hybrid:
        default:
            return lossPressure || delayPressure || bandwidthPressure;
        }
    }

    bool AdaptiveStreamingController::IsCongestionRecoveryAllowed(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta
    ) const {
        const bool lossStable =
            EffectiveAckMissingRate(input) <= 0.02 &&
            EffectivePacketLossRate(input) <= 0.02 &&
            EffectiveBandwidthLossTrend(input) <= 0.015 &&
            deadlineNackDelta == 0 &&
            deadlineNackMissingChunkDelta == 0;
        const bool delayStable =
            input.rttMs <= 100.0 &&
            input.latencyMs <= 100.0 &&
            input.jitterMs <= 20.0 &&
            input.bandwidthQueueDelayMs <= 10.0 &&
            input.bandwidthRttTrendMs <= 4.0 &&
            input.bandwidthJitterTrendMs <= 8.0;

        switch (ResolveActiveCongestionControlMode()) {
        case CongestionControlMode::LossBased:
            return lossStable && IsBandwidthRecoveryAllowed(input);
        case CongestionControlMode::DelayBased:
            return delayStable && IsBandwidthRecoveryAllowed(input);
        case CongestionControlMode::Hybrid:
        default:
            return lossStable && delayStable && IsBandwidthRecoveryAllowed(input);
        }
    }

    double AdaptiveStreamingController::CalculateAimdDecreaseFactor(
        const AdaptiveStreamingInput& input,
        AdaptiveDegradationCause cause,
        bool hardProblem
    ) const {
        const double lossRate = (std::max)(
            (std::max)(
                EffectiveAckMissingRate(input),
                EffectivePacketLossRate(input)),
            EffectiveBandwidthLossTrend(input)
        );
        const double queueDelayMs = input.bandwidthQueueDelayMs;

        switch (cause) {
        case AdaptiveDegradationCause::PacketLoss:
            if (lossRate > 0.20) {
                return 0.50;
            }
            if (lossRate > 0.10) {
                return 0.70;
            }
            if (lossRate > 0.05) {
                return 0.85;
            }
            return hardProblem ? 0.84 : 0.90;
        case AdaptiveDegradationCause::Bandwidth:
        case AdaptiveDegradationCause::Rtt:
        case AdaptiveDegradationCause::Jitter:
            if (queueDelayMs > 80.0 ||
                input.bandwidthRttTrendMs > 30.0) {
                return 0.70;
            }
            if (queueDelayMs > 30.0 ||
                input.bandwidthRttTrendMs > 10.0 ||
                input.bandwidthJitterTrendMs > 20.0) {
                return 0.85;
            }
            return hardProblem ? 0.82 : 0.90;
        case AdaptiveDegradationCause::DecodeLoad:
        case AdaptiveDegradationCause::DisplayLoad:
            return hardProblem ? 0.78 : 0.86;
        case AdaptiveDegradationCause::FrameFreshness:
            return hardProblem ? 0.76 : 0.84;
        case AdaptiveDegradationCause::RecoveryDeadline:
            return hardProblem ? 0.82 : 0.90;
        case AdaptiveDegradationCause::PacingQueue:
            return hardProblem ? 0.88 : 0.94;
        case AdaptiveDegradationCause::None:
        default:
            return hardProblem ? 0.82 : 0.90;
        }
    }

    bool AdaptiveStreamingController::HasBandwidthEstimate(
        const AdaptiveStreamingInput& input
    ) const {
        return input.bandwidthFeedbackSamples >= 128 &&
            observedTimeSec_ >= 5.0 &&
            input.estimatedBandwidthBps >=
            static_cast<uint32_t>(kMinBitrateKbps * 1000) &&
            input.deliveryRateBps > 0;
    }

    int AdaptiveStreamingController::CalculateBandwidthCeilingKbps(
        const AdaptiveStreamingInput& input
    ) const {
        if (controlMode_ == AdaptiveControlMode::FixedQuality ||
            !HasBandwidthEstimate(input) ||
            !HasBandwidthCongestionEvidence(input)) {
            return kMaxBitrateKbps;
        }

        const double estimatedKbps =
            static_cast<double>(input.estimatedBandwidthBps) / 1000.0;
        const double safetyMargin =
            input.bandwidthQueueDelayMs >= 8.0 ||
            EffectiveBandwidthLossTrend(input) >= 0.04 ||
            input.bandwidthRttTrendMs >= 10.0
            ? 0.80
            : 0.90;

        const int ceilingKbps = static_cast<int>(
            std::lround(estimatedKbps * safetyMargin)
        );

        return (std::max)(
            kMinBitrateKbps,
            (std::min)(kMaxBitrateKbps, ceilingKbps)
        );
    }

    bool AdaptiveStreamingController::HasBandwidthPressure(
        const AdaptiveStreamingInput& input
    ) const {
        if (controlMode_ == AdaptiveControlMode::FixedQuality ||
            !HasBandwidthEstimate(input) ||
            !HasBandwidthCongestionEvidence(input)) {
            return false;
        }

        const int ceilingKbps = CalculateBandwidthCeilingKbps(input);
        if (state_.targetBitrateKbps <= ceilingKbps + 300) {
            return false;
        }

        const double deliveryKbps =
            static_cast<double>(input.deliveryRateBps) / 1000.0;
        const bool deliveryBelowTarget =
            deliveryKbps > 0.0 &&
            deliveryKbps <
            static_cast<double>(state_.targetBitrateKbps) * 0.75;

        return deliveryBelowTarget ||
            input.bandwidthQueueDelayMs >= 30.0 ||
            EffectiveBandwidthLossTrend(input) >= 0.05 ||
            input.bandwidthRttTrendMs >= 10.0 ||
            input.bandwidthJitterTrendMs >= 20.0;
    }

    bool AdaptiveStreamingController::HasBandwidthCongestionEvidence(
        const AdaptiveStreamingInput& input
    ) const {
        return input.bandwidthQueueDelayMs >= 10.0 ||
            EffectiveBandwidthLossTrend(input) >= 0.03 ||
            input.bandwidthRttTrendMs >= 8.0 ||
            input.bandwidthJitterTrendMs >= 10.0;
    }

    bool AdaptiveStreamingController::IsBandwidthRecoveryAllowed(
        const AdaptiveStreamingInput& input
    ) const {
        if (controlMode_ == AdaptiveControlMode::FixedQuality ||
            !HasBandwidthEstimate(input)) {
            return true;
        }

        if (!HasBandwidthCongestionEvidence(input)) {
            return true;
        }

        const int ceilingKbps = CalculateBandwidthCeilingKbps(input);
        const bool hasHeadroom =
            ceilingKbps >= state_.targetBitrateKbps + 300;
        const bool estimatorStable =
            input.bandwidthQueueDelayMs <= 2.0 &&
            EffectiveBandwidthLossTrend(input) <= 0.015 &&
            input.bandwidthRttTrendMs <= 4.0 &&
            input.bandwidthJitterTrendMs <= 5.0;

        return hasHeadroom && estimatorStable;
    }

    AdaptiveDegradationCause AdaptiveStreamingController::DetermineDegradationCause(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineDropDelta,
        uint64_t outputQueueDropDelta,
        uint64_t deadlineNackDelta,
        uint64_t deadlineNackMissingChunkDelta,
        uint64_t recoveryDeadlineDropDelta,
        uint64_t retransmitStaleDropDelta,
        uint64_t freshnessDropDelta
    ) const {
        const double freshnessThresholdMs =
            input.receiveFreshnessDropThresholdMs;
        const bool hasFreshnessThreshold = freshnessThresholdMs > 0.0;
        const bool decodeInputStale =
            hasFreshnessThreshold &&
            input.receiveDecodeInputFrameAgeMs >= freshnessThresholdMs;
        const bool latestDecodedStale =
            hasFreshnessThreshold &&
            input.receiveLatestDecodedFrameAgeMs >=
            freshnessThresholdMs * 1.25;

        if (freshnessDropDelta > 0 ||
            decodeInputStale ||
            latestDecodedStale) {
            return AdaptiveDegradationCause::FrameFreshness;
        }

        if (recoveryDeadlineDropDelta > 0 ||
            retransmitStaleDropDelta > 0) {
            return AdaptiveDegradationCause::RecoveryDeadline;
        }

        const CongestionControlMode congestionMode =
            ResolveActiveCongestionControlMode();

        if (congestionMode == CongestionControlMode::LossBased) {
            return HasLossPressure(
                input,
                deadlineNackDelta,
                deadlineNackMissingChunkDelta)
                ? AdaptiveDegradationCause::PacketLoss
                : AdaptiveDegradationCause::None;
        }

        if (congestionMode == CongestionControlMode::DelayBased) {
            if (HasBandwidthPressure(input)) {
                return AdaptiveDegradationCause::Bandwidth;
            }
            if (input.rttMs >= 140.0 ||
                input.latencyMs >= 130.0 ||
                input.bandwidthRttTrendMs >= 10.0) {
                return AdaptiveDegradationCause::Rtt;
            }
            if (input.jitterMs >= 35.0 ||
                input.bandwidthJitterTrendMs >= 20.0 ||
                input.bandwidthQueueDelayMs >= 30.0) {
                return AdaptiveDegradationCause::Jitter;
            }

            return AdaptiveDegradationCause::None;
        }

        const bool enoughDecodeData =
            observedTimeSec_ >= 2.0 &&
            input.receiveFps >= 5.0 &&
            input.decodeFps > 0.0;

        const bool enoughDisplayData =
            observedTimeSec_ >= 2.0 &&
            input.displayedFrames >= 10 &&
            input.decodeFps >= 5.0 &&
            input.displayFps > 0.0;

        const bool rendererLag =
            outputQueueDropDelta > 0 &&
            input.lastOutputQueueDropReason == "renderer-lag";
        const bool jitterBurst =
            outputQueueDropDelta > 0 &&
            input.lastOutputQueueDropReason == "jitter-burst-release";

        if (rendererLag ||
            (enoughDisplayData &&
                input.displayFps < input.decodeFps * 0.75)) {
            return AdaptiveDegradationCause::DisplayLoad;
        }

        if (enoughDecodeData &&
            input.decodeFps < input.receiveFps * 0.75) {
            return AdaptiveDegradationCause::DecodeLoad;
        }

        if (input.rttMs >= 140.0 ||
            (input.latencyMs >= 130.0 && input.rttMs >= 100.0)) {
            return AdaptiveDegradationCause::Rtt;
        }

        if (HasBandwidthPressure(input)) {
            return AdaptiveDegradationCause::Bandwidth;
        }

        if (deadlineNackDelta >= 3 ||
            deadlineNackMissingChunkDelta >= 3 ||
            ((EffectiveAckMissingRate(input) >= 0.03 ||
                EffectivePacketLossRate(input) >= 0.08) &&
                input.jitterMs < 35.0)) {
            return AdaptiveDegradationCause::PacketLoss;
        }

        if (input.jitterMs >= 35.0 ||
            jitterBurst ||
            (outputQueueDropDelta > 0 && input.jitterMs >= 15.0)) {
            return AdaptiveDegradationCause::Jitter;
        }

        if (EffectiveAckMissingRate(input) >= 0.03 ||
            EffectivePacketLossRate(input) >= 0.03 ||
            deadlineNackDelta > 0 ||
            deadlineNackMissingChunkDelta > 0 ||
            deadlineDropDelta > 0) {
            return AdaptiveDegradationCause::PacketLoss;
        }

        return AdaptiveDegradationCause::None;
    }

    double AdaptiveStreamingController::CalculateQoeScore(
        const AdaptiveStreamingInput& input,
        uint64_t deadlineDropDelta,
        uint64_t outputQueueDropDelta,
        uint64_t recoveryDeadlineDropDelta,
        uint64_t retransmitStaleDropDelta,
        uint64_t freshnessDropDelta
    ) const {
        double score = 0.0;

        if (deadlineDropDelta > 0) {
            score = (std::max)(score, 3.0);
        }
        if (outputQueueDropDelta > 0) {
            if (input.lastOutputQueueDropReason == "renderer-lag") {
                score = (std::max)(score, 2.0);
            }
            else if (input.lastOutputQueueDropReason == "jitter-burst-release") {
                score = (std::max)(score, 1.0);
            }
            else {
                score = (std::max)(score, 2.0);
            }
        }

        if (freshnessDropDelta > 0) {
            score = (std::max)(score, 3.0);
        }
        else if (input.receiveFreshnessDropThresholdMs > 0.0) {
            const double thresholdMs = input.receiveFreshnessDropThresholdMs;
            const double frameAgeMs = (std::max)(
                input.receiveDecodeInputFrameAgeMs,
                input.receiveLatestDecodedFrameAgeMs);

            if (frameAgeMs >= thresholdMs * 1.25) {
                score = (std::max)(score, 3.0);
            }
            else if (frameAgeMs >= thresholdMs) {
                score = (std::max)(score, 2.0);
            }
            else if (frameAgeMs >= thresholdMs * 0.75) {
                score = (std::max)(score, 1.0);
            }
        }

        if (recoveryDeadlineDropDelta > 0) {
            score = (std::max)(score, 2.0);
        }
        else if (retransmitStaleDropDelta > 0) {
            score = (std::max)(score, 1.0);
        }

        if (input.latencyMs >= 150.0 || input.rttMs >= 220.0) {
            score = (std::max)(score, 3.0);
        }
        else if (input.latencyMs >= 130.0 || input.rttMs >= 180.0) {
            score = (std::max)(score, 2.0);
        }
        else if (input.latencyMs >= 110.0 || input.rttMs >= 140.0) {
            score = (std::max)(score, 1.0);
        }

        const bool displayFpsReady =
            observedTimeSec_ >= 2.0 &&
            input.displayedFrames >= 10 &&
            input.displayFps > 0.0 &&
            state_.targetFps > 0;

        if (displayFpsReady) {
            const double displayRatio =
                input.displayFps / static_cast<double>(state_.targetFps);

            if (displayRatio < 0.60) {
                score = (std::max)(score, 2.0);
            }
            else if (displayRatio < 0.75) {
                score = (std::max)(score, 1.0);
            }
        }

        const bool decodeFpsReady =
            observedTimeSec_ >= 2.0 &&
            input.receiveFps >= 5.0 &&
            input.decodeFps > 0.0;

        if (decodeFpsReady) {
            const double decodeRatio = input.decodeFps / input.receiveFps;
            if (decodeRatio < 0.60) {
                score = (std::max)(score, 2.0);
            }
            else if (decodeRatio < 0.75) {
                score = (std::max)(score, 1.0);
            }
        }

        const bool displayVsDecodeReady =
            observedTimeSec_ >= 2.0 &&
            input.displayedFrames >= 10 &&
            input.decodeFps >= 5.0 &&
            input.displayFps > 0.0;

        if (displayVsDecodeReady) {
            const double displayDecodeRatio =
                input.displayFps / input.decodeFps;
            if (displayDecodeRatio < 0.60) {
                score = (std::max)(score, 2.0);
            }
            else if (displayDecodeRatio < 0.75) {
                score = (std::max)(score, 1.0);
            }
        }

        if (input.jitterMs >= 50.0) {
            score = (std::max)(score, 2.0);
        }
        else if (input.jitterMs >= 30.0) {
            score = (std::max)(score, 1.0);
        }

        if (EffectiveAckMissingRate(input) >= 0.08 ||
            EffectivePacketLossRate(input) >= 0.08) {
            score = (std::max)(score, 0.75);
        }
        else if (EffectiveAckMissingRate(input) >= 0.03 ||
            EffectivePacketLossRate(input) >= 0.03) {
            score = (std::max)(score, 0.5);
        }

        return score;
    }

    int AdaptiveStreamingController::ClampQuality(int value) const {
        return (std::max)(kMinQuality, (std::min)(kMaxQuality, value));
    }

    int AdaptiveStreamingController::ClampFps(int value) const {
        return (std::max)(kMinFps, (std::min)(kMaxFps, value));
    }

    int AdaptiveStreamingController::ClampBitrate(int value) const {
        return (std::max)(kMinBitrateKbps, (std::min)(kMaxBitrateKbps, value));
    }

    int AdaptiveStreamingController::ClampWidth(int value) const {
        return (std::max)(160, (std::min)(640, value));
    }

    int AdaptiveStreamingController::ClampHeight(int value) const {
        return (std::max)(90, (std::min)(360, value));
    }

} // namespace net
