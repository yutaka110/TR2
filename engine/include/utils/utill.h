#pragma once
#include <d3d12.h>
#include <wrl.h>

inline D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle(
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap,
    UINT stride, UINT index);


inline D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle(
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap,
    UINT stride, UINT index);
