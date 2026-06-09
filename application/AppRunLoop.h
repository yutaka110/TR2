#pragma once

#include <Windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

#include "camera/debugCamera.h"
#include "core/CommandListPool.h"
#include "core/DescriptorHeap.h"
#include "core/Device.h"
#include "EffectSystem.h"
#include "EffectAssetLoader.h"
#include "EffectRuntime.h"
#include "AppFrameState.h"
#include "AppFrameGraphBuilder.h"
#include "AppGpuParticleSystem.h"
#include "AppVfxRenderTargets.h"
#include "graphics/RenderGraph.h"
#include "graphics/SwapChain.h"
#include "PostProcessStack.h"
#include "resources/ResourceRegistry.h"
#include "utils/math/MathUtils.h"
#include "vfx/AppVfxRendererSet.h"
#include "vfx/BeamRenderer.h"
#include "vfx/DistortionRenderer.h"
#include "vfx/ParticleRenderer.h"
#include "vfx/TrailRenderer.h"
#include "../network/NetworkStats.h"
#include "../network/NetworkVideoReceiver.h"
#include "../network/NetworkConditionSimulator.h"

class AppFrameRenderer;
class AppImGuiLayer;
class AppPipelines;
class AppParticleSystem;
class AppRenderResources;
struct AppRuntimeState;
class AppSceneResources;
class EngineContext;

class AppRunLoop {
public:
    AppRunLoop(
        DebugCamera& debugCamera,
        AppRuntimeState& runtimeState,
        AppSceneResources& scene,
        AppParticleSystem& particleSystem,
        AppImGuiLayer& imguiLayer,
        AppFrameRenderer& frameRenderer,
        AppPipelines& appPipelines,
        AppRenderResources& renderResources,
        graphics::SwapChain& swapChain,
        core::CommandListPool& clPool,
        EngineContext& engineContext,
        ge3::core::DescriptorHeapSet& heaps,
        core::Device& dev,
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvDescriptorHeap,
        Matrix4x4* wvpData,
        uint32_t windowWidth,
        uint32_t windowHeight,
        FrameLoopState& frameState,
        ID3D12CommandQueue* commandQueue,
        ID3D12Fence* fence,
        HANDLE fenceEvent);

    void InitializeBeam(
        ID3D12Device* device,
        ID3D12DescriptorHeap* srvDescriptorHeap,
        uint32_t descriptorSizeSRV,
        DXGI_FORMAT rtvFormat,
        DXGI_FORMAT dsvFormat);
    void UpdateFrame();
    void RenderFrame();
    void Shutdown();
    void SetNetworkStatsProvider(std::function<net::NetworkStatsSnapshot()> provider);
    void SetJitterBufferTargetDelaySetter(std::function<void(uint32_t)> setter);
    void SetJitterBufferAutoModeSetter(std::function<void(bool)> setter);
    void SetNetworkConditionSetter(std::function<void(const net::NetworkCondition&)> setter);
    void SetAdaptiveControlModeSetter(std::function<void(int)> setter);
    void SetCongestionControlModeSetter(std::function<void(int)> setter);

    void SetReceivedFrameProvider(std::function<bool(net::DecodedVideoFrame&)> provider);
    void SetNetworkFrameDisplayNotifier(std::function<void()> notifier);

    void SetReceivedVideoTexture(
        Microsoft::WRL::ComPtr<ID3D12Resource> texture,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadBuffers,
        D3D12_GPU_DESCRIPTOR_HANDLE uavGpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle,
        uint32_t width,
        uint32_t height);
    void SetReceivedVideoNv12Textures(
        Microsoft::WRL::ComPtr<ID3D12Resource> yTexture,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> yUploadBuffers,
        D3D12_GPU_DESCRIPTOR_HANDLE ySrvGpuHandle,
        Microsoft::WRL::ComPtr<ID3D12Resource> uvTexture,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uvUploadBuffers,
        D3D12_GPU_DESCRIPTOR_HANDLE uvSrvGpuHandle);
    void PopulateNetworkRenderTimings(net::NetworkStatsSnapshot& stats) const;
private:
    void BeginFrameSystems();
    void WaitForFrameResource(UINT frameIndex);
    void SignalFrameResource(UINT frameIndex);
    void FlushGpu();
    void WaitForReceivedVideoUploadBuffer(UINT uploadBufferIndex);
    void UploadReceivedVideoFrame(
        ID3D12GraphicsCommandList* commandList,
        UINT frameIndex);

