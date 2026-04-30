#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "graphics/MaterialBinding.h"
#include "utils/math/MathUtils.h"

enum class EffectLayer {
    OpaqueFx,
    AdditiveFx,
    DistortionFx,
    TrailFx,
    VolumetricFx,
};

enum class EffectComponentType {
    Particle,
    Trail,
    Beam,
    Distortion,
};

enum class EffectTechnique {
    ParticleAdditive,
    TrailRibbon,
    BeamLightning,
    DistortionSprite,
};

enum class EffectRendererType {
    ParticleRenderer,
    TrailRenderer,
    BeamRenderer,
    DistortionRenderer,
};

enum class EffectSimulationType {
    CpuSpawnGpuSim,
    CpuTimeline,
    GpuSimulation,
    None,
};

struct EffectComponentCommon {
    uint32_t id = 0;
    EffectComponentType type = EffectComponentType::Particle;
    EffectTechnique technique = EffectTechnique::ParticleAdditive;
    EffectRendererType rendererType = EffectRendererType::ParticleRenderer;
    EffectSimulationType simulationType = EffectSimulationType::CpuSpawnGpuSim;
    std::string name;
    std::string shader;
    std::string texture;
    ge3::graphics::PassState passState{};
    EffectLayer layer = EffectLayer::AdditiveFx;
    float startTime = 0.0f;
    float duration = 1.0f;
    Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f};
    Vector3 size = {1.0f, 1.0f, 1.0f};
};

struct EffectParticleSettings {
    float emissive = 1.0f;
    float distortionStrength = 0.0f;
    float noiseStrength = 0.0f;
    float uvScrollSpeed = 0.0f;
    float pulseSpeed = 5.0f;
    float spawnRadius = 4.0f;
    float depthFadeSoftness = 0.02f;
    float edgeSoftness = 0.5f;
};

struct EffectTrailSettings {
    float depthFadeSoftness = 0.02f;
    float trailTailFade = 1.35f;
};

struct EffectBeamSettings {
    float emissive = 1.0f;
};

struct EffectDistortionSettings {
    float strength = 0.0f;
    float noiseStrength = 0.0f;
    float uvScrollSpeed = 0.0f;
    float depthFadeSoftness = 0.02f;
    float depthAttenuation = 1.0f;
};

struct EffectComponentPayload {
    using Variant = std::variant<
        EffectParticleSettings,
        EffectTrailSettings,
        EffectBeamSettings,
        EffectDistortionSettings>;

    Variant data{EffectParticleSettings{}};
};

struct EffectComponentAsset {
    // Full component storage is an authoring/registry boundary. Runtime queues,
    // renderer inputs, and renderers should consume typed views or typed items.
    EffectComponentCommon common{};
    EffectComponentPayload payload{};
};

struct ParticleComponentAsset {
    EffectComponentCommon common{};
    EffectParticleSettings settings{};
};

struct TrailComponentAsset {
    EffectComponentCommon common{};
    EffectTrailSettings settings{};
};

struct BeamComponentAsset {
    EffectComponentCommon common{};
    EffectBeamSettings settings{};
};

struct DistortionComponentAsset {
    EffectComponentCommon common{};
    EffectDistortionSettings settings{};
};

inline EffectComponentAsset ToEffectComponentAsset(const ParticleComponentAsset& component) {
    EffectComponentAsset asset{};
    asset.common = component.common;
    asset.common.type = EffectComponentType::Particle;
    asset.payload.data = component.settings;
    return asset;
}

inline EffectComponentAsset ToEffectComponentAsset(const TrailComponentAsset& component) {
    EffectComponentAsset asset{};
    asset.common = component.common;
    asset.common.type = EffectComponentType::Trail;
    asset.payload.data = component.settings;
    return asset;
}

inline EffectComponentAsset ToEffectComponentAsset(const BeamComponentAsset& component) {
    EffectComponentAsset asset{};
    asset.common = component.common;
    asset.common.type = EffectComponentType::Beam;
    asset.payload.data = component.settings;
    return asset;
}

inline EffectComponentAsset ToEffectComponentAsset(const DistortionComponentAsset& component) {
    EffectComponentAsset asset{};
    asset.common = component.common;
    asset.common.type = EffectComponentType::Distortion;
    asset.payload.data = component.settings;
    return asset;
}

struct ParticleComponentStorageView;
struct MutableParticleComponentStorageView;
struct TrailComponentStorageView;
struct MutableTrailComponentStorageView;
struct BeamComponentStorageView;
struct MutableBeamComponentStorageView;
struct DistortionComponentStorageView;
struct MutableDistortionComponentStorageView;

