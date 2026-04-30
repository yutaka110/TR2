#include "graphics/RenderGraph.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace ge3::graphics {

namespace {
int LayerOrder(RenderPassLayer layer) {
    switch (layer) {
    case RenderPassLayer::Geometry: return 0;
    case RenderPassLayer::Lighting: return 1;
    case RenderPassLayer::Vfx: return 2;
    case RenderPassLayer::PostProcess: return 3;
    case RenderPassLayer::Ui: return 4;
    case RenderPassLayer::Debug: return 5;
    }
    return 0;
}

const char* AccessTypeLabel(RenderResourceAccessType type) {
    switch (type) {
    case RenderResourceAccessType::ReadSrv:
        return "ReadSRV";
    case RenderResourceAccessType::ReadUav:
        return "ReadUAV";
    case RenderResourceAccessType::ReadDepth:
        return "ReadDepth";
    case RenderResourceAccessType::ReadIndirect:
        return "ReadIndirect";
    case RenderResourceAccessType::WriteRtv:
        return "WriteRTV";
    case RenderResourceAccessType::WriteDepth:
        return "WriteDepth";
    case RenderResourceAccessType::WriteUav:
        return "WriteUAV";
    }
    return "Unknown";
}

std::string JoinStrings(const std::vector<std::string>& values, std::string_view separator) {
    std::ostringstream stream;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            stream << separator;
        }
        stream << values[i];
    }
    return stream.str();
}
} // namespace

bool RenderGraph::IsReadAccess(RenderResourceAccessType type) {
    return type == RenderResourceAccessType::ReadSrv ||
        type == RenderResourceAccessType::ReadUav ||
        type == RenderResourceAccessType::ReadDepth ||
        type == RenderResourceAccessType::ReadIndirect;
}

bool RenderGraph::IsWriteAccess(RenderResourceAccessType type) {
    return type == RenderResourceAccessType::WriteRtv ||
        type == RenderResourceAccessType::WriteDepth ||
        type == RenderResourceAccessType::WriteUav;
}

