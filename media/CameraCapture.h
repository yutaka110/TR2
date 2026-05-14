#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstdint>
#include <string>
#include <vector>

class CameraCapture {
public:
    bool Initialize(UINT32 width, UINT32 height);
    bool GetFrame(IMFSample** outSample);
    bool TryGetRgbaFrame(std::vector<uint8_t>& outRgba);
    void Shutdown();

private:
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
};