class EffectAssetComponentStorage {
public:
    using Container = std::vector<EffectComponentAsset>;

    bool Empty() const { return ComponentCount() == 0; }
    std::size_t ComponentCount() const {
        return particleComponents_.size() +
               trailComponents_.size() +
               beamComponents_.size() +
               distortionComponents_.size();
    }
    void Reserve(std::size_t count) { packedComponents_.reserve(count); }

    EffectComponentAsset& Add(EffectComponentAsset component) {
        // Compatibility import path. New component creation should use typed Add overloads.
        MirrorTypedComponent(component);
        packedComponents_.push_back(std::move(component));
        return packedComponents_.back();
    }

    EffectComponentAsset& Add(ParticleComponentAsset component) {
        component.common.type = EffectComponentType::Particle;
        EffectComponentAsset packed = ToEffectComponentAsset(component);
        particleComponents_.push_back(std::move(component));
        packedComponents_.push_back(std::move(packed));
        return packedComponents_.back();
    }

    EffectComponentAsset& Add(TrailComponentAsset component) {
        component.common.type = EffectComponentType::Trail;
        EffectComponentAsset packed = ToEffectComponentAsset(component);
        trailComponents_.push_back(std::move(component));
        packedComponents_.push_back(std::move(packed));
        return packedComponents_.back();
    }

    EffectComponentAsset& Add(BeamComponentAsset component) {
        component.common.type = EffectComponentType::Beam;
        EffectComponentAsset packed = ToEffectComponentAsset(component);
        beamComponents_.push_back(std::move(component));
        packedComponents_.push_back(std::move(packed));
        return packedComponents_.back();
    }

    EffectComponentAsset& Add(DistortionComponentAsset component) {
        component.common.type = EffectComponentType::Distortion;
        EffectComponentAsset packed = ToEffectComponentAsset(component);
        distortionComponents_.push_back(std::move(component));
        packedComponents_.push_back(std::move(packed));
        return packedComponents_.back();
    }

    bool ReplaceParticleComponentAndSyncPacked(ParticleComponentAsset component) {
        component.common.type = EffectComponentType::Particle;
        EffectComponentAsset* packedComponent = Find(component.common.id);
        if (packedComponent == nullptr) {
            return false;
        }
        ReplaceTypedComponent(particleComponents_, component);
        RemoveTypedComponent(trailComponents_, component.common.id);
        RemoveTypedComponent(beamComponents_, component.common.id);
        RemoveTypedComponent(distortionComponents_, component.common.id);
        *packedComponent = ToEffectComponentAsset(component);
        return true;
    }

    bool ReplaceTrailComponentAndSyncPacked(TrailComponentAsset component) {
        component.common.type = EffectComponentType::Trail;
        EffectComponentAsset* packedComponent = Find(component.common.id);
        if (packedComponent == nullptr) {
            return false;
        }
        ReplaceTypedComponent(trailComponents_, component);
        RemoveTypedComponent(particleComponents_, component.common.id);
        RemoveTypedComponent(beamComponents_, component.common.id);
        RemoveTypedComponent(distortionComponents_, component.common.id);
        *packedComponent = ToEffectComponentAsset(component);
        return true;
    }

    bool ReplaceBeamComponentAndSyncPacked(BeamComponentAsset component) {
        component.common.type = EffectComponentType::Beam;
        EffectComponentAsset* packedComponent = Find(component.common.id);
        if (packedComponent == nullptr) {
            return false;
        }
        ReplaceTypedComponent(beamComponents_, component);
        RemoveTypedComponent(particleComponents_, component.common.id);
        RemoveTypedComponent(trailComponents_, component.common.id);
        RemoveTypedComponent(distortionComponents_, component.common.id);
        *packedComponent = ToEffectComponentAsset(component);
        return true;
    }

    bool ReplaceDistortionComponentAndSyncPacked(DistortionComponentAsset component) {
        component.common.type = EffectComponentType::Distortion;
        EffectComponentAsset* packedComponent = Find(component.common.id);
        if (packedComponent == nullptr) {
            return false;
        }
        ReplaceTypedComponent(distortionComponents_, component);
        RemoveTypedComponent(particleComponents_, component.common.id);
        RemoveTypedComponent(trailComponents_, component.common.id);
        RemoveTypedComponent(beamComponents_, component.common.id);
        *packedComponent = ToEffectComponentAsset(component);
        return true;
    }

private:
    friend class EffectSystem;
    friend class EffectAssetLoader;

    bool ReplaceParticleComponentAtForAuthoring(
        std::size_t index,
        ParticleComponentAsset component) {
        if (index >= packedComponents_.size()) {
            return false;
        }
        component.common.type = EffectComponentType::Particle;
        packedComponents_[index] = ToEffectComponentAsset(component);
        return true;
    }

