#pragma once
#include <d3d12.h>
#include <wrl/client.h>

#include "utils/Math/MathUtils.h"     // Matrix4x4 / Transform / Vector4 など
#include "graphics/RenderContext.h"
#include "graphics/Sprite.h"

class SpriteRenderer {
public:
    SpriteRenderer() = default;
    ~SpriteRenderer() = default;

    // 初期化
    //  - device: D3D12 デバイス
    //  - srvHeap: 共通 SRV ヒープ（すでに作っているもの）
    //  - srvDescSize: SRV インクリメントサイズ（device->GetDescriptorHandleIncrementSize）
    bool Initialize(
        ID3D12Device* device,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvDescSize
    );

    // Sprite 描画
    //  - rootSig / pso は既存の Sprite 用 RootSignature / PSO をそのまま渡す想定
    //    （= SpriteRenderer は「ジオメトリ＋CB」だけ面倒を見る）
    void DrawSprite(
        ge3::graphics::RenderContext& ctx,
        ID3D12GraphicsCommandList* cmdList,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const Sprite& sprite,
        const Matrix4x4& viewProj   // 2D なら正射影行列、3D なら通常の VP
    );

private:
    using ComPtr = Microsoft::WRL::ComPtr<ID3D12Resource>;

    // 内部で使う定数バッファのレイアウト
    struct SpriteCB {
        Matrix4x4 worldViewProj;
        Vector4   color;
    };

    bool CreateGeometry();       // 1枚の板ポリの VB/IB を作成
    bool CreateConstantBuffer(); // SpriteCB 用の CB を作成

private:
    // 所有しない
    ID3D12Device* device_ = nullptr;
    ID3D12DescriptorHeap* srvHeap_ = nullptr;
    UINT                  srvDescSize_ = 0;

    // ジオメトリ（四角形）
    ComPtr vertexBuffer_;
    ComPtr indexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vbv_{};
    D3D12_INDEX_BUFFER_VIEW  ibv_{};

    // 定数バッファ
    ComPtr cbResource_;
    SpriteCB* cbMapped_ = nullptr;
};
