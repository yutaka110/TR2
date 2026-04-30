#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <queue>
#include <cassert>

namespace ge3::core {

    using Microsoft::WRL::ComPtr;

    enum class HeapKind { RTV, DSV, CBV_SRV_UAV };

    struct DescriptorHandle {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{}; // RTV/DSVでは.ptr==0
        uint32_t index = UINT32_MAX;
        bool IsValid() const { return index != UINT32_MAX; }
    };

    class DescriptorHeap {
    public:
        void Initialize(ID3D12Device* device, HeapKind kind, uint32_t capacity, bool shaderVisible = false);
        void Finalize();

        DescriptorHandle Allocate();
        void Reserve(uint32_t count);
        void Free(uint32_t index);
        DescriptorHandle GetHandle(uint32_t index) const;

        ID3D12DescriptorHeap* GetHeap() const { return heap_.Get(); }
        D3D12_DESCRIPTOR_HEAP_TYPE GetNativeType() const { return type_; }
        uint32_t GetCapacity() const { return capacity_; }
        uint32_t GetIncrement() const { return incrementSize_; }

    private:
        static D3D12_DESCRIPTOR_HEAP_TYPE ToNative(HeapKind k);

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
        D3D12_DESCRIPTOR_HEAP_TYPE type_{};
        uint32_t capacity_ = 0;
        uint32_t incrementSize_ = 0;
        bool shaderVisible_ = false;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart_{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart_{};

        std::queue<uint32_t> freeList_;
        uint32_t nextAllocate_ = 0;
    };

    struct DescriptorHeapSet {
        DescriptorHeap rtv;       // shaderVisible=false
        DescriptorHeap dsv;       // shaderVisible=false
        DescriptorHeap srv;       // shaderVisible=true

        void Initialize(ID3D12Device* device, uint32_t rtvCount, uint32_t dsvCount, uint32_t srvCount) {
            rtv.Initialize(device, HeapKind::RTV, rtvCount, false);
            dsv.Initialize(device, HeapKind::DSV, dsvCount, false);
            srv.Initialize(device, HeapKind::CBV_SRV_UAV, srvCount, true);
        }
        void Finalize() { rtv.Finalize(); dsv.Finalize(); srv.Finalize(); }
    };

} // namespace ge3::core