    bool ReplaceTrailComponentAtForAuthoring(
        std::size_t index,
        TrailComponentAsset component) {
        if (index >= packedComponents_.size()) {
            return false;
        }
        component.common.type = EffectComponentType::Trail;
        packedComponents_[index] = ToEffectComponentAsset(component);
        return true;
    }

    bool ReplaceBeamComponentAtForAuthoring(
        std::size_t index,
        BeamComponentAsset component) {
        if (index >= packedComponents_.size()) {
            return false;
        }
        component.common.type = EffectComponentType::Beam;
        packedComponents_[index] = ToEffectComponentAsset(component);
        return true;
    }

    bool ReplaceDistortionComponentAtForAuthoring(
        std::size_t index,
        DistortionComponentAsset component) {
        if (index >= packedComponents_.size()) {
            return false;
        }
        component.common.type = EffectComponentType::Distortion;
        packedComponents_[index] = ToEffectComponentAsset(component);
        return true;
    }

public:
    ParticleComponentStorageView ParticleStorageView() const;
    MutableParticleComponentStorageView MutableParticleStorageView();
    TrailComponentStorageView TrailStorageView() const;
    MutableTrailComponentStorageView MutableTrailStorageView();
    BeamComponentStorageView BeamStorageView() const;
    MutableBeamComponentStorageView MutableBeamStorageView();
    DistortionComponentStorageView DistortionStorageView() const;
    MutableDistortionComponentStorageView MutableDistortionStorageView();

    template <typename Visitor>
    void ForEachComponentCommon(Visitor&& visitor) const {
        for (const ParticleComponentAsset& component : particleComponents_) {
            visitor(component.common);
        }
        for (const TrailComponentAsset& component : trailComponents_) {
            visitor(component.common);
        }
        for (const BeamComponentAsset& component : beamComponents_) {
            visitor(component.common);
        }
        for (const DistortionComponentAsset& component : distortionComponents_) {
            visitor(component.common);
        }
    }

private:
    bool HasPackedComponentsForNormalization() const { return !packedComponents_.empty(); }
    std::size_t PackedComponentCountForNormalization() const { return packedComponents_.size(); }
    const EffectComponentAsset& PackedComponentAtForNormalization(std::size_t index) const {
        return packedComponents_.at(index);
    }

    void SyncTypedStorageFromPackedForNormalization() {
        particleComponents_.clear();
        trailComponents_.clear();
        beamComponents_.clear();
        distortionComponents_.clear();

        for (const EffectComponentAsset& component : packedComponents_) {
            MirrorTypedComponent(component);
        }
    }

    EffectComponentAsset* Find(uint32_t componentId) {
        for (EffectComponentAsset& component : packedComponents_) {
            if (component.common.id == componentId) {
                return &component;
            }
        }
        return nullptr;
    }
    template <typename TypedComponent>
    static void ReplaceTypedComponent(
        std::vector<TypedComponent>& components,
        TypedComponent component) {
        for (TypedComponent& typedComponent : components) {
            if (typedComponent.common.id == component.common.id) {
                typedComponent = std::move(component);
                return;
            }
        }
        components.push_back(std::move(component));
    }

    template <typename TypedComponent>
    static void RemoveTypedComponent(
        std::vector<TypedComponent>& components,
        uint32_t componentId) {
        for (auto it = components.begin(); it != components.end(); ++it) {
            if (it->common.id == componentId) {
                components.erase(it);
                return;
            }
        }
    }

    void MirrorTypedComponent(const EffectComponentAsset& component) {
        switch (component.common.type) {
        case EffectComponentType::Particle:
            if (const auto* settings = std::get_if<EffectParticleSettings>(&component.payload.data)) {
                particleComponents_.push_back({component.common, *settings});
            }
            break;
        case EffectComponentType::Trail:
            if (const auto* settings = std::get_if<EffectTrailSettings>(&component.payload.data)) {
                trailComponents_.push_back({component.common, *settings});
            }
            break;
        case EffectComponentType::Beam:
            if (const auto* settings = std::get_if<EffectBeamSettings>(&component.payload.data)) {
                beamComponents_.push_back({component.common, *settings});
            }
            break;
        case EffectComponentType::Distortion:
            if (const auto* settings = std::get_if<EffectDistortionSettings>(&component.payload.data)) {
                distortionComponents_.push_back({component.common, *settings});
            }
            break;
        }
    }

