#pragma once
#include <mfapi.h>
#include <mftransform.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <wrl.h>
#include <vector>

class H264Encoder {
public:
    // H.264エンコーダの初期化（幅、高さ、ビットレート、フレームレート）
    bool Initialize(UINT32 width, UINT32 height, UINT32 bitrate = 800000, UINT32 fps = 30);

    // RGBフレームをエンコードし、H.264のバイト列を出力ベクタに格納する
    bool EncodeFrame(const BYTE* rgbData, UINT dataSize, std::vector<BYTE>& outH264Data);

    // シャットダウン処理（エンコーダの解放）
    void Shutdown();

    // エンコードされた SPS / PPS を取り出す（Annex-B 形式）
    std::vector<uint8_t> GetSpsPps() const;

    // H264Encoder.h
    bool EncodeSample(IMFSample* inputSample, std::vector<uint8_t>& outData);
    bool FlushDelayedFrames(std::vector<std::vector<BYTE>>& flushedFrames);

private:
    Microsoft::WRL::ComPtr<IMFTransform> encoder_;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT fps_ = 0;
    LONGLONG frameCount_ = 0;
    std::vector<uint8_t> spsPpsBuffer_;  // SPS / PPS を保存するバッファ
};
