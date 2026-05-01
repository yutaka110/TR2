#pragma once
#include <mfapi.h>
#include <mftransform.h>
#include <mfobjects.h>
#include <wrl/client.h>

class ColorConverter {
public:
    bool Initialize(UINT width, UINT height);
    bool Convert(IMFSample* inputSample, IMFSample** outputSample);
    ~ColorConverter();  // デストラクタを明示的に追加
    bool ProcessMessage(DWORD msg, ULONG_PTR param);

private:
    Microsoft::WRL::ComPtr<IMFTransform> converter_;
    UINT width_ = 0;
    UINT height_ = 0;
};