D3D12_RESOURCE_STATES RenderGraph::StateForAccess(RenderResourceAccessType type) {
    switch (type) {
    case RenderResourceAccessType::ReadUav:
    case RenderResourceAccessType::WriteUav:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case RenderResourceAccessType::ReadDepth:
        return D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case RenderResourceAccessType::ReadIndirect:
        return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case RenderResourceAccessType::WriteRtv:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case RenderResourceAccessType::WriteDepth:
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case RenderResourceAccessType::ReadSrv:
    default:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
}

void RenderGraph::Clear() {
    passes_.clear();
}

void RenderGraph::ClearResources() {
    resources_.clear();
    resourceStateKeys_.clear();
    renderTargetBindings_.clear();
    depthTargetBindings_.clear();
    depthTargetDeclarations_.clear();
    transientRenderTargets_.clear();
    transientBuffers_.clear();
    stateChanged_ = nullptr;
}

void RenderGraph::AddPass(RenderPassDesc pass) {
    passes_.push_back(std::move(pass));
}

void RenderGraph::RegisterResource(
    std::string name,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES initialState,
    std::string stateKey) {
    if (name.empty() || resource == nullptr) {
        return;
    }
    const std::string logicalName = name;
    if (stateKey.empty()) {
        stateKey = logicalName;
    }
    resourceStateKeys_[logicalName] = stateKey;
    ResourceState& state = resources_[stateKey];
    state.resource = resource;
    state.state = initialState;
}

void RenderGraph::RegisterRenderTargetBinding(
    std::string name,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    uint32_t width,
    uint32_t height) {
    if (name.empty() || rtv.ptr == 0 || width == 0 || height == 0) {
        return;
    }
    renderTargetBindings_[std::move(name)] = {rtv, width, height};
}

void RenderGraph::RegisterDepthTargetBinding(
    std::string name,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv) {
    if (name.empty() || dsv.ptr == 0) {
        return;
    }
    depthTargetBindings_[std::move(name)] = {dsv};
}

void RenderGraph::DeclareTransientRenderTarget(
    std::string name,
    float resolutionScale,
    DXGI_FORMAT format,
    const float clearColor[4],
    D3D12_RESOURCE_STATES initialState) {
    if (name.empty() || resolutionScale <= 0.0f || format == DXGI_FORMAT_UNKNOWN) {
        return;
    }
    const std::string key = name;
    TransientRenderTargetDesc& desc = transientRenderTargets_[std::move(name)];
    desc.name = key;
    desc.resolutionScale = (std::max)(desc.resolutionScale, resolutionScale);
    desc.format = format;
    desc.clearColor[0] = clearColor != nullptr ? clearColor[0] : 0.0f;
    desc.clearColor[1] = clearColor != nullptr ? clearColor[1] : 0.0f;
    desc.clearColor[2] = clearColor != nullptr ? clearColor[2] : 0.0f;
    desc.clearColor[3] = clearColor != nullptr ? clearColor[3] : 0.0f;
    desc.initialState = initialState;
    desc.lifetimeBegin = -1;
    desc.lifetimeEnd = -1;
    desc.transient = true;
}

void RenderGraph::DeclarePersistentRenderTarget(
    std::string name,
    float resolutionScale,
    DXGI_FORMAT format,
    const float clearColor[4],
    D3D12_RESOURCE_STATES initialState) {
    const std::string key = name;
    DeclareTransientRenderTarget(
        std::move(name),
        resolutionScale,
        format,
        clearColor,
        initialState);
    TransientRenderTargetDesc& desc = transientRenderTargets_[key];
    desc.transient = false;
}

void RenderGraph::DeclareTransientBuffer(
    std::string name,
    size_t sizeInBytes,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState) {
    if (name.empty() || sizeInBytes == 0) {
        return;
    }
    const std::string key = name;
    TransientBufferDesc& desc = transientBuffers_[std::move(name)];
    desc.name = key;
    desc.sizeInBytes = (std::max)(desc.sizeInBytes, sizeInBytes);
    desc.flags = flags;
    desc.initialState = initialState;
    desc.lifetimeBegin = -1;
    desc.lifetimeEnd = -1;
    desc.transient = true;
}

void RenderGraph::DeclarePersistentBuffer(
    std::string name,
    size_t sizeInBytes,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState) {
    const std::string key = name;
    DeclareTransientBuffer(std::move(name), sizeInBytes, flags, initialState);
    TransientBufferDesc& desc = transientBuffers_[key];
    desc.transient = false;
}

void RenderGraph::DeclarePersistentDepthTarget(
    std::string name,
    DXGI_FORMAT format,
    float clearDepth,
    uint8_t clearStencil,
    D3D12_RESOURCE_STATES initialState) {
    if (name.empty() || format == DXGI_FORMAT_UNKNOWN) {
        return;
    }

    const std::string key = name;
    DepthTargetDesc& desc = depthTargetDeclarations_[std::move(name)];
    desc.name = key;
    desc.format = format;
    desc.clearDepth = clearDepth;
    desc.clearStencil = clearStencil;
    desc.initialState = initialState;
}

void RenderGraph::SetResourceStateChangedCallback(
    std::function<void(std::string_view, D3D12_RESOURCE_STATES)> callback) {
    stateChanged_ = std::move(callback);
}

std::vector<TransientRenderTargetDesc> RenderGraph::BuildTransientRenderTargetPlan() const {
    std::map<std::string, TransientRenderTargetDesc> planned;
    for (const auto& [name, desc] : transientRenderTargets_) {
        planned.emplace(name, desc);
    }

    const std::vector<const RenderPassDesc*> orderedPasses = BuildExecutionList();
    for (int passIndex = 0; passIndex < static_cast<int>(orderedPasses.size()); ++passIndex) {
        const RenderPassDesc* pass = orderedPasses[passIndex];
        if (pass == nullptr) {
            continue;
        }
        for (const RenderPassResourceAccess& access : pass->accesses) {
            auto found = planned.find(access.resource);
            if (found == planned.end()) {
                continue;
            }

            TransientRenderTargetDesc& desc = found->second;
            if (desc.lifetimeBegin < 0) {
                desc.lifetimeBegin = passIndex;
            }
            desc.lifetimeEnd = passIndex;
        }
    }

    std::vector<TransientRenderTargetDesc> result;
    result.reserve(planned.size());
    for (auto& [name, desc] : planned) {
        (void)name;
        if (desc.lifetimeBegin >= 0 && desc.lifetimeEnd >= desc.lifetimeBegin) {
            result.push_back(desc);
        }
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const TransientRenderTargetDesc& lhs, const TransientRenderTargetDesc& rhs) {
            if (lhs.lifetimeBegin != rhs.lifetimeBegin) {
                return lhs.lifetimeBegin < rhs.lifetimeBegin;
            }
            if (lhs.resolutionScale != rhs.resolutionScale) {
                return lhs.resolutionScale < rhs.resolutionScale;
            }
            if (lhs.format != rhs.format) {
                return lhs.format < rhs.format;
            }
            return lhs.name < rhs.name;
        });

    struct StorageSlot {
        std::string name;
        float resolutionScale = 1.0f;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        int lifetimeEnd = -1;
    };

    std::vector<StorageSlot> slots;
    for (TransientRenderTargetDesc& desc : result) {
        if (!desc.transient) {
            desc.storageName = desc.name;
            continue;
        }
        auto foundSlot = slots.end();
        for (auto it = slots.begin(); it != slots.end(); ++it) {
            if (it->resolutionScale != desc.resolutionScale ||
                it->format != desc.format ||
                it->lifetimeEnd >= desc.lifetimeBegin) {
                continue;
            }
            foundSlot = it;
            break;
        }

        if (foundSlot == slots.end()) {
            StorageSlot slot{};
            slot.name = "_TransientStorage" + std::to_string(slots.size());
            slot.resolutionScale = desc.resolutionScale;
            slot.format = desc.format;
            slot.lifetimeEnd = desc.lifetimeEnd;
            desc.storageName = slot.name;
            slots.push_back(std::move(slot));
            continue;
        }

        desc.storageName = foundSlot->name;
        foundSlot->lifetimeEnd = desc.lifetimeEnd;
    }
    return result;
}

