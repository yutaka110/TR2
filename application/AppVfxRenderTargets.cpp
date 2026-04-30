#include "AppVfxRenderTargets.h"

namespace {
constexpr DXGI_FORMAT kVfxColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

D3D12_RESOURCE_BARRIER MakeTransition(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

bool RequestEquals(
    const AppVfxRenderTargets::TargetRequest& lhs,
    const AppVfxRenderTargets::TargetRequest& rhs) {
    return lhs.name == rhs.name &&
        lhs.storageName == rhs.storageName &&
        lhs.resolutionScale == rhs.resolutionScale &&
        lhs.format == rhs.format &&
        lhs.clearColor[0] == rhs.clearColor[0] &&
        lhs.clearColor[1] == rhs.clearColor[1] &&
        lhs.clearColor[2] == rhs.clearColor[2] &&
        lhs.clearColor[3] == rhs.clearColor[3] &&
        lhs.initialState == rhs.initialState &&
        lhs.lifetimeBegin == rhs.lifetimeBegin &&
        lhs.lifetimeEnd == rhs.lifetimeEnd &&
        lhs.transient == rhs.transient &&
        lhs.pingPong == rhs.pingPong;
}
} // namespace

AppVfxRenderTargets::TargetKey AppVfxRenderTargets::MakeTargetKey(const Target& target) {
    return {target.width, target.height, target.format};
}

bool AppVfxRenderTargets::MatchesTargetKey(const Target& target, const TargetKey& key) {
    return target.width == key.width &&
        target.height == key.height &&
        target.format == key.format &&
        target.resource != nullptr &&
        target.rtv.IsValid() &&
        target.srv.IsValid();
}

void AppVfxRenderTargets::ResetRequests() {
    requests_.clear();
}

bool AppVfxRenderTargets::RequestTarget(
    std::string name,
    float resolutionScale,
    DXGI_FORMAT format,
    const float clearColor[4],
    D3D12_RESOURCE_STATES initialState,
    bool transient,
    bool pingPong) {
    if (name.empty() || clearColor == nullptr || resolutionScale <= 0.0f || format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }

    TargetRequest request{};
    request.name = std::move(name);
    request.resolutionScale = resolutionScale;
    request.format = format;
    request.clearColor[0] = clearColor[0];
    request.clearColor[1] = clearColor[1];
    request.clearColor[2] = clearColor[2];
    request.clearColor[3] = clearColor[3];
    request.initialState = initialState;
    request.storageName.clear();
    request.lifetimeBegin = -1;
    request.lifetimeEnd = -1;
    request.transient = transient;
    request.pingPong = pingPong;
    requests_.push_back(std::move(request));
    return true;
}

bool AppVfxRenderTargets::RequestTransientTarget(
    const ge3::graphics::TransientRenderTargetDesc& desc,
    const float clearColor[4],
    D3D12_RESOURCE_STATES initialState,
    bool pingPong) {
    if (!RequestTarget(
            desc.name,
            desc.resolutionScale,
            desc.format,
            clearColor,
            initialState,
            true,
            pingPong)) {
        return false;
    }

    TargetRequest& request = requests_.back();
    request.storageName = desc.storageName;
    request.lifetimeBegin = desc.lifetimeBegin;
    request.lifetimeEnd = desc.lifetimeEnd;
    return true;
}

bool AppVfxRenderTargets::Initialize(
    ID3D12Device* device,
    ge3::core::DescriptorHeapSet& heaps,
    uint32_t width,
    uint32_t height) {
    if (device == nullptr || width == 0 || height == 0) {
        return false;
    }

    if (requests_.empty()) {
        const float opaqueBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float transparentBlack[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        ResetRequests();
        RequestTarget("SceneColor", 1.0f, kVfxColorFormat, opaqueBlack, D3D12_RESOURCE_STATE_RENDER_TARGET);
        RequestTarget("VfxAccumulation", 1.0f, kVfxColorFormat, transparentBlack, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    if (initialized_ && RequestsMatch(width, height)) {
        return true;
    }

    ReleaseTargets(heaps);
    width_ = width;
    height_ = height;
    targets_.clear();
    initialized_ = true;
    std::unordered_set<std::string> usedStorageNames;
    for (const TargetRequest& request : requests_) {
        Target requestedTarget{};
        requestedTarget.name = request.name;
        requestedTarget.width = static_cast<uint32_t>(width * request.resolutionScale);
        requestedTarget.height = static_cast<uint32_t>(height * request.resolutionScale);
        requestedTarget.width = requestedTarget.width == 0 ? 1 : requestedTarget.width;
        requestedTarget.height = requestedTarget.height == 0 ? 1 : requestedTarget.height;
        requestedTarget.format = request.format;
        requestedTarget.resolutionScale = request.resolutionScale;
        requestedTarget.clearColor[0] = request.clearColor[0];
        requestedTarget.clearColor[1] = request.clearColor[1];
        requestedTarget.clearColor[2] = request.clearColor[2];
        requestedTarget.clearColor[3] = request.clearColor[3];
        requestedTarget.state = request.initialState;
        requestedTarget.storageName = request.storageName;
        requestedTarget.lifetimeBegin = request.lifetimeBegin;
        requestedTarget.lifetimeEnd = request.lifetimeEnd;
        requestedTarget.transient = request.transient;
        requestedTarget.pingPong = request.pingPong;

        Target target = AcquireTargetStorage(device, heaps, requestedTarget);
        if (target.resource == nullptr) {
            initialized_ = false;
            break;
        }
        if (!request.storageName.empty()) {
            usedStorageNames.insert(request.storageName);
        }
        targets_[target.name] = std::move(target);
    }
    ReleaseUnusedTransientStorages(heaps, usedStorageNames);
    activeRequests_ = requests_;
    return initialized_;
}

bool AppVfxRenderTargets::CreateTarget(
    ID3D12Device* device,
    ge3::core::DescriptorHeapSet& heaps,
    Target& target) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = target.width;
    desc.Height = target.height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = target.format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = target.format;
    clearValue.Color[0] = target.clearColor[0];
    clearValue.Color[1] = target.clearColor[1];
    clearValue.Color[2] = target.clearColor[2];
    clearValue.Color[3] = target.clearColor[3];

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        target.state,
        &clearValue,
        IID_PPV_ARGS(&target.resource));
    if (FAILED(hr)) {
        return false;
    }

    target.rtv = heaps.rtv.Allocate();
    target.srv = heaps.srv.Allocate();

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = target.format;
    rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(target.resource.Get(), &rtvDesc, target.rtv.cpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = target.format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(target.resource.Get(), &srvDesc, target.srv.cpu);
    return true;
}

AppVfxRenderTargets::Target AppVfxRenderTargets::AcquireTargetStorage(
    ID3D12Device* device,
    ge3::core::DescriptorHeapSet& heaps,
    const Target& requestedTarget) {
    if (!requestedTarget.storageName.empty()) {
        return AcquirePlannedTransientStorage(device, heaps, requestedTarget);
    }

    Target created = requestedTarget;
    created.ownsStorage = true;
    if (!CreateTarget(device, heaps, created)) {
        return {};
    }
    return created;
}

AppVfxRenderTargets::Target AppVfxRenderTargets::AcquirePlannedTransientStorage(
    ID3D12Device* device,
    ge3::core::DescriptorHeapSet& heaps,
    const Target& requestedTarget) {
    Target& storage = transientStorageTargets_[requestedTarget.storageName];
    const TargetKey requestedKey = MakeTargetKey(requestedTarget);
    if (!MatchesTargetKey(storage, requestedKey)) {
        ReleaseOwnedTarget(heaps, storage);
        storage = requestedTarget;
        storage.name = requestedTarget.storageName;
        storage.ownsStorage = true;
        if (!CreateTarget(device, heaps, storage)) {
            storage = {};
            return {};
        }
    }

    Target logicalTarget = storage;
    logicalTarget.name = requestedTarget.name;
    logicalTarget.storageName = requestedTarget.storageName;
    logicalTarget.resolutionScale = requestedTarget.resolutionScale;
    logicalTarget.clearColor[0] = requestedTarget.clearColor[0];
    logicalTarget.clearColor[1] = requestedTarget.clearColor[1];
    logicalTarget.clearColor[2] = requestedTarget.clearColor[2];
    logicalTarget.clearColor[3] = requestedTarget.clearColor[3];
    logicalTarget.lifetimeBegin = requestedTarget.lifetimeBegin;
    logicalTarget.lifetimeEnd = requestedTarget.lifetimeEnd;
    logicalTarget.transient = requestedTarget.transient;
    logicalTarget.pingPong = requestedTarget.pingPong;
    logicalTarget.state = storage.state;
    logicalTarget.ownsStorage = false;
    return logicalTarget;
}

void AppVfxRenderTargets::ReleaseOwnedTarget(
    ge3::core::DescriptorHeapSet& heaps,
    Target& target) {
    if (target.rtv.IsValid()) {
        heaps.rtv.Free(target.rtv.index);
    }
    if (target.srv.IsValid()) {
        heaps.srv.Free(target.srv.index);
    }
    target.rtv = {};
    target.srv = {};
    target.resource.Reset();
}

void AppVfxRenderTargets::ReleaseTargets(ge3::core::DescriptorHeapSet& heaps) {
    for (auto& [name, target] : targets_) {
        (void)name;
        if (!target.ownsStorage) {
            continue;
        }
        ReleaseOwnedTarget(heaps, target);
    }
    targets_.clear();
}

void AppVfxRenderTargets::ReleaseUnusedTransientStorages(
    ge3::core::DescriptorHeapSet& heaps,
    const std::unordered_set<std::string>& usedStorageNames) {
    for (auto it = transientStorageTargets_.begin(); it != transientStorageTargets_.end();) {
        if (usedStorageNames.contains(it->first)) {
            ++it;
            continue;
        }
        ReleaseOwnedTarget(heaps, it->second);
        it = transientStorageTargets_.erase(it);
    }
}

bool AppVfxRenderTargets::RequestsMatch(uint32_t width, uint32_t height) const {
    if (width_ != width || height_ != height || activeRequests_.size() != requests_.size()) {
        return false;
    }

    for (size_t i = 0; i < requests_.size(); ++i) {
        if (!RequestEquals(activeRequests_[i], requests_[i])) {
            return false;
        }
    }
    return true;
}

void AppVfxRenderTargets::Register(ge3::resources::ResourceRegistry& registry) const {
    for (const auto& [name, target] : targets_) {
        registry.RegisterRenderTarget({name, target.resource, target.rtv.cpu, target.srv.gpu, target.format, target.width, target.height});
    }
}

void AppVfxRenderTargets::RegisterGraphResources(ge3::graphics::RenderGraph& renderGraph) const {
    for (const auto& [name, target] : targets_) {
        renderGraph.RegisterResource(
            name,
            target.resource.Get(),
            target.state,
            target.storageName.empty() ? name : target.storageName);
        renderGraph.RegisterRenderTargetBinding(name, target.rtv.cpu, target.width, target.height);
    }
}

void AppVfxRenderTargets::RegisterDepthBinding(
    ge3::graphics::RenderGraph& renderGraph,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv) const {
    renderGraph.RegisterDepthTargetBinding("SceneDepth", dsv);
}

void AppVfxRenderTargets::SetResourceState(std::string_view name, D3D12_RESOURCE_STATES state) {
    Target* target = FindTarget(name);
    if (target == nullptr) {
        auto foundStorage = transientStorageTargets_.find(std::string(name));
        if (foundStorage == transientStorageTargets_.end()) {
            return;
        }
        foundStorage->second.state = state;
        for (auto& [otherName, otherTarget] : targets_) {
            (void)otherName;
            if (otherTarget.storageName == name) {
                otherTarget.state = state;
            }
        }
        return;
    }

    target->state = state;
    if (target->storageName.empty()) {
        return;
    }

    auto foundStorage = transientStorageTargets_.find(target->storageName);
    if (foundStorage != transientStorageTargets_.end()) {
        foundStorage->second.state = state;
    }
    for (auto& [otherName, otherTarget] : targets_) {
        (void)otherName;
        if (otherTarget.storageName == target->storageName) {
            otherTarget.state = state;
        }
    }
}

AppVfxRenderTargets::Target* AppVfxRenderTargets::FindTarget(std::string_view name) {
    const auto found = targets_.find(std::string(name));
    return found != targets_.end() ? &found->second : nullptr;
}

const AppVfxRenderTargets::Target* AppVfxRenderTargets::FindTarget(std::string_view name) const {
    const auto found = targets_.find(std::string(name));
    return found != targets_.end() ? &found->second : nullptr;
}

void AppVfxRenderTargets::Transition(
    ID3D12GraphicsCommandList* commandList,
    Target& target,
    D3D12_RESOURCE_STATES after) {
    if (commandList == nullptr || target.resource == nullptr || target.state == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = MakeTransition(target.resource.Get(), target.state, after);
    commandList->ResourceBarrier(1, &barrier);
    SetResourceState(target.storageName.empty() ? std::string_view(target.name) : std::string_view(target.storageName), after);
}

void AppVfxRenderTargets::BeginScene(
    ID3D12GraphicsCommandList* commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv) {
    if (!initialized_ || commandList == nullptr) {
        return;
    }

    Target* sceneColor = FindTarget("SceneColor");
    Target* vfxAccumulation = FindTarget("VfxAccumulation");
    if (sceneColor == nullptr || vfxAccumulation == nullptr) {
        return;
    }

    Transition(commandList, *sceneColor, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Transition(commandList, *vfxAccumulation, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float clearScene[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float clearVfx[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    commandList->ClearRenderTargetView(sceneColor->rtv.cpu, clearScene, 0, nullptr);
    commandList->ClearRenderTargetView(vfxAccumulation->rtv.cpu, clearVfx, 0, nullptr);
    for (auto& [name, target] : targets_) {
        if (name == "SceneColor" || name == "VfxAccumulation") {
            continue;
        }
        Transition(commandList, target, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commandList->ClearRenderTargetView(target.rtv.cpu, target.clearColor, 0, nullptr);
    }
    (void)dsv;
}

void AppVfxRenderTargets::BeginVfx(
    ID3D12GraphicsCommandList* commandList,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv) {
    if (!initialized_ || commandList == nullptr) {
        return;
    }

    Target* vfxAccumulation = FindTarget("VfxAccumulation");
    if (vfxAccumulation == nullptr) {
        return;
    }
    Transition(commandList, *vfxAccumulation, D3D12_RESOURCE_STATE_RENDER_TARGET);
    (void)dsv;
}

void AppVfxRenderTargets::CompositeToBackBuffer(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* backBuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pipelineState,
    std::string_view postColorResourceName,
    const float compositeParams[8]) {
    if (!initialized_ || commandList == nullptr || backBuffer == nullptr ||
        rootSignature == nullptr || pipelineState == nullptr) {
        return;
    }

    Target* sceneColor = FindTarget("SceneColor");
    Target* vfxAccumulation = FindTarget("VfxAccumulation");
    Target* postColor = FindTarget(postColorResourceName);
    if (sceneColor == nullptr || vfxAccumulation == nullptr || postColor == nullptr) {
        return;
    }

    Transition(commandList, *sceneColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(commandList, *vfxAccumulation, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(commandList, *postColor, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    (void)backBuffer;
    (void)backBufferRtv;
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->SetGraphicsRootDescriptorTable(0, sceneColor->srv.gpu);
    commandList->SetGraphicsRootDescriptorTable(1, vfxAccumulation->srv.gpu);
    commandList->SetGraphicsRootDescriptorTable(2, postColor->srv.gpu);
    commandList->SetGraphicsRoot32BitConstants(3, 8, compositeParams, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}

void AppVfxRenderTargets::ExecutePostProcessPass(
    ID3D12GraphicsCommandList* commandList,
    std::string_view outputResourceName,
    ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pipelineState,
    std::string_view inputResourceName,
    std::string_view secondaryResourceName,
    std::string_view tertiaryResourceName,
    const float passParams[8]) {
    if (!initialized_ || commandList == nullptr || rootSignature == nullptr || pipelineState == nullptr) {
        return;
    }

    Target* output = FindTarget(outputResourceName);
    const Target* input = FindTarget(inputResourceName);
    const Target* secondary = FindTarget(secondaryResourceName);
    const Target* tertiary = FindTarget(tertiaryResourceName);
    if (output == nullptr || input == nullptr || secondary == nullptr || tertiary == nullptr) {
        return;
    }

    (void)output;
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->SetGraphicsRootDescriptorTable(0, input->srv.gpu);
    commandList->SetGraphicsRootDescriptorTable(1, secondary->srv.gpu);
    commandList->SetGraphicsRootDescriptorTable(2, tertiary->srv.gpu);
    commandList->SetGraphicsRoot32BitConstants(3, 8, passParams, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}

void AppVfxRenderTargets::ExecuteDebugPreviewPass(
    ID3D12GraphicsCommandList* commandList,
    std::string_view outputResourceName,
    ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pipelineState,
    D3D12_GPU_DESCRIPTOR_HANDLE inputHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE secondaryHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE tertiaryHandle,
    const float passParams[8]) {
    if (!initialized_ || commandList == nullptr || rootSignature == nullptr ||
        pipelineState == nullptr || inputHandle.ptr == 0 ||
        secondaryHandle.ptr == 0 || tertiaryHandle.ptr == 0) {
        return;
    }

    Target* output = FindTarget(outputResourceName);
    if (output == nullptr) {
        return;
    }

    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->SetGraphicsRootDescriptorTable(0, inputHandle);
    commandList->SetGraphicsRootDescriptorTable(1, secondaryHandle);
    commandList->SetGraphicsRootDescriptorTable(2, tertiaryHandle);
    commandList->SetGraphicsRoot32BitConstants(3, 8, passParams, 0);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
}

D3D12_GPU_DESCRIPTOR_HANDLE AppVfxRenderTargets::GetSrvHandle(std::string_view name) const {
    const Target* target = FindTarget(name);
    if (target == nullptr) {
        return {};
    }
    return target->srv.gpu;
}
