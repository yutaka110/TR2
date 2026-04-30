#pragma once
#include <d3d12.h>
#include <wrl/client.h>

// あなたのプロジェクトのヘッダ群（型名は main.cpp に合わせている）
#include "graphics/RenderContext.h"   // ge3::graphics::RenderContext
#include "utils/Math/MathUtils.h"          // Matrix4x4, MakeIdentity4x4, Multiply, etc.
#include "utils/Math/Vector.h"             // Vector3, Vector4
#include "Camera/DebugCamera.h"      // DebugCamera
//#include "TextureHelper.h"           // CreateTextureResourceResolution, CreateTextureSRV
#include "utils/dx12/BufferHelper.h"            // CreateBufferResource（なければ後で差し替え）
#include "core/ShaderCompiler.h"
using Microsoft::WRL::ComPtr;


//// HLSL のコンパイルで使っている関数（main.cpp に実装があるはず）
////struct IDxcUtils;
////struct IDxcCompiler;
////struct IDxcIncludeHandler;
////IDxcBlob* CompileShader(
////    const std::wstring& filePath,
////    const wchar_t* profile,
////    IDxcUtils* dxcUtils,
////    IDxcCompiler* dxcCompiler,
////    IDxcIncludeHandler* includeHandler);

struct DirectionalLight;  // 既存の struct を前方宣言

// シンプルな Debug 用描画クラス。
// ・Object3D.VS/PS.hlsl を使った1枚ポリゴン描画
// ・ライト / WVP / マテリアルは main.cpp と同じレイアウトを想定
class DebugSceneRenderer {
public:
    DebugSceneRenderer() = default;
    ~DebugSceneRenderer() = default;

    // 初期化：
    //  - device: D3D12 デバイス
    //  - srvHeap: 共通 SRV ヒープ
    //  - srvDescriptorSize: SRV インクリメントサイズ
    //  - dxcUtils / dxcCompiler / includeHandler: 既存の DXC オブジェクト
    bool Initialize(
        ID3D12Device* device,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvDescriptorSize,
        IDxcUtils* dxcUtils,
        IDxcCompiler* dxcCompiler,
        IDxcIncludeHandler* includeHandler
    );

    // 毎フレーム更新（カメラ、行列、ライトなど）
    void Update(float deltaTime);

    // 毎フレーム描画
    void Render(
        ge3::graphics::RenderContext& ctx,
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* backBuffer,
        UINT backBufferIndex
    );

private:

    // 内部ヘルパー
    bool CreateRootSignatureAndPSO(
       
    );
    bool CreateSpriteGeometry();
    bool CreateConstantBuffers();
    bool LoadTextureUVChecker();

private:
    // 共有リソース（所有しない）
    ID3D12Device* device_ = nullptr;
    ID3D12DescriptorHeap* srvHeap_ = nullptr;
    UINT                  srvDescSize_ = 0;

    UINT                  clientWidth_ = 0;
    UINT                  clientHeight_ = 0;

    // RootSignature / PSO
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> graphicsPipelineState_;

    // Sprite 用 VB/IB
    ComPtr<ID3D12Resource> vertexResourceSprite_;
    ComPtr<ID3D12Resource> indexResourceSprite_;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferViewSprite_{};
    D3D12_INDEX_BUFFER_VIEW  indexBufferViewSprite_{};

    // CBuffer
    // これらの struct / 型配置は「main.cpp で使っていたもの」と合わせてね
    ComPtr<ID3D12Resource> materialResourceSprite_;
    ComPtr<ID3D12Resource> wvpResource_;
    ComPtr<ID3D12Resource> directionalLightResource_;

    Material* materialDataSprite_ = nullptr;
    Matrix4x4* wvpData_ = nullptr;
    DirectionalLight* directionalLightData_ = nullptr;

    // SRV (t0) : uvChecker
    D3D12_CPU_DESCRIPTOR_HANDLE textureSrvCPU_{};
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvGPU_{};

    // カメラ / Transform
    DebugCamera debugCamera_;
    Transform   worldTransform_;      // 1枚ポリゴンのワールド
    Transform   uvTransformSprite_;   // マテリアル側 UV 行列

    // ライトパラメータ
    DirectionalLight directionalLightInit_{};

    ge3::core::ShaderCompiler shaderCompiler_;
};