std::vector<TransientBufferDesc> RenderGraph::BuildTransientBufferPlan() const {
    std::map<std::string, TransientBufferDesc> planned;
    for (const auto& [name, desc] : transientBuffers_) {
        planned.emplace(name, desc);
    }

    const std::vector<const RenderPassDesc*> orderedPasses = BuildExecutionList();
    for (int passIndex = 0; passIndex < static_cast<int>(orderedPasses.size()); ++passIndex) {
        const RenderPassDesc* pass = orderedPasses[passIndex];
        if (pass == nullptr) {
            continue;
        }
        for (const RenderPassResourceAccess& access : pass->accesses) {
            auto found = planned.find(access.resource);
            if (found == planned.end()) {
                continue;
            }
            TransientBufferDesc& desc = found->second;
            if (desc.lifetimeBegin < 0) {
                desc.lifetimeBegin = passIndex;
            }
            desc.lifetimeEnd = passIndex;
        }
    }

    std::vector<TransientBufferDesc> result;
    result.reserve(planned.size());
    for (auto& [name, desc] : planned) {
        (void)name;
        if (desc.lifetimeBegin >= 0 && desc.lifetimeEnd >= desc.lifetimeBegin) {
            result.push_back(desc);
        }
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const TransientBufferDesc& lhs, const TransientBufferDesc& rhs) {
            if (lhs.lifetimeBegin != rhs.lifetimeBegin) {
                return lhs.lifetimeBegin < rhs.lifetimeBegin;
            }
            if (lhs.sizeInBytes != rhs.sizeInBytes) {
                return lhs.sizeInBytes < rhs.sizeInBytes;
            }
            if (lhs.flags != rhs.flags) {
                return lhs.flags < rhs.flags;
            }
            return lhs.name < rhs.name;
        });

    struct StorageSlot {
        std::string name;
        size_t sizeInBytes = 0;
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        int lifetimeEnd = -1;
    };

    std::vector<StorageSlot> slots;
    for (TransientBufferDesc& desc : result) {
        if (!desc.transient) {
            desc.storageName = desc.name;
            continue;
        }
        auto foundSlot = slots.end();
        for (auto it = slots.begin(); it != slots.end(); ++it) {
            if (it->sizeInBytes != desc.sizeInBytes ||
                it->flags != desc.flags ||
                it->lifetimeEnd >= desc.lifetimeBegin) {
                continue;
            }
            foundSlot = it;
            break;
        }
        if (foundSlot == slots.end()) {
            StorageSlot slot{};
            slot.name = "_TransientBufferStorage" + std::to_string(slots.size());
            slot.sizeInBytes = desc.sizeInBytes;
            slot.flags = desc.flags;
            slot.lifetimeEnd = desc.lifetimeEnd;
            desc.storageName = slot.name;
            slots.push_back(std::move(slot));
            continue;
        }
        desc.storageName = foundSlot->name;
        foundSlot->lifetimeEnd = desc.lifetimeEnd;
    }
    return result;
}

