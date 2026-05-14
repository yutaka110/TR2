#pragma once
#include <functional>
#include <string>
#include <vector>
#include <Windows.h>
#include <d3d12.h>

#include "graphics/RenderGraph.h"

struct AppRuntimeState;
class EffectRuntime;
class PostProcessStack;
namespace net {
    struct NetworkStatsSnapshot;
    struct NetworkCondition;
}
class AppImGuiLayer {
public:
    bool Initialize(HWND hwnd, ID3D12Device* device, int bufferCount,
        DXGI_FORMAT rtvFormat, ID3D12DescriptorHeap* srvHeap);

    void BeginFrame();
    void BuildUi(
        AppRuntimeState& runtimeState,
        EffectRuntime& effectRuntime,
        PostProcessStack& postProcessStack,
        const std::string& renderGraphDescription,
        const std::string& renderGraphError,
        const std::vector<ge3::graphics::RenderPassDebugInfo>& renderPassDebugInfo,
        uint32_t transientTargetCount,
        uint32_t transientTargetStorageCount,
        uint32_t transientBufferCount,
        uint32_t transientBufferStorageCount,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneColorPreview,
        D3D12_GPU_DESCRIPTOR_HANDLE vfxAccumulationPreview,
        D3D12_GPU_DESCRIPTOR_HANDLE postColorPreview,
        D3D12_GPU_DESCRIPTOR_HANDLE depthPreview,
        D3D12_GPU_DESCRIPTOR_HANDLE emissivePreview,
        D3D12_GPU_DESCRIPTOR_HANDLE receivedVideoPreview,
        const net::NetworkStatsSnapshot* networkStats,
        const std::function<void(uint32_t)>& onJitterBufferTargetDelayChanged,
        const std::function<void(bool)>& onJitterBufferAutoModeChanged,
        const std::function<void(const net::NetworkCondition&)>& onNetworkConditionChanged,
        const std::function<void()>& onAddParticle);
    void EndFrame();

    void Render(ID3D12GraphicsCommandList* cmdList);

    void Shutdown();

private:
    bool initialized_ = false;
    uint32_t selectedEffectInstanceId_ = 0;
};
