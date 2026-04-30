#pragma once

#include <cstdint>
#include <d3d12.h>

#include "graphics/MaterialBinding.h"

class AppFrameRenderer {
public:
    void BeginFrame(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* backBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        const float clearColor[4]) const;

    bool PrepareMainPass(
        ID3D12GraphicsCommandList* commandList,
        const D3D12_VIEWPORT& viewport,
        const D3D12_RECT& scissorRect,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState) const;

    void DrawMainModel(
        ID3D12GraphicsCommandList* commandList,
        const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView,
        D3D12_GPU_VIRTUAL_ADDRESS materialBufferAddress,
        D3D12_GPU_VIRTUAL_ADDRESS transformBufferAddress,
        D3D12_GPU_DESCRIPTOR_HANDLE textureHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE receivedTextureHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE motionMaskTextureHandle,
        D3D12_GPU_VIRTUAL_ADDRESS directionalLightBufferAddress,
        D3D12_GPU_VIRTUAL_ADDRESS cameraBufferAddress,
        D3D12_GPU_VIRTUAL_ADDRESS pointLightBufferAddress,
        D3D12_GPU_VIRTUAL_ADDRESS spotLightBufferAddress,
        uint32_t vertexCount) const;

    void ApplyMaterialInstance(
        ID3D12GraphicsCommandList* commandList,
        const ge3::graphics::MaterialInstance& material) const;

    void DrawSprite(
        ID3D12GraphicsCommandList* commandList,
        ID3D12DescriptorHeap* descriptorHeap,
        const D3D12_INDEX_BUFFER_VIEW& indexBufferView,
        const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView,
        D3D12_GPU_VIRTUAL_ADDRESS materialBufferAddress,
        D3D12_GPU_VIRTUAL_ADDRESS transformBufferAddress,
        D3D12_GPU_DESCRIPTOR_HANDLE textureHandle) const;

    void PrepareSphere(
        ID3D12GraphicsCommandList* commandList,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        D3D12_GPU_VIRTUAL_ADDRESS cameraBufferAddress,
        const D3D12_VERTEX_BUFFER_VIEW& sphereVertexBufferView) const;

    void EndFrame(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* backBuffer) const;
};
