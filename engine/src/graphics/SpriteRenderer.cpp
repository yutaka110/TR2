#include "graphics/SpriteRenderer.h"

#include <cassert>
#include <cmath>

// あなたの環境の CreateBufferResource に合わせて include
#include "utils/dx12/BufferHelper.h"   // CreateBufferResource(device, size) 的なヘルパーに合わせて

using Microsoft::WRL::ComPtr;

bool SpriteRenderer::Initialize(
    ID3D12Device* device,
    ID3D12DescriptorHeap* srvHeap,
    UINT srvDescSize
) {
    device_ = device;
    srvHeap_ = srvHeap;
    srvDescSize_ = srvDescSize;

    assert(device_);
    assert(srvHeap_);

    if (!CreateGeometry()) {
        return false;
    }
    if (!CreateConstantBuffer()) {
        return false;
    }
    return true;
}

// 四角形ジオメトリ作成（-0.5〜0.5 の板ポリ）
bool SpriteRenderer::CreateGeometry() {
    struct Vertex {
        Vector4 position;
        Vector2 texcoord;
    };

    // 頂点 4, インデックス 6 の矩形
    constexpr UINT kVertexCount = 4;
    constexpr UINT kIndexCount = 6;

    vertexBuffer_ = CreateBufferResource(device_, sizeof(Vertex) * kVertexCount);
    indexBuffer_ = CreateBufferResource(device_, sizeof(uint32_t) * kIndexCount);
    if (!vertexBuffer_ || !indexBuffer_) {
        return false;
    }

    // VBV
    vbv_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
    vbv_.SizeInBytes = sizeof(Vertex) * kVertexCount;
    vbv_.StrideInBytes = sizeof(Vertex);

    // IBV
    ibv_.BufferLocation = indexBuffer_->GetGPUVirtualAddress();
    ibv_.SizeInBytes = sizeof(uint32_t) * kIndexCount;
    ibv_.Format = DXGI_FORMAT_R32_UINT;

    // 中身を書き込み
    Vertex* vtx = nullptr;
    vertexBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&vtx));

    // 中心 0,0 に 1x1 の四角形（Y+ 上、X+ 右）
    vtx[0].position = { -0.5f, -0.5f, 0.0f, 1.0f }; // 左下
    vtx[0].texcoord = { 0.0f, 1.0f };

    vtx[1].position = { -0.5f,  0.5f, 0.0f, 1.0f }; // 左上
    vtx[1].texcoord = { 0.0f, 0.0f };

    vtx[2].position = { 0.5f, -0.5f, 0.0f, 1.0f }; // 右下
    vtx[2].texcoord = { 1.0f, 1.0f };

    vtx[3].position = { 0.5f,  0.5f, 0.0f, 1.0f }; // 右上
    vtx[3].texcoord = { 1.0f, 0.0f };

    uint32_t* idx = nullptr;
    indexBuffer_->Map(0, nullptr, reinterpret_cast<void**>(&idx));

    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 1; idx[4] = 3; idx[5] = 2;

    return true;
}

// CB 作成
bool SpriteRenderer::CreateConstantBuffer() {
    cbResource_ = CreateBufferResource(device_, sizeof(SpriteCB));
    if (!cbResource_) {
        return false;
    }
    cbResource_->Map(0, nullptr, reinterpret_cast<void**>(&cbMapped_));

    // 初期値
    cbMapped_->worldViewProj = MakeIdentity4x4();
    cbMapped_->color = { 1, 1, 1, 1 };
    return true;
}

// Sprite 1枚の描画
void SpriteRenderer::DrawSprite(
    ge3::graphics::RenderContext& ctx,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pipelineState,
    const Sprite& sprite,
    const Matrix4x4& viewProj
) {
    assert(cmdList);
    assert(rootSignature);
    assert(pipelineState);

    // RootSig / PSO セット
    cmdList->SetGraphicsRootSignature(rootSignature);
    cmdList->SetPipelineState(pipelineState);

    // 頂点 / インデックスバッファ
    cmdList->IASetVertexBuffers(0, 1, &vbv_);
    cmdList->IASetIndexBuffer(&ibv_);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Sprite の Transform から World 行列を作成
    Matrix4x4 world =
        MakeAffineMatrix(sprite.transform.scale, sprite.transform.rotate, sprite.transform.translate);

    // WVP = world * viewProj
    cbMapped_->worldViewProj = Multiply(world, viewProj);
    cbMapped_->color = sprite.color;

    // CBV & SRV セット
    // ここは RootSignature のレイアウトに合わせてスロットを変えてね
    cmdList->SetGraphicsRootConstantBufferView(
        0, cbResource_->GetGPUVirtualAddress()); // 例: b0 に SpriteCB

    // SRV ヒープをセット
    ID3D12DescriptorHeap* heaps[] = { srvHeap_ };
    cmdList->SetDescriptorHeaps(1, heaps);

    // 例: t0 に Sprite テクスチャ
    cmdList->SetGraphicsRootDescriptorTable(1, sprite.textureGpuHandle);

    // 描画
    cmdList->DrawIndexedInstanced(6, 1, 0, 0, 0);
}
