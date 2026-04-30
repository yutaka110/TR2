#pragma once

#include <d3d12.h>

class AppGpuParticleSystem;
class AppPipelines;
class AppRenderResources;
class AppSceneResources;
class BeamRenderer;
struct FrameLoopState;
namespace vfx {
struct VfxTypedResourceSet;
}

struct VfxRenderContext {
    AppPipelines* appPipelines = nullptr;
    AppRenderResources* renderResources = nullptr;
    AppSceneResources* scene = nullptr;
    AppGpuParticleSystem* gpuParticleSystem = nullptr;
    BeamRenderer* beam = nullptr;
    const FrameLoopState* frameState = nullptr;
    ID3D12DescriptorHeap* srvDescriptorHeap = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE vfxTextureHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE depthTextureHandle{};
    float beamTime = 0.0f;
    const vfx::VfxTypedResourceSet* typedResources = nullptr;
};