std::vector<const RenderPassDesc*> RenderGraph::BuildExecutionList() const {
    std::vector<const RenderPassDesc*> orderedPasses;
    orderedPasses.reserve(passes_.size());
    for (const RenderPassDesc& pass : passes_) {
        orderedPasses.push_back(&pass);
    }

    std::stable_sort(
        orderedPasses.begin(),
        orderedPasses.end(),
        [](const RenderPassDesc* lhs, const RenderPassDesc* rhs) {
            return LayerOrder(lhs->layer) < LayerOrder(rhs->layer);
        });

    if (!passCullingEnabled_) {
        return orderedPasses;
    }

    std::unordered_set<std::string> needed;
    needed.insert(backBufferResource_);

    std::vector<const RenderPassDesc*> culled;
    for (auto it = orderedPasses.rbegin(); it != orderedPasses.rend(); ++it) {
        const RenderPassDesc* pass = *it;
        bool required = pass->forceExecute;
        for (const RenderPassResourceAccess& access : pass->accesses) {
            if (IsWriteAccess(access.type) && needed.contains(access.resource)) {
                required = true;
                break;
            }
        }

        if (!required) {
            continue;
        }

        for (const RenderPassResourceAccess& access : pass->accesses) {
            if (IsReadAccess(access.type)) {
                needed.insert(access.resource);
            }
        }
        culled.push_back(pass);
    }

    std::reverse(culled.begin(), culled.end());
    return culled;
}

std::vector<RenderPassDebugInfo> RenderGraph::BuildPassDebugInfo() const {
    std::vector<const RenderPassDesc*> orderedPasses;
    orderedPasses.reserve(passes_.size());
    for (const RenderPassDesc& pass : passes_) {
        orderedPasses.push_back(&pass);
    }

    std::stable_sort(
        orderedPasses.begin(),
        orderedPasses.end(),
        [](const RenderPassDesc* lhs, const RenderPassDesc* rhs) {
            return LayerOrder(lhs->layer) < LayerOrder(rhs->layer);
        });

    std::vector<RenderPassDebugInfo> debugInfo;
    debugInfo.reserve(orderedPasses.size());
    for (const RenderPassDesc* pass : orderedPasses) {
        RenderPassDebugInfo info{};
        info.name = pass->name;
        info.layer = pass->layer;
        info.accesses = pass->accesses;
        info.depthTarget = pass->depthTarget;
        info.forceExecute = pass->forceExecute;
        debugInfo.push_back(std::move(info));
    }

    if (!passCullingEnabled_) {
        for (size_t index = 0; index < debugInfo.size(); ++index) {
            debugInfo[index].executed = true;
            debugInfo[index].executionIndex = static_cast<int>(index);
            debugInfo[index].reason = debugInfo[index].forceExecute
                ? "pass culling disabled (forceExecute)"
                : "pass culling disabled";
        }
        return debugInfo;
    }

    std::unordered_set<std::string> needed;
    needed.insert(backBufferResource_);
    for (int i = static_cast<int>(orderedPasses.size()) - 1; i >= 0; --i) {
        const RenderPassDesc* pass = orderedPasses[static_cast<size_t>(i)];
        RenderPassDebugInfo& info = debugInfo[static_cast<size_t>(i)];

        std::vector<std::string> requiredOutputs;
        for (const RenderPassResourceAccess& access : pass->accesses) {
            if (IsWriteAccess(access.type) && needed.contains(access.resource)) {
                requiredOutputs.push_back(access.resource);
            }
        }
        info.requiredOutputs = requiredOutputs;

        if (pass->forceExecute) {
            info.executed = true;
            info.reason = "forceExecute";
        } else if (!requiredOutputs.empty()) {
            info.executed = true;
            info.reason = "required by " + JoinStrings(requiredOutputs, ", ");
        } else {
            info.executed = false;
            info.reason = "culled (no required outputs)";
            continue;
        }

        for (const RenderPassResourceAccess& access : pass->accesses) {
            if (IsReadAccess(access.type)) {
                needed.insert(access.resource);
            }
        }
    }

    int executionIndex = 0;
    for (RenderPassDebugInfo& info : debugInfo) {
        if (info.executed) {
            info.executionIndex = executionIndex++;
        } else {
            info.executionIndex = -1;
        }
    }
    return debugInfo;
}

