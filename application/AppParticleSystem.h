#pragma once

#include <cstdint>
#include <d3d12.h>
#include <random>
#include <vector>

#include "wrl.h"
#include "utils/math/MathUtils.h"

struct InstanceTransform {
    Vector3 scale;
    Vector3 rotate;
    Vector3 translate;
};

struct Particle {
    Transform transform;
    Vector3 velocity;
    Vector4 color;
    float lifeTime;
    float currentTime;
};

struct ParticleForGPU {
    Matrix4x4 WVP;
    Matrix4x4 World;
    Vector4 color;
};

struct Emitter {
    Transform transform;
    uint32_t count;
    float frequency;
    float frequencyTime;
};

struct AABB {
    Vector3 min;
    Vector3 max;
};

struct AccelerationField {
    Vector3 acceleration;
    AABB area;
};

inline bool IsCollision(const AABB& aabb, const Vector3& p) {
    if (p.x < aabb.min.x || p.x > aabb.max.x) {
        return false;
    }
    if (p.y < aabb.min.y || p.y > aabb.max.y) {
        return false;
    }
    if (p.z < aabb.min.z || p.z > aabb.max.z) {
        return false;
    }
    return true;
}

class AppParticleSystem {
public:
    static constexpr uint32_t kDefaultMaxInstances = 10;

    AppParticleSystem();

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateInstancingResource(ID3D12Device* device) const;
    void InitializeInstancingBuffer(ID3D12Resource* instancingResource, uint32_t maxInstances);
    void CreateInstancingSrv(
        ID3D12Device* device,
        ID3D12Resource* instancingResource,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
    D3D12_GPU_DESCRIPTOR_HANDLE InstancingSrvGpuHandle() const;
    uint32_t MaxInstances() const;

    void SetAccelerationField(const AccelerationField& accelerationField);
    void AddParticle(const Vector3& baseTranslate);
    void Emit(const Emitter& emitter);
    uint32_t UpdateInstances(const Matrix4x4& viewProj, float deltaTime);

private:
    Particle MakeNewParticle();
    Particle MakeNewParticle(const Vector3& baseTranslate);

    std::vector<Particle> particles_{};
    std::random_device seedGenerator_{};
    std::mt19937 randomEngine_;
    ParticleForGPU* instancingData_ = nullptr;
    uint32_t maxInstances_ = kDefaultMaxInstances;
    AccelerationField accelerationField_{};
    D3D12_GPU_DESCRIPTOR_HANDLE instancingSrvGpuHandle_{};
};
