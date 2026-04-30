#pragma once

#include <Windows.h>
#include <cstdint>
#include <wrl/client.h>

#include "core/Device.h"
#include "graphics/SwapChain.h"
#include "core/CommandListPool.h"
#include "core/DescriptorHeap.h"

// AppMain から「DX12基盤」を切り出すための集約クラス。
// まずは AppMain.cpp の初期化ブロックを移して AppMain を薄くする。
// BeginFrame/EndFrame の完全分離は次の段階でOK。
class EngineContext {
public:
    bool Initialize(HWND hwnd, UINT width, UINT height, bool enableDebugLayer = true);
    void Shutdown();

    core::Device& GetDevice() { return dev_; }
    graphics::SwapChain& GetSwapChain() { return swapChain_; }
    core::CommandListPool& GetCommandListPool() { return clPool_; }
    ge3::core::DescriptorHeapSet& GetHeaps() { return heaps_; }

    ID3D12DescriptorHeap* GetSrvHeap() const { return heaps_.srv.GetHeap(); }
    ID3D12Resource* GetDepthStencil() const { return depthStencil_.Get(); }
    D3D12_GPU_DESCRIPTOR_HANDLE GetDepthSrvGpuHandle() const { return depthSrv_.gpu; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetDepthSrvCpuHandle() const { return depthSrv_.cpu; }
    uint32_t GetMainDsvIndex() const { return mainDsvIndex_; }
    uint32_t GetReadOnlyDsvIndex() const { return readOnlyDsvIndex_; }

    ID3D12CommandAllocator* GetMainCommandAllocator() const { return mainCmdAlloc_.Get(); }
    ID3D12GraphicsCommandList* GetMainCommandList() const { return mainCmdList_.Get(); }

    HANDLE GetFenceEvent() const { return fenceEvent_; }
    uint64_t GetFenceValue() const { return fenceValue_; }
    void SetFenceValue(uint64_t v) { fenceValue_ = v; }

private:
    core::Device dev_{};
    graphics::SwapChain swapChain_{};
    core::CommandListPool clPool_{};
    ge3::core::DescriptorHeapSet heaps_{};

    Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil_;
    ge3::core::DescriptorHandle depthSrv_{};
    uint32_t mainDsvIndex_ = UINT32_MAX;
    uint32_t readOnlyDsvIndex_ = UINT32_MAX;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> mainCmdAlloc_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> mainCmdList_;

    HANDLE fenceEvent_ = nullptr;
    uint64_t fenceValue_ = 0;
};
