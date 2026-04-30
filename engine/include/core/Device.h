#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace core {

    class Device {
    public:
        // enableDebugLayer: 開発時は true（リリースでは false 推奨）
        bool Initialize(bool enableDebugLayer);

        // アクセサ
        IDXGIFactory6* GetFactory()        const { return factory_.Get(); }
        ID3D12Device* GetDevice()         const { return device_.Get(); }
        ID3D12CommandQueue* GetCommandQueue()   const { return queue_.Get(); }
        ID3D12Fence* GetFence()          const { return fence_.Get(); }
        UINT                      GetRTVDescriptorSize() const { return rtvIncSize_; }
        UINT                      GetCBVSRVUAVDescriptorSize() const { return cbvSrvUavIncSize_; }
        D3D_FEATURE_LEVEL         GetFeatureLevel()   const { return featureLevel_; }
        bool                      IsTearingSupported() const { return allowTearing_; }

        // 破棄（明示的に呼ぶ必要は通常なし）
        void Shutdown();

    private:
        bool CreateFactory(bool enableDebugLayer);
        bool PickAdapter(Microsoft::WRL::ComPtr<IDXGIAdapter4>& outAdapter);
        bool CreateDeviceAndQueue(IDXGIAdapter4* adapter, bool enableDebugLayer);
        bool CreateFence();
        void CacheDescriptorIncrements();
        void CheckTearingSupport();

    private:
        Microsoft::WRL::ComPtr<IDXGIFactory6>      factory_;
        Microsoft::WRL::ComPtr<ID3D12Device>       device_;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
        Microsoft::WRL::ComPtr<ID3D12Fence>        fence_;

        UINT rtvIncSize_ = 0;
        UINT cbvSrvUavIncSize_ = 0;
        D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_12_0;
        bool allowTearing_ = false;
    };

} // namespace core