void RenderGraph::Execute(ID3D12GraphicsCommandList* commandList) const {
    if (commandList == nullptr) {
        return;
    }

    const std::vector<const RenderPassDesc*> orderedPasses = BuildExecutionList();
    RenderPassContext context{};
    context.commandList = commandList;
    for (const RenderPassDesc* pass : orderedPasses) {
        if (pass != nullptr && pass->execute) {
            std::unordered_set<std::string> transitioned;
            for (const RenderPassResourceAccess& access : pass->accesses) {
                if (access.resource.empty() || transitioned.contains(access.resource)) {
                    continue;
                }

                RenderResourceAccessType selectedAccess = access.type;
                for (const RenderPassResourceAccess& candidate : pass->accesses) {
                    if (candidate.resource != access.resource) {
                        continue;
                    }
                    if (IsWriteAccess(candidate.type)) {
                        selectedAccess = candidate.type;
                        break;
                    }
                }

                TransitionResource(commandList, access.resource, StateForAccess(selectedAccess));
                transitioned.insert(access.resource);
            }
            BindPassTargets(commandList, *pass);
            pass->execute(context);
            EmitUavBarriers(commandList, *pass);
        }
    }
}

void RenderGraph::BindPassTargets(
    ID3D12GraphicsCommandList* commandList,
    const RenderPassDesc& pass) const {
    if (commandList == nullptr) {
        return;
    }

    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
    for (const RenderPassResourceAccess& access : pass.accesses) {
        if (access.type != RenderResourceAccessType::WriteRtv) {
            continue;
        }

        const auto found = renderTargetBindings_.find(access.resource);
        if (found == renderTargetBindings_.end()) {
            continue;
        }
        rtvs.push_back(found->second.handle);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE* dsv = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE depthHandle{};
    if (!pass.depthTarget.empty()) {
        const auto foundDepth = depthTargetBindings_.find(pass.depthTarget);
        if (foundDepth != depthTargetBindings_.end()) {
            depthHandle = foundDepth->second.handle;
            dsv = &depthHandle;
        }
    }

    if (!rtvs.empty()) {
        const RenderTargetBinding* firstBinding = nullptr;
        for (const RenderPassResourceAccess& access : pass.accesses) {
            if (access.type != RenderResourceAccessType::WriteRtv) {
                continue;
            }
            const auto found = renderTargetBindings_.find(access.resource);
            if (found != renderTargetBindings_.end()) {
                firstBinding = &found->second;
                break;
            }
        }

        if (firstBinding != nullptr) {
            D3D12_VIEWPORT viewport{};
            viewport.TopLeftX = 0.0f;
            viewport.TopLeftY = 0.0f;
            viewport.Width = static_cast<float>(firstBinding->width);
            viewport.Height = static_cast<float>(firstBinding->height);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            D3D12_RECT scissor{};
            scissor.left = 0;
            scissor.top = 0;
            scissor.right = static_cast<LONG>(firstBinding->width);
            scissor.bottom = static_cast<LONG>(firstBinding->height);
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
        }
        commandList->OMSetRenderTargets(
            static_cast<UINT>(rtvs.size()),
            rtvs.data(),
            FALSE,
            dsv);
    }
}

void RenderGraph::EmitUavBarriers(
    ID3D12GraphicsCommandList* commandList,
    const RenderPassDesc& pass) const {
    if (commandList == nullptr) {
        return;
    }

    std::unordered_set<std::string> emitted;
    for (const RenderPassResourceAccess& access : pass.accesses) {
        if (access.type != RenderResourceAccessType::WriteUav || access.resource.empty()) {
            continue;
        }

        const std::string stateKey = ResolveStateKey(access.resource);
        if (emitted.contains(stateKey)) {
            continue;
        }

        const auto found = resources_.find(stateKey);
        if (found == resources_.end() || found->second.resource == nullptr) {
            continue;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = found->second.resource;
        commandList->ResourceBarrier(1, &barrier);
        emitted.insert(stateKey);
    }
}

void RenderGraph::TransitionResource(
    ID3D12GraphicsCommandList* commandList,
    const std::string& name,
    D3D12_RESOURCE_STATES after) const {
    if (commandList == nullptr || name.empty()) {
        return;
    }

    const std::string stateKey = ResolveStateKey(name);
    auto found = resources_.find(stateKey);
    if (found == resources_.end() || found->second.resource == nullptr ||
        found->second.state == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = found->second.resource;
    barrier.Transition.StateBefore = found->second.state;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    found->second.state = after;
    if (stateChanged_) {
        stateChanged_(stateKey, after);
    }
}

std::string RenderGraph::ResolveStateKey(std::string_view name) const {
    const auto found = resourceStateKeys_.find(std::string(name));
    if (found != resourceStateKeys_.end() && !found->second.empty()) {
        return found->second;
    }
    return std::string(name);
}

std::string RenderGraph::Describe() const {
    std::ostringstream stream;
    const std::vector<RenderPassDebugInfo> passDebugInfo = BuildPassDebugInfo();
    for (const RenderPassDebugInfo& pass : passDebugInfo) {
        stream << (pass.executed ? "[ON]  " : "[OFF] ") <<
            ToString(pass.layer) << "." << pass.name;
        if (pass.executionIndex >= 0) {
            stream << " #" << pass.executionIndex;
        }
        if (!pass.reason.empty()) {
            stream << " {" << pass.reason << "}";
        }
        if (!pass.accesses.empty()) {
            stream << " accesses(";
            for (size_t i = 0; i < pass.accesses.size(); ++i) {
                if (i != 0) {
                    stream << ", ";
                }
                const RenderPassResourceAccess& access = pass.accesses[i];
                stream << AccessTypeLabel(access.type) << ":" << access.resource;
            }
            stream << ")";
        }
        if (!pass.depthTarget.empty()) {
            stream << " depth(" << pass.depthTarget << ")";
        }
        stream << "\n";
    }
    const std::vector<TransientRenderTargetDesc> transientPlan = BuildTransientRenderTargetPlan();
    if (!transientPlan.empty()) {
        std::unordered_set<std::string> transientStorages;
        std::vector<std::string> transientEntries;
        std::vector<std::string> persistentEntries;
        for (const TransientRenderTargetDesc& desc : transientPlan) {
            std::ostringstream entry;
            entry << desc.name << "->" << desc.storageName <<
                "[" << desc.lifetimeBegin << "-" << desc.lifetimeEnd << "]";
            if (desc.transient) {
                transientStorages.insert(desc.storageName);
                transientEntries.push_back(entry.str());
            } else {
                persistentEntries.push_back(entry.str());
            }
        }
        if (!transientEntries.empty()) {
            stream << "TransientTargets(";
            for (size_t i = 0; i < transientEntries.size(); ++i) {
                if (i != 0) {
                    stream << ", ";
                }
                stream << transientEntries[i];
            }
            stream << ") storages=" << transientStorages.size() << "\n";
        }
        if (!persistentEntries.empty()) {
            stream << "PersistentTargets(";
            for (size_t i = 0; i < persistentEntries.size(); ++i) {
                if (i != 0) {
                    stream << ", ";
                }
                stream << persistentEntries[i];
            }
            stream << ")\n";
        }
    }
    const std::vector<TransientBufferDesc> bufferPlan = BuildTransientBufferPlan();
    if (!bufferPlan.empty()) {
        std::unordered_set<std::string> transientStorages;
        std::vector<std::string> transientEntries;
        std::vector<std::string> persistentEntries;
        for (const TransientBufferDesc& desc : bufferPlan) {
            std::ostringstream entry;
            entry << desc.name << "->" << desc.storageName <<
                "[" << desc.lifetimeBegin << "-" << desc.lifetimeEnd << "]";
            if (desc.transient) {
                transientStorages.insert(desc.storageName);
                transientEntries.push_back(entry.str());
            } else {
                persistentEntries.push_back(entry.str());
            }
        }
        if (!transientEntries.empty()) {
            stream << "TransientBuffers(";
            for (size_t i = 0; i < transientEntries.size(); ++i) {
                if (i != 0) {
                    stream << ", ";
                }
                stream << transientEntries[i];
            }
            stream << ") storages=" << transientStorages.size() << "\n";
        }
        if (!persistentEntries.empty()) {
            stream << "PersistentBuffers(";
            for (size_t i = 0; i < persistentEntries.size(); ++i) {
                if (i != 0) {
                    stream << ", ";
                }
                stream << persistentEntries[i];
            }
            stream << ")\n";
        }
    }
    if (!depthTargetDeclarations_.empty()) {
        stream << "DepthTargets(";
        size_t index = 0;
        for (const auto& [name, desc] : depthTargetDeclarations_) {
            (void)desc;
            if (index++ != 0) {
                stream << ", ";
            }
            stream << name;
        }
        stream << ")\n";
    }
    return stream.str();
}

bool RenderGraph::Validate(std::string* error) const {
    std::set<std::string> produced;
    for (const RenderPassDesc& pass : passes_) {
        if (!pass.depthTarget.empty() && !depthTargetDeclarations_.contains(pass.depthTarget)) {
            if (error != nullptr) {
                *error = "RenderGraph pass '" + pass.name +
                    "' references undeclared depth target: " + pass.depthTarget;
            }
            return false;
        }
        for (const RenderPassResourceAccess& access : pass.accesses) {
            if (access.resource.empty()) {
                continue;
            }
            if (IsReadAccess(access.type) &&
                !produced.contains(access.resource) &&
                !resources_.contains(ResolveStateKey(access.resource)) &&
                !transientBuffers_.contains(access.resource) &&
                !depthTargetDeclarations_.contains(access.resource) &&
                access.resource != "ParticleState") {
                if (error != nullptr) {
                    *error = "RenderGraph pass '" + pass.name +
                        "' reads resource before it is produced: " + access.resource;
                }
                return false;
            }
            if (access.type == RenderResourceAccessType::WriteRtv &&
                !resources_.contains(ResolveStateKey(access.resource)) &&
                !transientRenderTargets_.contains(access.resource) &&
                access.resource != backBufferResource_) {
                if (error != nullptr) {
                    *error = "RenderGraph pass '" + pass.name +
                        "' writes undeclared render target: " + access.resource;
                }
                return false;
            }
            if (access.type == RenderResourceAccessType::WriteUav &&
                !resources_.contains(ResolveStateKey(access.resource)) &&
                !transientBuffers_.contains(access.resource)) {
                if (error != nullptr) {
                    *error = "RenderGraph pass '" + pass.name +
                        "' writes undeclared UAV resource: " + access.resource;
                }
                return false;
            }
            if (access.type == RenderResourceAccessType::WriteDepth &&
                !resources_.contains(ResolveStateKey(access.resource)) &&
                !depthTargetDeclarations_.contains(access.resource)) {
                if (error != nullptr) {
                    *error = "RenderGraph pass '" + pass.name +
                        "' writes undeclared depth resource: " + access.resource;
                }
                return false;
            }
            if (IsWriteAccess(access.type)) {
                produced.insert(access.resource);
            }
        }
    }
    return true;
}

void RenderGraph::SetBackBufferResource(std::string_view name) {
    backBufferResource_ = std::string(name);
}

const char* ToString(RenderPassLayer layer) {
    switch (layer) {
    case RenderPassLayer::Geometry: return "Geometry";
    case RenderPassLayer::Lighting: return "Lighting";
    case RenderPassLayer::Vfx: return "VFX";
    case RenderPassLayer::PostProcess: return "PostProcess";
    case RenderPassLayer::Ui: return "UI";
    case RenderPassLayer::Debug: return "Debug";
    }
    return "Unknown";
}

} // namespace ge3::graphics
