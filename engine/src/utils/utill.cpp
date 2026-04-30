
#include <d3d12.h>
#include <wrl.h>
#include "utils/utill.h"



inline D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle(
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap,
    UINT stride, UINT index)
{
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(stride) * index;
    return h;
}

inline D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle(
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap,
    UINT stride, UINT index)
{
    auto h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(stride) * index;
    return h;
}
