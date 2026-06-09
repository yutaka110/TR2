#pragma once
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <d3d11.h>
#include <wrl.h>
#include <vector>

class H264Encoder {
public:
    // H.264エンコーダの初期化（幅、高さ、ビットレート、フレームレート）
    bool Initialize(UINT32 width, UINT32 height, UINT32 bitrate = 800000, UINT32 fps = 30);

    // RGBフレームをエンコードし、H.264のバイト列を出力ベクタに格納する
    bool EncodeFrame(const BYTE* rgbData, UINT dataSize, std::vector<BYTE>& outH264Data);
    void RequestKeyFrame();
    bool SetTargetBitrate(UINT32 bitrate);

    // シャットダウン処理（エンコーダの解放）
    void Shutdown();

    // エンコードされた SPS / PPS を取り出す（Annex-B 形式）
    std::vector<uint8_t> GetSpsPps() const;

    // H264Encoder.h
    bool EncodeSample(IMFSample* inputSample, std::vector<uint8_t>& outData);
    bool FlushDelayedFrames(std::vector<std::vector<BYTE>>& flushedFrames);

private:
    bool InitializeInternal(
        UINT32 width,
        UINT32 height,
        UINT32 bitrate,
        UINT32 fps,
        bool allowHardware);
    bool EncodeHardwareFrameAsync(
        const BYTE* data,
        UINT dataSize,
        std::vector<BYTE>& outH264Data);
    bool PumpHardwareEncoderEvents(
        DWORD timeoutMs,
        std::vector<BYTE>* outH264Data);
    bool ProcessAvailableOutput(std::vector<BYTE>& outH264Data);
    void ExtractSpsPps(const std::vector<BYTE>& h264Data);

    Microsoft::WRL::ComPtr<IMFTransform> encoder_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> asyncEventGenerator_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11Device_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager_;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT fps_ = 0;
    UINT bitrate_ = 0;
    UINT dxgiDeviceManagerResetToken_ = 0;
    DWORD inputBufferBytes_ = 0;
    DWORD outputBufferBytes_ = 0;
    bool outputProvidesSamples_ = false;
    bool usingHardwareEncoder_ = false;
    bool asyncHardwareEncoder_ = false;
    bool hardwareNeedsInput_ = false;
    LONGLONG frameCount_ = 0;
    std::vector<uint8_t> spsPpsBuffer_;  // SPS / PPS を保存するバッファ
};
