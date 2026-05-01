#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

class CameraCapture {
public:
    bool Initialize(UINT32 width, UINT32 height);
    bool GetFrame(IMFSample** outSample); // 呼び出し元が Release() すべき
    void Shutdown();

private:
    IMFSourceReader* reader_ = nullptr;
};


