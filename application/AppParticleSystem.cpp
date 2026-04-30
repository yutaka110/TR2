#include "AppParticleSystem.h"

#include <cassert>

#include "utils/dx12/BufferHelper.h"

AppParticleSystem::AppParticleSystem()
    : randomEngine_(seedGenerator_()) {}

Microsoft::WRL::ComPtr<ID3D12Resource> AppParticleSystem::CreateInstancingResource(
    ID3D12Device* device) const {
    return CreateBufferResource(device, sizeof(ParticleForGPU) * maxInstances_);
}

void AppParticleSystem::InitializeInstancingBuffer(
    ID3D12Resource* instancingResource,
    uint32_t maxInstances) {
    maxInstances_ = maxInstances;
    particles_.reserve(maxInstances_);

    ParticleForGPU* instancingData = nullptr;
    HRESULT hr = instancingResource->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&instancingData));
    assert(SUCCEEDED(hr));

    instancingData_ = instancingData;
    instancingSrvGpuHandle_ = {};

    if (instancingData_ == nullptr) {
        return;
    }

    for (uint32_t i = 0; i < maxInstances_; ++i) {
        instancingData_[i].World = MakeIdentity4x4();
        instancingData_[i].WVP = MakeIdentity4x4();
    }
}

void AppParticleSystem::CreateInstancingSrv(
    ID3D12Device* device,
    ID3D12Resource* instancingResource,
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    srvDesc.Buffer.NumElements = maxInstances_;
    srvDesc.Buffer.StructureByteStride = sizeof(ParticleForGPU);

    device->CreateShaderResourceView(instancingResource, &srvDesc, cpuHandle);
    instancingSrvGpuHandle_ = gpuHandle;
}

D3D12_GPU_DESCRIPTOR_HANDLE AppParticleSystem::InstancingSrvGpuHandle() const {
    return instancingSrvGpuHandle_;
}

uint32_t AppParticleSystem::MaxInstances() const {
    return maxInstances_;
}

void AppParticleSystem::SetAccelerationField(const AccelerationField& accelerationField) {
    accelerationField_ = accelerationField;
}

void AppParticleSystem::AddParticle(const Vector3& baseTranslate) {
    particles_.push_back(MakeNewParticle(baseTranslate));
}

void AppParticleSystem::Emit(const Emitter& emitter) {
    for (uint32_t i = 0; i < emitter.count; ++i) {
        Particle particle = MakeNewParticle();
        particle.transform.translate = emitter.transform.translate;
        particles_.push_back(particle);
    }
}

Particle AppParticleSystem::MakeNewParticle() {
    std::uniform_real_distribution<float> distPos(-1.0f, 1.0f);
    std::uniform_real_distribution<float> distVel(-1.0f, 1.0f);
    std::uniform_real_distribution<float> distCol(0.0f, 1.0f);
    std::uniform_real_distribution<float> distTime(1.0f, 3.0f);

    Particle particle{};
    particle.transform.scale = {1.0f, 1.0f, 1.0f};
    particle.transform.rotate = {0.0f, 0.0f, 0.0f};
    particle.transform.translate = {
        distPos(randomEngine_),
        distPos(randomEngine_),
        2.0f + distPos(randomEngine_),
    };
    particle.velocity = {
        distVel(randomEngine_),
        distVel(randomEngine_),
        distVel(randomEngine_),
    };
    particle.color = {
        distCol(randomEngine_),
        distCol(randomEngine_),
        distCol(randomEngine_),
        1.0f,
    };
    particle.lifeTime = distTime(randomEngine_);
    particle.currentTime = 0.0f;
    return particle;
}

Particle AppParticleSystem::MakeNewParticle(const Vector3& baseTranslate) {
    Particle particle = MakeNewParticle();
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::uniform_real_distribution<float> distScale(0.1f, 0.5f);

    float scale = distScale(randomEngine_);
    particle.transform.scale = {scale, scale, 1.0f};
    particle.transform.rotate = {0.0f, 0.0f, 0.0f};
    particle.transform.translate = {
        baseTranslate.x + dist(randomEngine_),
        baseTranslate.y + dist(randomEngine_),
        baseTranslate.z + dist(randomEngine_),
    };
    return particle;
}

uint32_t AppParticleSystem::UpdateInstances(
    const Matrix4x4& viewProj,
    float deltaTime) {
    if (instancingData_ == nullptr) {
        return 0;
    }

    uint32_t numInstances = 0;

    for (auto it = particles_.begin(); it != particles_.end();) {
        if (it->currentTime >= it->lifeTime) {
            it = particles_.erase(it);
            continue;
        }

        if (IsCollision(accelerationField_.area, it->transform.translate)) {
            it->velocity.x += accelerationField_.acceleration.x * deltaTime;
            it->velocity.y += accelerationField_.acceleration.y * deltaTime;
            it->velocity.z += accelerationField_.acceleration.z * deltaTime;
        }

        it->transform.translate.x += it->velocity.x * deltaTime;
        it->transform.translate.y += it->velocity.y * deltaTime;
        it->currentTime += deltaTime;

        if (numInstances >= maxInstances_) {
            ++it;
            continue;
        }

        Matrix4x4 world = MakeAffineMatrix(
            it->transform.scale,
            it->transform.rotate,
            it->transform.translate);
        instancingData_[numInstances].World = world;
        instancingData_[numInstances].WVP = Multiply(world, viewProj);

        float alpha = 1.0f - (it->currentTime / it->lifeTime);
        Vector4 color = it->color;
        color.x *= alpha;
        color.y *= alpha;
        color.z *= alpha;
        color.w = alpha;
        instancingData_[numInstances].color = color;

        ++numInstances;
        ++it;
    }

    return numInstances;
}
