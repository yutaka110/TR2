#include "graphics/RenderContext.h"

// DescriptorHeapSet の定義があるヘッダ
#include "core/DescriptorHeap.h"

// CD3DX12_HEAP_PROPERTIES 用
#include "../../../externals/DirectXTex/d3dx12.h"   // ← プロジェクトで実際に使っているパスに合わせて

#include <cassert>

using Microsoft::WRL::ComPtr;

namespace ge3::graphics
{
    ComPtr<ID3D12Resource> RenderContext::CreateDepthStencilTextureResource(
        std::uint32_t width,
        std::uint32_t height
    )
    {
        assert(device_);

        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Width = width;
        resourceDesc.Height = height;
        resourceDesc.MipLevels = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Alignment = 0;

        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;

        // ★ 一旦変数にする
        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

        ComPtr<ID3D12Resource> depthStencil;
        HRESULT hr = device_->CreateCommittedResource(
            &heapProps,                // ← これでOK
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            IID_PPV_ARGS(depthStencil.GetAddressOf())
        );
        assert(SUCCEEDED(hr));
        return depthStencil;
    }


    void RenderContext::BeginFrame(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* backBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv
    )
    {
        assert(cmdList);
        assert(backBuffer);

        // PRESENT → RENDER_TARGET
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = backBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmdList->ResourceBarrier(1, &barrier);

        // RTV / DSV
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsvHandle_);

        // Clear
        const float clearColor[4] = { 0.35f, 0.5f, 0.8f, 1.0f };
        cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        cmdList->ClearDepthStencilView(
            dsvHandle_,
            D3D12_CLEAR_FLAG_DEPTH,
            1.0f,
            0,
            0,
            nullptr
        );

        // ビューポート / シザー
        cmdList->RSSetViewports(1, &viewport_);
        cmdList->RSSetScissorRects(1, &scissorRect_);
    }

    void RenderContext::EndFrame(
        ID3D12GraphicsCommandList* cmdList,
        ID3D12Resource* backBuffer
    )
    {
        assert(cmdList);
        assert(backBuffer);

        // RENDER_TARGET → PRESENT
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = backBuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmdList->ResourceBarrier(1, &barrier);
    }

} // namespace ge3::graphics
