#pragma once

#include <d3d12.h>

#include "AppFrameState.h"
#include "EffectSystem.h"

class AppFrameRenderer;
class AppGpuParticleSystem;
class AppImGuiLayer;
class AppPipelines;
class AppRenderResources;
class AppSceneResources;
class AppVfxRenderTargets;
struct AppVfxRendererSet;
class PostProcessStack;
struct AppRuntimeState;

namespace ge3::graphics {
class RenderGraph;
}

struct AppFrameGraphBuildContext {
    ge3::graphics::RenderGraph* renderGraph = nullptr;
    AppRuntimeState* runtimeState = nullptr;
    AppFrameRenderer* frameRenderer = nullptr;
    AppImGuiLayer* imguiLayer = nullptr;
    AppPipelines* appPipelines = nullptr;
    AppRenderResources* renderResources = nullptr;
    AppSceneResources* scene = nullptr;
    AppVfxRenderTargets* vfxRenderTargets = nullptr;
    AppGpuParticleSystem* gpuParticleSystem = nullptr;
    AppVfxRendererSet* vfxRenderers = nullptr;
    PostProcessStack* postProcessStack = nullptr;
    FrameLoopState* frameState = nullptr;
    ID3D12DescriptorHeap* srvDescriptorHeap = nullptr;
    ID3D12Resource* backBuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    D3D12_GPU_DESCRIPTOR_HANDLE spriteTextureHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE vfxTextureHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE depthTextureHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE receivedTextureHandle{};
    const EffectRuntimeFrame* effectRuntime = nullptr;
    ParticleRenderFallback primaryParticleFx{};
    float beamTime = 0.0f;
};

class AppFrameGraphBuilder {
public:
    void Build(const AppFrameGraphBuildContext& context) const;
};
