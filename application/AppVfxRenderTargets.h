#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <d3d12.h>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <wrl/client.h>

#include "core/DescriptorHeap.h"
#include "graphics/RenderGraph.h"
#include "resources/ResourceRegistry.h"

class AppVfxRenderTargets {
public:
    struct TargetRequest {
        std::string name;
        float resolutionScale = 1.0f;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        float clearColor[4] = {};
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
        std::string storageName;
        int lifetimeBegin = -1;
        int lifetimeEnd = -1;
        bool transient = false;
        bool pingPong = false;
    };

    void ResetRequests();
    bool RequestTarget(
        std::string name,
        float resolutionScale,
        DXGI_FORMAT format,
        const float clearColor[4],
        D3D12_RESOURCE_STATES initialState,
        bool transient = false,
        bool pingPong = false);
    bool RequestTransientTarget(
        const ge3::graphics::TransientRenderTargetDesc& desc,
        const float clearColor[4],
        D3D12_RESOURCE_STATES initialState,
        bool pingPong = false);

    bool Initialize(
        ID3D12Device* device,
        ge3::core::DescriptorHeapSet& heaps,
        uint32_t width,
        uint32_t height);

    void Register(ge3::resources::ResourceRegistry& registry) const;
    void RegisterGraphResources(ge3::graphics::RenderGraph& renderGraph) const;
    void RegisterDepthBinding(
        ge3::graphics::RenderGraph& renderGraph,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv) const;
    void SetResourceState(std::string_view name, D3D12_RESOURCE_STATES state);
    void BeginScene(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE dsv);
    void BeginVfx(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE dsv);
    void CompositeToBackBuffer(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* backBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        std::string_view postColorResourceName,
        const float compositeParams[8]);
    void ExecutePostProcessPass(
        ID3D12GraphicsCommandList* commandList,
        std::string_view outputResourceName,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        std::string_view inputResourceName,
        std::string_view secondaryResourceName,
        std::string_view tertiaryResourceName,
        const float passParams[8]);
    void ExecuteDebugPreviewPass(
        ID3D12GraphicsCommandList* commandList,
        std::string_view outputResourceName,
        ID3D12RootSignature* rootSignature,
        ID3D12PipelineState* pipelineState,
        D3D12_GPU_DESCRIPTOR_HANDLE inputHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE secondaryHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE tertiaryHandle,
        const float passParams[8]);
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvHandle(std::string_view name) const;

    bool IsInitialized() const { return initialized_; }

private:
    struct TargetKey {
        uint32_t width = 0;
        uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    struct Target {
        std::string name;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        ge3::core::DescriptorHandle rtv{};
        ge3::core::DescriptorHandle srv{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
        float resolutionScale = 1.0f;
        float clearColor[4] = {};
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        std::string storageName;
        int lifetimeBegin = -1;
        int lifetimeEnd = -1;
        bool transient = false;
        bool pingPong = false;
        bool ownsStorage = true;
    };

    bool CreateTarget(
        ID3D12Device* device,
        ge3::core::DescriptorHeapSet& heaps,
        Target& target);
    static TargetKey MakeTargetKey(const Target& target);
    static bool MatchesTargetKey(const Target& target, const TargetKey& key);
    Target AcquireTargetStorage(
        ID3D12Device* device,
        ge3::core::DescriptorHeapSet& heaps,
        const Target& requestedTarget);
    Target AcquirePlannedTransientStorage(
        ID3D12Device* device,
        ge3::core::DescriptorHeapSet& heaps,
        const Target& requestedTarget);
    void ReleaseOwnedTarget(ge3::core::DescriptorHeapSet& heaps, Target& target);
    void ReleaseTargets(ge3::core::DescriptorHeapSet& heaps);
    void ReleaseUnusedTransientStorages(
        ge3::core::DescriptorHeapSet& heaps,
        const std::unordered_set<std::string>& usedStorageNames);
    bool RequestsMatch(uint32_t width, uint32_t height) const;
    void Transition(
        ID3D12GraphicsCommandList* commandList,
        Target& target,
        D3D12_RESOURCE_STATES after);
    Target* FindTarget(std::string_view name);
    const Target* FindTarget(std::string_view name) const;

    std::vector<TargetRequest> requests_;
    std::vector<TargetRequest> activeRequests_;
    std::unordered_map<std::string, Target> targets_;
    std::unordered_map<std::string, Target> transientStorageTargets_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool initialized_ = false;
};
