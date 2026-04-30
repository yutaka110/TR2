#pragma once
#pragma once
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vector>
#include <Windows.h>

namespace core { class Device; }

namespace graphics {

    class SwapChain {
    public:
        bool Create(core::Device& dev, HWND hwnd, UINT width, UINT height, UINT bufferCount = 2);
        void Resize(core::Device& dev, UINT width, UINT height);
        void Present(core::Device& dev, UINT syncInterval = 1);

        UINT CurrentIndex() const { return swapChain_->GetCurrentBackBufferIndex(); }
        UINT BufferCount()  const { return static_cast<UINT>(backBuffers_.size()); }

        ID3D12Resource* BackBuffer(UINT i) const { return backBuffers_[i].Get(); }
        D3D12_CPU_DESCRIPTOR_HANDLE   RTV(UINT i)        const { return rtvHandles_[i]; }
        ID3D12DescriptorHeap* RtvHeap()          const { return rtvHeap_.Get(); }

    private:
        void CreateRTVs(core::Device& dev);
        void ReleaseBuffers();

    private:
        Microsoft::WRL::ComPtr<IDXGISwapChain3>          swapChain_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>     rtvHeap_;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> backBuffers_;
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>         rtvHandles_;
        DXGI_FORMAT format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
        UINT bufferCount_ = 2;
        bool allowTearing_ = false;
    };

} // namespace graphics