    DebugCamera& debugCamera_;
    AppRuntimeState& runtimeState_;
    AppSceneResources& scene_;
    AppParticleSystem& particleSystem_;
    AppImGuiLayer& imguiLayer_;
    AppFrameRenderer& frameRenderer_;
    AppPipelines& appPipelines_;
    AppRenderResources& renderResources_;
    graphics::SwapChain& swapChain_;
    core::CommandListPool& clPool_;
    EngineContext& engineContext_;
    ge3::core::DescriptorHeapSet& heaps_;
    core::Device& dev_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvDescriptorHeap_;
    Matrix4x4* wvpData_;
    uint32_t windowWidth_;
    uint32_t windowHeight_;
    FrameLoopState& frameState_;
    ID3D12CommandQueue* commandQueue_;
    ID3D12Fence* fence_;
    HANDLE fenceEvent_;
    BeamRenderer beam_;
    ParticleRenderer particleRenderer_;
    TrailRenderer trailRenderer_;
    DistortionRenderer distortionRenderer_;
    AppVfxRendererSet vfxRenderers_{&particleRenderer_, &trailRenderer_, &beam_, &distortionRenderer_};
    EffectSystem effectSystem_;
    EffectRuntime effectRuntime_;
    EffectAssetLoader effectAssetLoader_;
    std::vector<LoadedEffectAsset> loadedEffectAssets_;
    PostProcessStack postProcessStack_;
    AppVfxRenderTargets vfxRenderTargets_;
    AppGpuParticleSystem gpuParticleSystem_;
    AppFrameGraphBuilder frameGraphBuilder_;
    ge3::graphics::RenderGraph renderGraph_;
    ge3::resources::ResourceRegistry resourceRegistry_;
    ge3::resources::EffectResourceCache effectResourceCache_;
    ge3::resources::FrameTransientAllocator frameTransientAllocator_;
    std::string lastRenderGraphDescription_;
    std::string lastRenderGraphError_;
    std::vector<ge3::graphics::RenderPassDebugInfo> lastRenderPassDebugInfo_;
    uint32_t lastTransientTargetCount_ = 0;
    uint32_t lastTransientTargetStorageCount_ = 0;
    uint32_t lastTransientBufferCount_ = 0;
    uint32_t lastTransientBufferStorageCount_ = 0;
    D3D12_RESOURCE_STATES sceneDepthState_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    float beamTime_ = 0.0f;
    bool uiToggleKeyWasDown_ = false;

    std::function<net::NetworkStatsSnapshot()> networkStatsProvider_;
    std::function<void(uint32_t)> jitterBufferTargetDelaySetter_;
    std::function<void(bool)> jitterBufferAutoModeSetter_;
    std::function<void(const net::NetworkCondition&)> networkConditionSetter_;
    std::function<void(int)> adaptiveControlModeSetter_;
    std::function<void(int)> congestionControlModeSetter_;

    std::function<bool(net::DecodedVideoFrame&)> receivedFrameProvider_;
    std::function<void()> networkFrameDisplayNotifier_;

    Microsoft::WRL::ComPtr<ID3D12Resource> receivedVideoTexture_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> receivedVideoUploadBuffers_;
    Microsoft::WRL::ComPtr<ID3D12Resource> receivedVideoNv12YTexture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> receivedVideoNv12UVTexture_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> receivedVideoNv12YUploadBuffers_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> receivedVideoNv12UVUploadBuffers_;
    D3D12_GPU_DESCRIPTOR_HANDLE receivedVideoUavGpuHandle_{};
    D3D12_GPU_DESCRIPTOR_HANDLE receivedVideoSrvGpuHandle_{};
    D3D12_GPU_DESCRIPTOR_HANDLE receivedVideoNv12YSrvGpuHandle_{};
    D3D12_GPU_DESCRIPTOR_HANDLE receivedVideoNv12UVSrvGpuHandle_{};
    uint32_t receivedVideoWidth_ = 0;
    uint32_t receivedVideoHeight_ = 0;
    uint32_t receivedVideoUploadCursor_ = 0;
    int activeReceivedVideoUploadBufferIndex_ = -1;
    double receiveUploadBufferWaitMs_ = 0.0;
    double textureUploadMs_ = 0.0;
    double presentGpuWaitMs_ = 0.0;
    double renderFramePacingWaitMs_ = 0.0;
    double waitableSwapChainWaitMs_ = 0.0;
    double presentMs_ = 0.0;
    bool hasReceiveUploadBufferWaitMs_ = false;
    bool hasTextureUploadMs_ = false;
    bool hasPresentGpuWaitMs_ = false;
    bool hasRenderFramePacingWaitMs_ = false;
    bool hasWaitableSwapChainWaitMs_ = false;
    bool hasPresentMs_ = false;
    std::vector<uint64_t> frameFenceValues_;
    std::vector<uint64_t> receivedVideoUploadFenceValues_;
};
