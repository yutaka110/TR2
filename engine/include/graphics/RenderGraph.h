#pragma once

#include <functional>
#include <cstddef>
#include <unordered_map>
#include <string_view>
#include <string>
#include <vector>

#include <d3d12.h>

namespace ge3::graphics {

enum class RenderPassLayer {
    Geometry,
    Lighting,
    Vfx,
    PostProcess,
    Ui,
    Debug,
};

struct RenderPassContext {
    ID3D12GraphicsCommandList* commandList = nullptr;
};

enum class RenderResourceAccessType {
    ReadSrv,
    ReadUav,
    ReadDepth,
    ReadIndirect,
    WriteRtv,
    WriteDepth,
    WriteUav,
};

struct RenderPassResourceAccess {
    std::string resource;
    RenderResourceAccessType type = RenderResourceAccessType::ReadSrv;
};

struct RenderPassDesc {
    std::string name;
    RenderPassLayer layer = RenderPassLayer::Geometry;
    std::vector<RenderPassResourceAccess> accesses;
    std::string depthTarget;
    std::function<void(RenderPassContext&)> execute;
    bool forceExecute = false;
};

struct TransientRenderTargetDesc {
    std::string name;
    std::string storageName;
    float resolutionScale = 1.0f;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    float clearColor[4] = {};
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    int lifetimeBegin = -1;
    int lifetimeEnd = -1;
    bool transient = true;
};

struct TransientBufferDesc {
    std::string name;
    std::string storageName;
    size_t sizeInBytes = 0;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    int lifetimeBegin = -1;
    int lifetimeEnd = -1;
    bool transient = true;
};

struct DepthTargetDesc {
    std::string name;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    float clearDepth = 1.0f;
    uint8_t clearStencil = 0;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
};

struct RenderPassDebugInfo {
    std::string name;
    RenderPassLayer layer = RenderPassLayer::Geometry;
    std::vector<RenderPassResourceAccess> accesses;
    std::string depthTarget;
    std::vector<std::string> requiredOutputs;
    bool executed = false;
    bool forceExecute = false;
    int executionIndex = -1;
    std::string reason;
};

class RenderGraph {
public:
    void Clear();
    void ClearResources();
    void AddPass(RenderPassDesc pass);
    void RegisterResource(
        std::string name,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES initialState,
        std::string stateKey = {});
    void RegisterRenderTargetBinding(
        std::string name,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        uint32_t width,
        uint32_t height);
    void RegisterDepthTargetBinding(
        std::string name,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv);
    void DeclareTransientRenderTarget(
        std::string name,
        float resolutionScale,
        DXGI_FORMAT format,
        const float clearColor[4] = nullptr,
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_RENDER_TARGET);
    void DeclarePersistentRenderTarget(
        std::string name,
        float resolutionScale,
        DXGI_FORMAT format,
        const float clearColor[4],
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_RENDER_TARGET);
    void DeclareTransientBuffer(
        std::string name,
        size_t sizeInBytes,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON);
    void DeclarePersistentBuffer(
        std::string name,
        size_t sizeInBytes,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON);
    void DeclarePersistentDepthTarget(
        std::string name,
        DXGI_FORMAT format,
        float clearDepth = 1.0f,
        uint8_t clearStencil = 0,
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE);
    void SetResourceStateChangedCallback(
        std::function<void(std::string_view, D3D12_RESOURCE_STATES)> callback);
    void Execute(ID3D12GraphicsCommandList* commandList) const;
    std::vector<TransientRenderTargetDesc> BuildTransientRenderTargetPlan() const;
    std::vector<TransientBufferDesc> BuildTransientBufferPlan() const;
    std::vector<RenderPassDebugInfo> BuildPassDebugInfo() const;
    std::string Describe() const;
    bool Validate(std::string* error) const;
    void SetBackBufferResource(std::string_view name);
    void SetPassCullingEnabled(bool enabled) { passCullingEnabled_ = enabled; }

    const std::vector<RenderPassDesc>& Passes() const { return passes_; }
    bool Empty() const { return passes_.empty(); }

private:
    struct ResourceState {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };
    struct RenderTargetBinding {
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
        uint32_t width = 0;
        uint32_t height = 0;
    };
    struct DepthTargetBinding {
        D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    };

    std::vector<const RenderPassDesc*> BuildExecutionList() const;
    static bool IsReadAccess(RenderResourceAccessType type);
    static bool IsWriteAccess(RenderResourceAccessType type);
    static D3D12_RESOURCE_STATES StateForAccess(RenderResourceAccessType type);
    void BindPassTargets(
        ID3D12GraphicsCommandList* commandList,
        const RenderPassDesc& pass) const;
    void EmitUavBarriers(
        ID3D12GraphicsCommandList* commandList,
        const RenderPassDesc& pass) const;
    void TransitionResource(
        ID3D12GraphicsCommandList* commandList,
        const std::string& name,
        D3D12_RESOURCE_STATES after) const;
    std::string ResolveStateKey(std::string_view name) const;

    std::vector<RenderPassDesc> passes_;
    mutable std::unordered_map<std::string, ResourceState> resources_;
    std::unordered_map<std::string, std::string> resourceStateKeys_;
    std::unordered_map<std::string, RenderTargetBinding> renderTargetBindings_;
    std::unordered_map<std::string, DepthTargetBinding> depthTargetBindings_;
    std::unordered_map<std::string, DepthTargetDesc> depthTargetDeclarations_;
    std::unordered_map<std::string, TransientRenderTargetDesc> transientRenderTargets_;
    std::unordered_map<std::string, TransientBufferDesc> transientBuffers_;
    std::function<void(std::string_view, D3D12_RESOURCE_STATES)> stateChanged_;
    std::string backBufferResource_ = "BackBuffer";
    bool passCullingEnabled_ = true;
};

const char* ToString(RenderPassLayer layer);

} // namespace ge3::graphics
