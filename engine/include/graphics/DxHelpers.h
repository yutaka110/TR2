#pragma once
#include <cstdint>
#include <string>

#include "wrl.h"
#include <d3d12.h>

#include "../../../externals/DirectXTex/DirectXTex.h"

using Microsoft::WRL::ComPtr;

namespace dxhelpers {

DirectX::ScratchImage LoadTexture(const std::string& filePath);

ComPtr<ID3D12Resource> CreateTextureResource(ComPtr<ID3D12Device> device, const DirectX::TexMetadata& metadata);
ComPtr<ID3D12Resource> CreateBufferResource(ComPtr<ID3D12Device> device, size_t sizeInBytes);
void UploadTextureData(ComPtr<ID3D12Resource> texture, const DirectX::ScratchImage& mipImages);

ComPtr<ID3D12Resource> CreateDepthStencilTextureResource(ComPtr<ID3D12Device> device, int32_t width, int32_t height);

ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(ComPtr<ID3D12Device> device, D3D12_DESCRIPTOR_HEAP_TYPE heapType, UINT numDescriptors, bool shaderVisible);

D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandle(ComPtr<ID3D12DescriptorHeap> descriptorHeap, UINT descriptorSize, UINT index);
D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle(ComPtr<ID3D12DescriptorHeap> descriptorHeap, UINT descriptorSize, UINT index);

} // namespace dxhelpers
