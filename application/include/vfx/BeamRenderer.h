#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <DirectXMath.h>
#include "core/ShaderCompiler.h"

struct BeamRenderInput;
struct VfxRenderContext;

class BeamRenderer
{
public:
    BeamRenderer() = default;
    ~BeamRenderer();

    // 初期化
    //  - device           : D3D12 デバイス
    //  - srvHeap          : ビーム用の SRV を置く DescriptorHeap（シェーダ可視 SRV）
    //  - srvDescriptorSize: そのヘープの 1ディスクリプタあたりのサイズ
    //  - rampCpuHandle    : rampTex の元 CPUハンドル（TextureManager などから取得）
    //  - noiseCpuHandle   : noiseTex の元 CPUハンドル（STEP1では未使用でもOK）
    //  - rtvFormat        : メインの RTV フォーマット（DXGI_FORMAT_R8G8B8A8_UNORM 等）
    //  - dsvFormat        : メインの DSV フォーマット（DXGI_FORMAT_D24_UNORM_S8_UINT 等）
    void Initialize(
        ID3D12Device* device,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvDescriptorSize,
        D3D12_CPU_DESCRIPTOR_HANDLE rampCpuHandle,
        D3D12_CPU_DESCRIPTOR_HANDLE noiseCpuHandle,
        DXGI_FORMAT rtvFormat,
        DXGI_FORMAT dsvFormat
    );

    // テスト用：1本だけビームを描画する
    // worldViewProj : W*V*P 行列
    // baseColor     : ビームの色
    // intensity     : 明るさ
    void DrawTest(
        ID3D12GraphicsCommandList* cmdList,
        const DirectX::XMMATRIX& worldViewProj,
        const DirectX::XMFLOAT4& baseColor,
        float intensity
    );

    void Draw(
        ID3D12GraphicsCommandList* cmdList,
        const BeamRenderInput& input,
        const VfxRenderContext& context);

    void SetTime(float t);
    void Shutdown();

private:

    ge3::core::ShaderCompiler shaderCompiler_;

    struct BeamVSConstants
    {
        DirectX::XMFLOAT4X4 worldViewProj;
    };
    struct BeamPSConstants
    {
        DirectX::XMFLOAT4   baseColor;
        float               intensity;
        float               time;      // STEP2 で使う用に確保
        float               pad[2];
    };

    // RootSig / PSO
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;

    // ビーム用板ポリ (四角形) の VB / IB
    Microsoft::WRL::ComPtr<ID3D12Resource> vb_;
    Microsoft::WRL::ComPtr<ID3D12Resource> ib_;
    D3D12_VERTEX_BUFFER_VIEW vbView_{};
    D3D12_INDEX_BUFFER_VIEW  ibView_{};
    UINT indexCount_ = 0;

    // 定数バッファ（UploadHeap）
    Microsoft::WRL::ComPtr<ID3D12Resource> vsCb_;
    Microsoft::WRL::ComPtr<ID3D12Resource> psCb_;
    BeamVSConstants* vsCbMapped_ = nullptr;
    BeamPSConstants* psCbMapped_ = nullptr;

    // SRVテーブル(t0=tampTex, t1=noiseTex)の GPU ハンドル
    D3D12_GPU_DESCRIPTOR_HANDLE srvTableGpuStart_{};
};
