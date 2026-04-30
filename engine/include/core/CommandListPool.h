#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

namespace core {
    class Device;

    class CommandListPool {
    public:
        // frameCount はスワップチェーンのバッファ数に合わせる（例: 2）
        bool Initialize(Device& dev, UINT frameCount);

        // Begin: 指定フレームの Allocator を Reset し、リストを Reset して返す
        // pInitialPSO は任意（nullptr可）
        ID3D12GraphicsCommandList* Begin(UINT frameIndex, ID3D12PipelineState* pInitialPSO = nullptr);

        // EndAndExecute: Close→Execute（QueueはDeviceから取得）
        void EndAndExecute(Device& dev);

        // 必要なら外からAllocator参照
        ID3D12CommandAllocator* Allocator(UINT frameIndex) { return allocators_[frameIndex].Get(); }

    private:
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
        std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> allocators_;
        UINT frameCount_ = 0;
        UINT currentFrame_ = 0;
    };

}
