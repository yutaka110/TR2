#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace ge3 {
    namespace core {
        // 実体は struct のはずなので struct で前方宣言しておく
        struct DescriptorHeapSet;
    }

    namespace graphics {

        class RenderContext {
        public:
            RenderContext() = default;
            ~RenderContext() = default;

            // ★ SwapChain は渡さない設計に変更
            bool Initialize(
                ID3D12Device* device,
                core::DescriptorHeapSet* heaps,
                std::uint32_t width,
                std::uint32_t height
            );

            // ★ backBuffer と RTV を外から渡してもらう
            void BeginFrame(
                ID3D12GraphicsCommandList* cmdList,
                ID3D12Resource* backBuffer,
                D3D12_CPU_DESCRIPTOR_HANDLE rtv
            );

            void EndFrame(
                ID3D12GraphicsCommandList* cmdList,
                ID3D12Resource* backBuffer
            );

            D3D12_VIEWPORT  GetViewport()    const { return viewport_; }
            D3D12_RECT      GetScissorRect() const { return scissorRect_; }
            D3D12_CPU_DESCRIPTOR_HANDLE GetDsvHandle() const { return dsvHandle_; }

        private:
            Microsoft::WRL::ComPtr<ID3D12Resource> CreateDepthStencilTextureResource(
                std::uint32_t width,
                std::uint32_t height
            );

        private:
            ID3D12Device* device_ = nullptr;   // 所有しない
            core::DescriptorHeapSet* heaps_ = nullptr;   // 所有しない

            Microsoft::WRL::ComPtr<ID3D12Resource> depthStencil_;
            D3D12_CPU_DESCRIPTOR_HANDLE  dsvHandle_{};
            std::uint32_t                dsvIndex_ = UINT32_MAX;

            D3D12_VIEWPORT  viewport_{};
            D3D12_RECT      scissorRect_{};
        };

    } // namespace graphics
} // namespace ge3
