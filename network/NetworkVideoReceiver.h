#pragma once

#include "FrameReassembler.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net {

class UdpReceiver;

struct NetworkVideoReceiverStats {
    double jpegDecodeMs = 0.0;
    double decodeWorkerFps = 0.0;
    uint64_t decodedFrames = 0;
    uint64_t decodePopSuccesses = 0;
    uint64_t decodePopEmptyPolls = 0;
    double decodeLoopLastPopGapMs = 0.0;
    double decodeLoopMaxPopGapMs = 0.0;
    double startupDecodeLoopMaxPopGapMs = 0.0;
    double steadyDecodeLoopMaxPopGapMs = 0.0;
    double steadyDecodeLoopMaxPopGapAtMs = 0.0;
    uint32_t steadyDecodeLoopMaxPopGapFrameId = 0;
    uint32_t steadyDecodeLoopMaxPopGapStreamId = 0;
    std::string steadyDecodeLoopMaxPopGapCodec;
    double steadyDecodeLoopMaxPopGapInputFrameAgeMs = 0.0;
    uint32_t steadyDecodeLoopMaxPopGapDecodedQueueSize = 0;
    uint32_t steadyDecodeLoopMaxPopGapCompletedQueueSize = 0;
    double steadyDecodeLoopMaxPopGapCompletedQueuePopAgeMs = 0.0;
    double steadyDecodeLoopMaxPopGapCompletedQueuePushIntervalMs = 0.0;
    double steadyDecodeLoopMaxPopGapArrivalRatio = 0.0;
    double steadyDecodeLoopMaxPopGapReceiverJitterMs = 0.0;
    double steadyDecodeLoopMaxPopGapReceiverLatencyMs = 0.0;
    std::string steadyDecodeLoopMaxPopGapClass;
    bool startupActive = true;
    bool startupDecoderSynced = false;
    bool startupFirstDecoded = false;
    bool startupFirstDisplayed = false;
    bool startupReady = false;
    double startupElapsedMs = 0.0;
    double startupDecoderSyncMs = 0.0;
    double startupFirstDecodedMs = 0.0;
    double startupFirstDisplayedMs = 0.0;
    double startupReadyMs = 0.0;
    uint64_t startupQueueFlushFrames = 0;
    uint64_t overwrittenFrames = 0;
    uint64_t decodeQueueDroppedFrames = 0;
    uint64_t decodeRenderOverwriteFrames = 0;
    uint64_t decodeFailures = 0;
    uint64_t freshnessDroppedFrames = 0;
    uint64_t h264AuInvalidFrames = 0;
    uint64_t h264AuCrcMismatches = 0;
    uint64_t h264AuPayloadSizeMismatches = 0;
    uint64_t h264AuNalCountMismatches = 0;
    uint64_t h264AuNoAnnexBNals = 0;
    uint64_t h264AuIdrFlagMismatches = 0;
    uint64_t h264AuSpsPpsFlagMismatches = 0;
    uint64_t h264AuSyncWithoutIdr = 0;
    uint64_t h264AuIdrWithoutSpsPps = 0;
    uint64_t h264AuForbiddenZeroBit = 0;
    std::string h264AuLastInvalidReason;
    double decodeInputFrameAgeMs = 0.0;
    double decodeInputCameraFrameAgeMs = 0.0;
    double decodeInputEncoderOutputAgeMs = 0.0;
    double latestDecodedFrameAgeMs = 0.0;
    double latestDecodedCameraFrameAgeMs = 0.0;
    double latestDecodedEncoderOutputAgeMs = 0.0;
    double freshnessDropThresholdMs = 0.0;
    std::string lastDropReason;
    double lastFreshnessDropAgeMs = 0.0;
    double maxFreshnessDropAgeMs = 0.0;
    uint32_t lastFreshnessDropFrameId = 0;
    uint32_t lastFreshnessDropStreamId = 0;
    std::string lastFreshnessDropCodec;
};

enum class DecodedVideoFrameFormat {
    Rgba8,
    Nv12
};

struct DecodedVideoFrame {
    uint32_t frameId = 0;
    uint32_t streamId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t sendTimeUs = 0;
    uint64_t receiveTimeUs = 0;
    uint64_t decodedTimeUs = 0;
    uint64_t cameraCaptureCompletedTimeUs = 0;
    uint64_t encoderOutputTimeUs = 0;
    DecodedVideoFrameFormat format = DecodedVideoFrameFormat::Rgba8;
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> nv12Y;
    std::vector<uint8_t> nv12UV;
    uint32_t nv12YPitch = 0;
    uint32_t nv12UVPitch = 0;
};

class NetworkVideoReceiver {
public:
    NetworkVideoReceiver() = default;
    ~NetworkVideoReceiver();

    NetworkVideoReceiver(const NetworkVideoReceiver&) = delete;
    NetworkVideoReceiver& operator=(const NetworkVideoReceiver&) = delete;

    bool Start(UdpReceiver* receiver, std::function<bool()> enabledProvider);
    void Stop();

    bool IsRunning() const;
    bool TryGetLatestFrame(DecodedVideoFrame& outFrame);
    NetworkVideoReceiverStats GetStats() const;

private:
    struct PendingDecodedFrame {
        DecodedVideoFrame frame;
        uint64_t storedTimeUs = 0;
    };

    void DecodeLoop();
    bool ShouldPredecodeCoalesceFrame(
        const CompletedFrame& frame,
        uint64_t nowUs,
        const char** outReason
    ) const;
    void RecordPredecodeCoalescedFrame(const char* reason);
    void StoreDecodedFrame(
        DecodedVideoFrame frame,
        double jpegDecodeMs,
        bool decoderSyncReady = true
    );
    void UpdateDecodeMs(double sampleMs);
    void UpdateInputFrameAge(double sampleMs);
    void UpdateInputCameraFrameAge(double sampleMs);
    void UpdateInputEncoderOutputAge(double sampleMs);
    void UpdateDecodeWorkerFpsLocked(uint64_t nowUs);
    void RecordStartupDecoderSyncedLocked(uint64_t nowUs);
    void RecordStartupFirstDecodedLocked(uint64_t nowUs);
    void RecordStartupFirstDisplayedLocked(uint64_t nowUs);
    void TryCompleteStartupLocked(uint64_t nowUs);
    void RecordDropLocked(
        const char* reason,
        bool queueDrop,
        double freshnessAgeMs = -1.0,
        uint32_t frameId = 0,
        uint32_t streamId = 0,
        const char* codec = nullptr
    );
    void RecordH264AuInvalidLocked(const char* reason);
    static uint64_t NowMicroseconds();

    UdpReceiver* receiver_ = nullptr;
    std::function<bool()> enabledProvider_;
    std::atomic<bool> running_{false};
    std::thread workerThread_;

    mutable std::mutex mutex_;
    std::deque<PendingDecodedFrame> decodedFrameQueue_;
    NetworkVideoReceiverStats stats_{};
    bool hasJpegDecodeMs_ = false;
    bool hasInputFrameAgeMs_ = false;
    bool hasInputCameraFrameAgeMs_ = false;
    bool hasInputEncoderOutputAgeMs_ = false;
    uint64_t lastFpsUpdateTimeUs_ = 0;
    uint64_t decodedFramesAtLastFpsUpdate_ = 0;
    uint64_t lastDecodePopTimeUs_ = 0;
    uint64_t startupStartTimeUs_ = 0;
};

} // namespace net
