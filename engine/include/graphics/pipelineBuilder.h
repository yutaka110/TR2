#pragma once
#include <d3d12.h>
#include <wrl/client.h>

namespace ge3::graphics {

    using Microsoft::WRL::ComPtr;

    // RootSignature と PSO を作るだけの薄いユーティリティ
    class PipelineBuilder {
    public:
        // RootSignature を作成（失敗時は nullptr）
        static ComPtr<ID3D12RootSignature> CreateRootSignature(
            ID3D12Device* device,
            const D3D12_ROOT_SIGNATURE_DESC& desc);

        // GraphicsPipelineState を作成（失敗時は nullptr）
        static ComPtr<ID3D12PipelineState> CreateGraphicsPipelineState(
            ID3D12Device* device,
            const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);
    };

} // namespace ge3::graphics
