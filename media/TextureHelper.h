#pragma once
#include <d3d12.h>
#include <wrl/client.h>
using namespace Microsoft::WRL;

ComPtr<ID3D12Resource> CreateTextureResourceResolution(ComPtr<ID3D12Device> device, UINT width, UINT height, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM);
//ComPtr<ID3D12Resource> CreateUploadHeap(ComPtr<ID3D12Device> device, UINT64 uploadBufferSize);
void CreateTextureSRV(ComPtr<ID3D12Device> device, ComPtr<ID3D12Resource> texture, D3D12_CPU_DESCRIPTOR_HANDLE srvHandle);
ComPtr<ID3D12Resource> CreateTextureWithUAV(ComPtr<ID3D12Device> device, UINT width, UINT height, DXGI_FORMAT format);