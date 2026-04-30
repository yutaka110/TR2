#pragma once

#include <string>

#include "../../externals/DirectXTex/DirectXTex.h"
#include <d3d12.h>
#include <wrl.h>

class AppRenderResources {
public:
    static Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        D3D12_DESCRIPTOR_HEAP_TYPE heapType,
        UINT numDescriptors,
        bool shaderVisible);

    static DirectX::ScratchImage LoadTexture(const std::string& filePath);

    static Microsoft::WRL::ComPtr<ID3D12Resource> CreateTextureResource(
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        const DirectX::TexMetadata& metadata);

    static void UploadTextureData(
        Microsoft::WRL::ComPtr<ID3D12Resource> texture,
        const DirectX::ScratchImage& mipImages);

    static D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandle(
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap,
        uint32_t descriptorSize,
        uint32_t index);

    static D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle(
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap,
        uint32_t descriptorSize,
        uint32_t index);

    bool InitializeParticleQuad(Microsoft::WRL::ComPtr<ID3D12Device> device);

    const D3D12_VERTEX_BUFFER_VIEW& ParticleVertexBufferView() const;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> particleVertexResource_;
    D3D12_VERTEX_BUFFER_VIEW particleVertexBufferView_{};
};
