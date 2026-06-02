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
    bool Initialize(UINT32 width, UINT32 height);
    bool GetFrame(IMFSample** outSample);
    bool TryGetRgbaFrame(std::vector<uint8_t>& outRgba);
    void StartAsyncCapture();
    void StopAsyncCapture();
    bool TryGetLatestRgbaFrame(std::vector<uint8_t>& outRgba);
    bool TryGetLatestRgbaFrame(
        std::vector<uint8_t>& outRgba,
        uint64_t& outFrameId
    );
    double GetAsyncCaptureFps() const;
    void Shutdown();

private:
    void AsyncCaptureLoop();
    bool ConfigureRgb32Output(UINT32 width, UINT32 height, bool setFrameSize);
    bool UpdateCurrentFrameSize();
    void LogDirectShowVideoDevices() const;
    void Log(const std::string& message) const;
    void LogHr(const char* label, HRESULT hr) const;

    IMFSourceReader* reader_ = nullptr;
    bool mfStarted_ = false;
    UINT32 captureWidth_ = 0;
    UINT32 captureHeight_ = 0;
    UINT32 outputWidth_ = 0;
    UINT32 outputHeight_ = 0;
    mutable uint32_t frameFailureLogCount_ = 0;

    std::atomic<bool> asyncRunning_{ false };
    std::thread asyncThread_;
    mutable std::mutex latestFrameMutex_;
    std::vector<uint8_t> latestFrame_;
    uint64_t latestFrameId_ = 0;
    bool latestFrameReady_ = false;
    double asyncCaptureFps_ = 0.0;
    uint64_t asyncCapturedFrames_ = 0;
    uint64_t asyncCapturedFramesAtLastFpsUpdate_ = 0;
    std::chrono::steady_clock::time_point asyncLastFpsUpdate_{};
};
