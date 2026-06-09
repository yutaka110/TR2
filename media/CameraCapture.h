#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CameraCapture {
public:
    struct FrameMetadata {
        uint64_t frameId = 0;
        int64_t sourceTimestamp100ns = 0;
        uint64_t captureCompletedTimeUs = 0;
        double readSampleMs = 0.0;
    };

    struct Nv12Frame {
        FrameMetadata metadata{};
        UINT32 width = 0;
        UINT32 height = 0;
        UINT32 yPitch = 0;
        UINT32 uvPitch = 0;
        std::vector<uint8_t> yPlane;
        std::vector<uint8_t> uvPlane;
    };

    bool Initialize(UINT32 width, UINT32 height);
    bool GetFrame(
        IMFSample** outSample,
        int64_t* outSourceTimestamp100ns = nullptr,
        double* outReadSampleMs = nullptr
    );
    bool TryGetRgbaFrame(std::vector<uint8_t>& outRgba);
    bool TryGetRgbaFrame(
        std::vector<uint8_t>& outRgba,
        FrameMetadata& outMetadata
    );
    void StartAsyncCapture();
    void StopAsyncCapture();
    bool TryGetLatestRgbaFrame(std::vector<uint8_t>& outRgba);
    bool TryGetLatestRgbaFrame(
        std::vector<uint8_t>& outRgba,
        uint64_t& outFrameId
    );
    bool TryGetLatestRgbaFrame(
        std::vector<uint8_t>& outRgba,
        FrameMetadata& outMetadata
    );
    bool TryGetLatestNv12Frame(Nv12Frame& outFrame);
    double GetAsyncCaptureFps() const;
    void Shutdown();

private:
    static uint64_t NowMicroseconds();
    void AsyncCaptureLoop();
    bool OpenReader(UINT32 width, UINT32 height);
    bool RecoverReader();
    void ReleaseReader();
    bool ConfigureLowLatencyOutput(UINT32 width, UINT32 height);
    bool ConfigureRgb32Output(UINT32 width, UINT32 height, bool setFrameSize);
    bool UpdateCurrentFrameSize();
    bool TryGetNv12Frame(Nv12Frame& outFrame);
    bool ConvertLatestNv12ToRgbaLocked(
        std::vector<uint8_t>& outRgba,
        FrameMetadata& outMetadata) const;
    void LogDirectShowVideoDevices() const;
    void Log(const std::string& message) const;
    void LogHr(const char* label, HRESULT hr) const;

    IMFSourceReader* reader_ = nullptr;
    bool mfStarted_ = false;
    UINT32 captureWidth_ = 0;
    UINT32 captureHeight_ = 0;
    GUID captureSubtype_ = GUID_NULL;
    LONG captureStride_ = 0;
    UINT32 outputWidth_ = 0;
    UINT32 outputHeight_ = 0;
    mutable uint32_t frameFailureLogCount_ = 0;

    std::atomic<bool> asyncRunning_{ false };
    std::thread asyncThread_;
    mutable std::mutex latestFrameMutex_;
    std::vector<uint8_t> latestFrame_;
    Nv12Frame latestNv12Frame_{};
    FrameMetadata latestFrameMetadata_{};
    uint64_t latestFrameId_ = 0;
    bool latestFrameReady_ = false;
    double asyncCaptureFps_ = 0.0;
    uint64_t asyncCapturedFrames_ = 0;
    uint64_t asyncCapturedFramesAtLastFpsUpdate_ = 0;
    std::chrono::steady_clock::time_point asyncLastFpsUpdate_{};
};
