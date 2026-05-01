// MfH264Receiver.h --- Aルート（RGB32）版
#pragma once
#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <vector>
#include <string>
#include <cassert>
#include <algorithm>
#include <queue>
#include <memory>
#include <mutex>
#include <utility> 
using Microsoft::WRL::ComPtr;

// 前方宣言
struct IMFSample;
struct IMFMediaBuffer;
struct IMFTransform;

struct RGBA8Desc {
    UINT width = 0;
    UINT height = 0;
    LONG pitch = 0;    // MF側の1行バイト数（RGB32）
};

class SamplePool {
public:
    // RGB32（1平面）のメモリバッファを持つIMFSampleを確保
    HRESULT Initialize(UINT width, UINT height, UINT poolSize);
    HRESULT Acquire(ComPtr<IMFSample>& out);
    void     Release(ComPtr<IMFSample> s);
private:
    UINT width_ = 0, height_ = 0;
    std::vector<ComPtr<IMFSample>> pool_;
};

class MfH264Receiver {
public:
    // 入力:H.264 Annex-B, 出力:RGB32（システムメモリ）
    HRESULT InitializeDecoder(UINT width, UINT height);

    // Annex-B AUを投入（ts100ns=100ns単位PTS, isIDRでキーフレ判定）
    HRESULT FeedAccessUnit(const BYTE* data, DWORD size, LONGLONG ts100ns, BOOL isIDR);

    // RGB32 1フレームをDrain → Upload(Texture2D, UPLOAD)へ行ピッチで書き込み → DefaultへCopy
    // rgbaTex:   Defaultヒープ RGBA8 テクスチャ
    // rgbaUpload:Uploadヒープ RGBA8 テクスチャ（同サイズ・同フォーマット）
    bool DrainOneFrameToD3D12(
        ID3D12Resource* rgbaTex,
        ID3D12Resource* rgbaUpload,
        ID3D12GraphicsCommandList* cmd,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
        bool bgraToRgba,
        const std::shared_ptr<std::queue<std::vector<uint8_t>>>& frameQueue,
        const std::shared_ptr<std::mutex>& frameQueueMtx
    );

    // 出力を1枚だけ取り出して即破棄（詰まり解消用）
    HRESULT TryDrainAndDiscard();

private:
    HRESULT ComputeRGBA8Desc(UINT w, UINT h, RGBA8Desc& out);

private:
    ComPtr<IMFTransform> decoder_;
    SamplePool           samplePool_;
    bool                 needAppSamples_ = true;
    UINT                 width_ = 0, height_ = 0;
    RGBA8Desc            rgba_{};
    // メンバにこれを追加
    ComPtr<IMFTransform> decoderMJPG_; // MJPG -> YUY2
    ComPtr<IMFTransform> colorConv_;   // YUY2 -> RGB32

};
