#include "AppGpuParticleSystem.h"

#include <cstring>
#include <vector>

namespace {
struct ParticleForGpuLayout {
    Matrix4x4 WVP;
    Matrix4x4 World;
    Vector4 color;
};

Microsoft::WRL::ComPtr<ID3D12Resource> CreateDefaultBuffer(
    ID3D12Device* device,
    size_t size,
    D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (size + 0xFF) & ~static_cast<size_t>(0xFF);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&resource)))) {
        return nullptr;
    }
    return resource;
}

Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, size_t size) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (size + 0xFF) & ~static_cast<size_t>(0xFF);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(
            &heap,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&resource)))) {
        return nullptr;
    }
    return resource;
}

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
} // namespace

size_t AppGpuParticleSystem::ParticleRenderBufferBytes(uint32_t maxParticles) {
    return sizeof(ParticleForGpuLayout) * maxParticles;
}

size_t AppGpuParticleSystem::ParticleStateBytes(uint32_t maxParticles) {
    return sizeof(ParticleState) * maxParticles;
}

bool AppGpuParticleSystem::CreateParticleOutputViews(ID3D12Device* device) {
    if (device == nullptr || particleOutput_ == nullptr || !particleSrv_.IsValid() || !particleUav_.IsValid()) {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = maxParticles_;
    srvDesc.Buffer.StructureByteStride = sizeof(ParticleForGpuLayout);
    device->CreateShaderResourceView(particleOutput_.Get(), &srvDesc, particleSrv_.cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav{};
    outputUav.Format = DXGI_FORMAT_UNKNOWN;
    outputUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    outputUav.Buffer.NumElements = maxParticles_;
    outputUav.Buffer.StructureByteStride = sizeof(ParticleForGpuLayout);
    device->CreateUnorderedAccessView(particleOutput_.Get(), nullptr, &outputUav, particleUav_.cpu);
    return true;
}

bool AppGpuParticleSystem::Initialize(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    ge3::core::DescriptorHeapSet& heaps,
    uint32_t maxParticles) {
    if (initialized_) {
        return true;
    }
    if (device == nullptr || commandList == nullptr || maxParticles == 0) {
        return false;
    }

    maxParticles_ = maxParticles;
    const size_t outputBytes = ParticleRenderBufferBytes(maxParticles_);
    const size_t stateBytes = ParticleStateBytes(maxParticles_);

    particleState_ = CreateDefaultBuffer(
        device,
        stateBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    uploadState_ = CreateUploadBuffer(device, stateBytes);
    indirectArgs_ = CreateDefaultBuffer(device, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), D3D12_RESOURCE_FLAG_NONE);
    uploadIndirectArgs_ = CreateUploadBuffer(device, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
    if (particleState_ == nullptr ||
        uploadState_ == nullptr || indirectArgs_ == nullptr || uploadIndirectArgs_ == nullptr) {
        return false;
    }

    std::vector<ParticleState> initial(maxParticles_);
    for (uint32_t i = 0; i < maxParticles_; ++i) {
        const float seed = static_cast<float>((i * 1664525u + 1013904223u) & 0xffffu) / 65535.0f;
        initial[i].position = {
            (static_cast<float>(i % 256) / 255.0f - 0.5f) * 8.0f,
            (static_cast<float>((i / 256) % 256) / 255.0f - 0.5f) * 4.0f,
            2.0f + seed * 4.0f};
        initial[i].velocity = {
            seed * 0.6f - 0.3f,
            0.4f + seed * 0.8f,
            seed * 0.4f - 0.2f};
        initial[i].lifetime = 2.0f + seed * 3.0f;
        initial[i].age = seed * initial[i].lifetime;
        initial[i].color = {0.25f + seed * 0.75f, 0.55f, 1.0f, 1.0f};
        initial[i].scale = {0.08f + seed * 0.08f, 0.08f + seed * 0.08f, 1.0f};
        initial[i].seed = seed;
    }

    void* mapped = nullptr;
    if (SUCCEEDED(uploadState_->Map(0, nullptr, &mapped))) {
        std::memcpy(mapped, initial.data(), stateBytes);
        uploadState_->Unmap(0, nullptr);
    }
    D3D12_RESOURCE_BARRIER stateCopy =
        MakeTransition(particleState_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->ResourceBarrier(1, &stateCopy);
    particleStateState_ = D3D12_RESOURCE_STATE_COPY_DEST;

    commandList->CopyBufferRegion(particleState_.Get(), 0, uploadState_.Get(), 0, stateBytes);
    D3D12_RESOURCE_BARRIER stateReady =
        MakeTransition(particleState_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->ResourceBarrier(1, &stateReady);
    particleStateState_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    particleOutputState_ = D3D12_RESOURCE_STATE_COMMON;

    D3D12_DRAW_INDEXED_ARGUMENTS args{};
    args.IndexCountPerInstance = 6;
    args.InstanceCount = maxParticles_;
    void* argsMapped = nullptr;
    if (SUCCEEDED(uploadIndirectArgs_->Map(0, nullptr, &argsMapped))) {
        std::memcpy(argsMapped, &args, sizeof(args));
        uploadIndirectArgs_->Unmap(0, nullptr);
    }
    D3D12_RESOURCE_BARRIER argsCopy =
        MakeTransition(indirectArgs_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->ResourceBarrier(1, &argsCopy);
    indirectArgsState_ = D3D12_RESOURCE_STATE_COPY_DEST;
    commandList->CopyBufferRegion(indirectArgs_.Get(), 0, uploadIndirectArgs_.Get(), 0, sizeof(args));
    D3D12_RESOURCE_BARRIER argsReady =
        MakeTransition(indirectArgs_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    commandList->ResourceBarrier(1, &argsReady);
    indirectArgsState_ = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;

    particleSrv_ = heaps.srv.Allocate();
    particleUav_ = heaps.srv.Allocate();
    stateUav_ = heaps.srv.Allocate();

    D3D12_UNORDERED_ACCESS_VIEW_DESC stateUav{};
    stateUav.Format = DXGI_FORMAT_UNKNOWN;
    stateUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    stateUav.Buffer.NumElements = maxParticles_;
    stateUav.Buffer.StructureByteStride = sizeof(ParticleState);
    device->CreateUnorderedAccessView(particleState_.Get(), nullptr, &stateUav, stateUav_.cpu);

    D3D12_INDIRECT_ARGUMENT_DESC argumentDesc{};
    argumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC signatureDesc{};
    signatureDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    signatureDesc.NumArgumentDescs = 1;
    signatureDesc.pArgumentDescs = &argumentDesc;
    if (FAILED(device->CreateCommandSignature(
            &signatureDesc,
            nullptr,
            IID_PPV_ARGS(&commandSignature_)))) {
        return false;
    }

    initialized_ = true;
    return true;
}

void AppGpuParticleSystem::DeclareGraphBuffers(ge3::graphics::RenderGraph& renderGraph) const {
    if (!initialized_ || maxParticles_ == 0) {
        return;
    }

    renderGraph.DeclarePersistentBuffer(
        "ParticleState",
        ParticleStateBytes(maxParticles_),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    renderGraph.DeclareTransientBuffer(
        "ParticleRenderBuffer",
        ParticleRenderBufferBytes(maxParticles_),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COMMON);
    renderGraph.DeclarePersistentBuffer(
        "ParticleIndirectArgs",
        sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
        D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
}

bool AppGpuParticleSystem::EnsureGraphBuffers(
    ID3D12Device* device,
    const ge3::graphics::RenderGraph& renderGraph) {
    if (!initialized_ || device == nullptr) {
        return false;
    }

    const std::vector<ge3::graphics::TransientBufferDesc> bufferPlan =
        renderGraph.BuildTransientBufferPlan();
    for (const ge3::graphics::TransientBufferDesc& buffer : bufferPlan) {
        if (buffer.name != "ParticleRenderBuffer") {
            continue;
        }
        if (particleOutput_ != nullptr &&
            particleOutputBytes_ == buffer.sizeInBytes) {
            return true;
        }

        particleOutput_ = CreateDefaultBuffer(device, buffer.sizeInBytes, buffer.flags);
        if (particleOutput_ == nullptr) {
            particleOutputBytes_ = 0;
            return false;
        }
        particleOutputBytes_ = buffer.sizeInBytes;
        particleOutputState_ = buffer.initialState;
        return CreateParticleOutputViews(device);
    }

    return particleOutput_ != nullptr;
}

void AppGpuParticleSystem::Simulate(
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
    float uvScrollSpeed) {
    if (!initialized_ || commandList == nullptr || rootSignature == nullptr || pipelineState == nullptr) {
        return;
    }
    if (particleOutput_ == nullptr || particleState_ == nullptr) {
        return;
    }

    struct Constants {
        Matrix4x4 viewProjection;
        float deltaTime;
        float time;
        uint32_t maxParticles;
        float pad;
        Vector4 tint;
        Vector4 scaleAndParams;
        Vector4 effectParams;
    } constants{};
    constants.viewProjection = viewProjection;
    constants.deltaTime = deltaTime;
    constants.time = time;
    constants.maxParticles = maxParticles_;
    constants.tint = tint;
    constants.scaleAndParams = {scale.x, scale.y, emissive, turbulence};
    constants.effectParams = {pulseSpeed, spawnRadius, uvScrollSpeed, 0.0f};

    if (particleOutputState_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER outputReady =
            MakeTransition(particleOutput_.Get(), particleOutputState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &outputReady);
        particleOutputState_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (particleStateState_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER stateReady =
            MakeTransition(particleState_.Get(), particleStateState_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &stateReady);
        particleStateState_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    commandList->SetComputeRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->SetComputeRoot32BitConstants(0, 32, &constants, 0);
    commandList->SetComputeRootDescriptorTable(1, particleUav_.gpu);
    commandList->SetComputeRootDescriptorTable(2, stateUav_.gpu);
    commandList->Dispatch((maxParticles_ + 255) / 256, 1, 1);
}

void AppGpuParticleSystem::RegisterGraphResources(ge3::graphics::RenderGraph& renderGraph) const {
    if (!initialized_ || particleOutput_ == nullptr) {
        return;
    }

    renderGraph.RegisterResource(
        "ParticleRenderBuffer",
        particleOutput_.Get(),
        particleOutputState_);
    renderGraph.RegisterResource(
        "DistortionRenderBuffer",
        particleOutput_.Get(),
        particleOutputState_,
        "ParticleRenderBuffer");
    renderGraph.RegisterResource(
        "ParticleState",
        particleState_.Get(),
        particleStateState_);
    renderGraph.RegisterResource(
        "ParticleIndirectArgs",
        indirectArgs_.Get(),
        indirectArgsState_);
    renderGraph.RegisterResource(
        "DistortionIndirectArgs",
        indirectArgs_.Get(),
        indirectArgsState_,
        "ParticleIndirectArgs");
}

D3D12_GPU_DESCRIPTOR_HANDLE AppGpuParticleSystem::SrvHandleForResource(std::string_view name) const {
    if (name == "ParticleRenderBuffer" || name == "DistortionRenderBuffer") {
        return particleSrv_.gpu;
    }
    return {};
}

ID3D12Resource* AppGpuParticleSystem::IndirectArgsForResource(std::string_view name) const {
    if (name == "ParticleIndirectArgs" || name == "DistortionIndirectArgs") {
        return indirectArgs_.Get();
    }
    return nullptr;
}

void AppGpuParticleSystem::SetResourceState(std::string_view name, D3D12_RESOURCE_STATES state) {
    if (name == "ParticleRenderBuffer") {
        particleOutputState_ = state;
    } else if (name == "ParticleState") {
        particleStateState_ = state;
    } else if (name == "ParticleIndirectArgs") {
        indirectArgsState_ = state;
    }
}
