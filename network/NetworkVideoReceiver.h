#pragma once

#include "FrameReassembler.h"

#include <atomic>
#include <cstdint>
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
    double latestDecodedFrameAgeMs = 0.0;
    double freshnessDropThresholdMs = 0.0;
    std::string lastDropReason;
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
    void DecodeLoop();
    void StoreDecodedFrame(DecodedVideoFrame frame, double jpegDecodeMs);
    void UpdateDecodeMs(double sampleMs);
    void UpdateInputFrameAge(double sampleMs);
    void UpdateDecodeWorkerFpsLocked(uint64_t nowUs);
    void RecordDropLocked(const char* reason, bool queueDrop);
    void RecordH264AuInvalidLocked(const char* reason);
    static uint64_t NowMicroseconds();

    UdpReceiver* receiver_ = nullptr;
    std::function<bool()> enabledProvider_;
    std::atomic<bool> running_{false};
    std::thread workerThread_;

    mutable std::mutex mutex_;
    DecodedVideoFrame latestDecodedFrame_{};
    bool hasLatestDecodedFrame_ = false;
    uint64_t latestDecodedFrameTimeUs_ = 0;
    NetworkVideoReceiverStats stats_{};
    bool hasJpegDecodeMs_ = false;
    bool hasInputFrameAgeMs_ = false;
    uint64_t lastFpsUpdateTimeUs_ = 0;
    uint64_t decodedFramesAtLastFpsUpdate_ = 0;
};

} // namespace net