    Container packedComponents_;
    std::vector<ParticleComponentAsset> particleComponents_;
    std::vector<TrailComponentAsset> trailComponents_;
    std::vector<BeamComponentAsset> beamComponents_;
    std::vector<DistortionComponentAsset> distortionComponents_;
};

struct EffectAsset {
    std::string name;
    std::string shader;
    std::string texture;
    ge3::graphics::PassState passState{};
    EffectLayer layer = EffectLayer::AdditiveFx;
    float lifetime = 1.0f;
    EffectParticleSettings defaultParticle{};
    EffectTrailSettings defaultTrail{};
    EffectBeamSettings defaultBeam{};
    EffectDistortionSettings defaultDistortion{};
    Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f};
    Vector3 size = {1.0f, 1.0f, 1.0f};
    const EffectAssetComponentStorage& Components() const { return components_; }
    // Mutable storage is restricted to asset construction/normalization and
    // typed replacement APIs. Runtime/render paths should use Components().
    EffectAssetComponentStorage& MutableComponents() { return components_; }

private:
    EffectAssetComponentStorage components_;
};

#include "EffectComponentViews.h"

struct EffectComponentInstance {
    uint32_t componentId = 0;
    float age = 0.0f;
    bool active = true;
};

struct EffectInstance {
    uint32_t id = 0;
    std::string assetName;
    const EffectAsset* asset = nullptr;
    std::vector<EffectComponentInstance> components;
    Transform transform{};
    Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f};
    float age = 0.0f;
    bool attached = false;
};

struct EffectEvent {
    std::string effectName;
    Transform transform{};
    Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct EffectRenderItemCommon {
    const EffectAsset* asset = nullptr;
    const EffectComponentCommon* componentCommon = nullptr;
    const EffectInstance* instance = nullptr;
    const EffectComponentInstance* componentInstance = nullptr;
    uint32_t renderQueue = 0;
    float normalizedAge = 0.0f;
};

struct ParticleRenderItem {
    EffectRenderItemCommon common{};
    const EffectParticleSettings* settings = nullptr;
};

struct TrailRenderItem {
    EffectRenderItemCommon common{};
    const EffectTrailSettings* settings = nullptr;
};

struct BeamRenderItem {
    EffectRenderItemCommon common{};
    const EffectBeamSettings* settings = nullptr;
};

struct DistortionRenderItem {
    EffectRenderItemCommon common{};
    const EffectDistortionSettings* settings = nullptr;
};

struct ParticleRenderFallback {
    const EffectComponentCommon* common = nullptr;
    const EffectParticleSettings* settings = nullptr;
};

using ParticleRenderQueue = std::vector<ParticleRenderItem>;
using TrailRenderQueue = std::vector<TrailRenderItem>;
using BeamRenderQueue = std::vector<BeamRenderItem>;
using DistortionRenderQueue = std::vector<DistortionRenderItem>;

struct ParticleRenderInput;
struct TrailRenderInput;
struct BeamRenderInput;
struct DistortionRenderInput;

struct EffectRuntimeFrame {
    ParticleRenderQueue particleQueue;
    TrailRenderQueue trailQueue;
    BeamRenderQueue beamQueue;
    DistortionRenderQueue distortionQueue;
    uint32_t activeInstanceCount = 0;
    uint32_t activeComponentCount = 0;

    void Clear();
    ParticleRenderFallback PrimaryParticleFallback() const;
    ParticleRenderInput ParticleInput(const ParticleRenderFallback& fallback) const;
    TrailRenderInput TrailInput() const;
    BeamRenderInput BeamInput() const;
    DistortionRenderInput DistortionInput() const;
};

class EffectSystem {
public:
    void RegisterAsset(EffectAsset asset);
    const EffectAsset* FindAsset(std::string_view name) const;

    uint32_t PlayEffect(std::string_view name, const Vector3& position);
    uint32_t PlayEffectWithParams(
        std::string_view name,
        const Vector3& position,
        const Vector4& color,
        const Vector3& scale);
    void StopEffect(uint32_t id);
    void Update(float deltaTime);
    void ClearInstances();
    EffectInstance* FindInstance(uint32_t id);
    void RestartInstance(uint32_t id);

    const std::vector<EffectInstance>& Instances() const { return instances_; }
    std::vector<EffectInstance>& MutableInstances() { return instances_; }
    const std::unordered_map<std::string, EffectAsset>& Assets() const { return assets_; }
    std::unordered_map<std::string, EffectAsset>& MutableAssets() { return assets_; }

private:
    static void EnsureDefaultComponent(EffectAsset& asset);

    uint32_t nextInstanceId_ = 1;
    std::unordered_map<std::string, EffectAsset> assets_;
    std::vector<EffectInstance> instances_;
};
