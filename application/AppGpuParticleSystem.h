#pragma once

#include <cstdint>
#include <string_view>
#include <d3d12.h>
#include <wrl/client.h>

#include "core/DescriptorHeap.h"
#include "graphics/RenderGraph.h"
#include "utils/math/MathUtils.h"

class AppGpuParticleSystem {
public:
    static constexpr uint32_t kDefaultMaxParticles = 65536;

    bool Initialize(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        ge3::core::DescriptorHeapSet& heaps,
        uint32_t maxParticles = kDefaultMaxParticles);

    void Simulate(
        ID3D12GraphicsCommandList* commandList,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        const Matrix4x4& viewProjection,
        float deltaTime,
        float time,
        const Vector4& tint,
        const Vector3& scale,
        float emissive,
        float turbulence,
        float pulseSpeed,
        float spawnRadius,
        float uvScrollSpeed);

    void DeclareGraphBuffers(ge3::graphics::RenderGraph& renderGraph) const;
    bool EnsureGraphBuffers(ID3D12Device* device, const ge3::graphics::RenderGraph& renderGraph);
    void RegisterGraphResources(ge3::graphics::RenderGraph& renderGraph) const;
    void SetResourceState(std::string_view name, D3D12_RESOURCE_STATES state);

    D3D12_GPU_DESCRIPTOR_HANDLE ParticleSrvGpuHandle() const { return particleSrv_.gpu; }
    D3D12_GPU_DESCRIPTOR_HANDLE SrvHandleForResource(std::string_view name) const;
    ID3D12CommandSignature* CommandSignature() const { return commandSignature_.Get(); }
    ID3D12Resource* IndirectArgsBuffer() const { return indirectArgs_.Get(); }
    ID3D12Resource* IndirectArgsForResource(std::string_view name) const;
    uint32_t MaxParticles() const { return maxParticles_; }
    bool IsInitialized() const { return initialized_; }

private:
    struct ParticleState {
        Vector3 position;
        float age;
        Vector3 velocity;
        float lifetime;
        Vector4 color;
        Vector3 scale;
        float seed;
    };

    bool CreateParticleOutputViews(ID3D12Device* device);
    static size_t ParticleRenderBufferBytes(uint32_t maxParticles);
    static size_t ParticleStateBytes(uint32_t maxParticles);

    Microsoft::WRL::ComPtr<ID3D12Resource> particleOutput_;
    Microsoft::WRL::ComPtr<ID3D12Resource> particleState_;
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadState_;
    Microsoft::WRL::ComPtr<ID3D12Resource> indirectArgs_;
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadIndirectArgs_;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> commandSignature_;
    ge3::core::DescriptorHandle particleSrv_{};
    ge3::core::DescriptorHandle particleUav_{};
    ge3::core::DescriptorHandle stateUav_{};
    D3D12_RESOURCE_STATES particleOutputState_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES particleStateState_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES indirectArgsState_ = D3D12_RESOURCE_STATE_COMMON;
    size_t particleOutputBytes_ = 0;
    uint32_t maxParticles_ = 0;
    bool initialized_ = false;
};
