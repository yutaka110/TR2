#include "core/DescriptorHeap.h"

using namespace ge3::core;

D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeap::ToNative(HeapKind k) {
    switch (k) {
    case HeapKind::RTV:         return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    case HeapKind::DSV:         return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    case HeapKind::CBV_SRV_UAV: return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    }
    return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
}

void DescriptorHeap::Initialize(ID3D12Device* device, HeapKind kind, uint32_t capacity, bool shaderVisible) {
    assert(device);
    capacity_ = capacity;
    shaderVisible_ = (kind == HeapKind::CBV_SRV_UAV) ? shaderVisible : false;
    type_ = ToNative(kind);

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = type_;
    desc.NumDescriptors = capacity_;
    desc.Flags = shaderVisible_ ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
        : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap_));
    assert(SUCCEEDED(hr));

    incrementSize_ = device->GetDescriptorHandleIncrementSize(type_);
    cpuStart_ = heap_->GetCPUDescriptorHandleForHeapStart();
    gpuStart_.ptr = shaderVisible_ ? heap_->GetGPUDescriptorHandleForHeapStart().ptr : 0;

    while (!freeList_.empty()) freeList_.pop();
    nextAllocate_ = 0;
}

void DescriptorHeap::Finalize() {
    while (!freeList_.empty()) freeList_.pop();
    heap_.Reset();
    capacity_ = 0;
    incrementSize_ = 0;
    shaderVisible_ = false;
    cpuStart_.ptr = 0;
    gpuStart_.ptr = 0;
    nextAllocate_ = 0;
}

DescriptorHandle DescriptorHeap::Allocate() {
    uint32_t index;
    if (!freeList_.empty()) { index = freeList_.front(); freeList_.pop(); }
    else { index = nextAllocate_++; assert(index < capacity_ && "DescriptorHeap exhausted"); }

    DescriptorHandle h;
    h.index = index;
    h.cpu = { cpuStart_.ptr + SIZE_T(index) * SIZE_T(incrementSize_) };
    h.gpu.ptr = shaderVisible_ ? (gpuStart_.ptr + UINT64(index) * UINT64(incrementSize_)) : 0;
    return h;
}

void DescriptorHeap::Reserve(uint32_t count) {
    assert(count <= capacity_);
    if (nextAllocate_ < count) {
        nextAllocate_ = count;
    }
}

void DescriptorHeap::Free(uint32_t index) {
    assert(index < capacity_);
    freeList_.push(index);
}

DescriptorHandle DescriptorHeap::GetHandle(uint32_t index) const {
    assert(index < capacity_);
    DescriptorHandle h;
    h.index = index;
    h.cpu = { cpuStart_.ptr + SIZE_T(index) * SIZE_T(incrementSize_) };
    h.gpu.ptr = shaderVisible_ ? (gpuStart_.ptr + UINT64(index) * UINT64(incrementSize_)) : 0;
    return h;
}
