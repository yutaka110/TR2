#include "EffectSystem.h"

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "vfx/VfxRenderInputs.h"

void EffectRuntimeFrame::Clear() {
    particleQueue.clear();
    trailQueue.clear();
    beamQueue.clear();
    distortionQueue.clear();
    activeInstanceCount = 0;
    activeComponentCount = 0;
}

namespace {
VfxComponentInputCommon MakeComponentInputCommon(const EffectRenderItemCommon& item) {
    return {
        &item,
        item.asset,
        item.componentCommon,
        item.instance,
        item.componentInstance,
        item.normalizedAge,
        item.renderQueue
    };
}

using ComponentNormalizationBuffer = std::variant<
    ParticleComponentAsset,
    TrailComponentAsset,
    BeamComponentAsset,
    DistortionComponentAsset>;

EffectComponentCommon& ComponentCommon(ComponentNormalizationBuffer& component) {
    return std::visit(
        [](auto& typedComponent) -> EffectComponentCommon& {
            return typedComponent.common;
        },
        component);
}

ComponentNormalizationBuffer MakeComponentBufferForNormalization(
    const EffectAsset& asset,
    const EffectComponentAsset& component) {
    switch (component.common.type) {
    case EffectComponentType::Particle:
        if (const ParticleComponentAssetView particle = ParticleComponentView(component)) {
            return ParticleComponentAsset{*particle.common, *particle.settings};
        }
        return EffectComponentAssetBuilder::MakeParticle(asset, component.common);
    case EffectComponentType::Trail:
        if (const TrailComponentAssetView trail = TrailComponentView(component)) {
            return TrailComponentAsset{*trail.common, *trail.settings};
        }
        return EffectComponentAssetBuilder::MakeTrail(asset, component.common);
    case EffectComponentType::Beam:
        if (const BeamComponentAssetView beam = BeamComponentView(component)) {
            return BeamComponentAsset{*beam.common, *beam.settings};
        }
        return EffectComponentAssetBuilder::MakeBeam(asset, component.common);
    case EffectComponentType::Distortion:
        if (const DistortionComponentAssetView distortion = DistortionComponentView(component)) {
            return DistortionComponentAsset{*distortion.common, *distortion.settings};
        }
        return EffectComponentAssetBuilder::MakeDistortion(asset, component.common);
    }
    return EffectComponentAssetBuilder::MakeParticle(asset, component.common);
}

void NormalizeComponentCommon(
    const EffectAsset& asset,
    uint32_t index,
    EffectComponentCommon& common) {
    if (common.id == 0) {
        common.id = index + 1;
    }
    if (common.name.empty()) {
        common.name = asset.name + "_component_" + std::to_string(common.id);
    }
    if (common.duration <= 0.0f) {
        common.duration = asset.lifetime;
    }
    if (common.type == EffectComponentType::Trail) {
        common.technique = EffectTechnique::TrailRibbon;
        common.rendererType = EffectRendererType::TrailRenderer;
        common.simulationType = EffectSimulationType::CpuTimeline;
    } else if (common.type == EffectComponentType::Beam) {
        common.technique = EffectTechnique::BeamLightning;
        common.rendererType = EffectRendererType::BeamRenderer;
        common.simulationType = EffectSimulationType::CpuTimeline;
    } else if (common.type == EffectComponentType::Distortion) {
        common.technique = EffectTechnique::DistortionSprite;
        common.rendererType = EffectRendererType::DistortionRenderer;
        common.simulationType = EffectSimulationType::None;
    }
}

} // namespace

ParticleRenderFallback EffectRuntimeFrame::PrimaryParticleFallback() const {
    if (particleQueue.empty()) {
        return {};
    }

    return {
        particleQueue.front().common.componentCommon,
        particleQueue.front().settings
    };
}

ParticleRenderInput EffectRuntimeFrame::ParticleInput(const ParticleRenderFallback& fallback) const {
    ParticleRenderInput input{};
    input.fallbackCommon = fallback.common;
    input.fallbackSettings = fallback.settings;
    if (!particleQueue.empty()) {
        input.primary = MakeComponentInputCommon(particleQueue.front().common);
        input.settings = particleQueue.front().settings;
    }
    return input;
}

TrailRenderInput EffectRuntimeFrame::TrailInput() const {
    TrailRenderInput input{};
    if (!trailQueue.empty()) {
        input.primary = MakeComponentInputCommon(trailQueue.front().common);
        input.settings = trailQueue.front().settings;
    }
    return input;
}

BeamRenderInput EffectRuntimeFrame::BeamInput() const {
    if (beamQueue.empty()) {
        return {};
    }

    return {
        MakeComponentInputCommon(beamQueue.front().common),
        beamQueue.front().settings
    };
}

DistortionRenderInput EffectRuntimeFrame::DistortionInput() const {
    DistortionRenderInput input{};
    if (!distortionQueue.empty()) {
        input.primary = MakeComponentInputCommon(distortionQueue.front().common);
        input.settings = distortionQueue.front().settings;
    }
    return input;
}

void EffectSystem::RegisterAsset(EffectAsset asset) {
    if (asset.name.empty()) {
        return;
    }
    EnsureDefaultComponent(asset);
    const std::string assetName = asset.name;
    assets_[assetName] = std::move(asset);

    for (EffectInstance& instance : instances_) {
        if (instance.assetName == assetName) {
            instance.asset = FindAsset(assetName);
        }
    }
}

const EffectAsset* EffectSystem::FindAsset(std::string_view name) const {
    const auto found = assets_.find(std::string(name));
    if (found == assets_.end()) {
        return nullptr;
    }
    return &found->second;
}

uint32_t EffectSystem::PlayEffect(std::string_view name, const Vector3& position) {
    return PlayEffectWithParams(
        name,
        position,
        {1.0f, 1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f});
}

uint32_t EffectSystem::PlayEffectWithParams(
    std::string_view name,
    const Vector3& position,
    const Vector4& color,
    const Vector3& scale) {
    const EffectAsset* asset = FindAsset(name);
    if (asset == nullptr) {
        return 0;
    }

    EffectInstance instance{};
    instance.id = nextInstanceId_++;
    instance.assetName = asset->name;
    instance.asset = asset;
    instance.components.reserve(asset->Components().ComponentCount());
    asset->Components().ForEachComponentCommon([&instance](const EffectComponentCommon& component) {
        instance.components.push_back({component.id, 0.0f, true});
    });
    instance.transform.translate = position;
    instance.transform.rotate = {0.0f, 0.0f, 0.0f};
    instance.transform.scale = {
        asset->size.x * scale.x,
        asset->size.y * scale.y,
        asset->size.z * scale.z,
    };
    instance.color = {
        asset->color.x * color.x,
        asset->color.y * color.y,
        asset->color.z * color.z,
        asset->color.w * color.w,
    };
    instances_.push_back(instance);
    return instance.id;
}

void EffectSystem::StopEffect(uint32_t id) {
    instances_.erase(
        std::remove_if(
            instances_.begin(),
            instances_.end(),
            [id](const EffectInstance& instance) {
                return instance.id == id;
            }),
        instances_.end());
}

void EffectSystem::Update(float deltaTime) {
    for (EffectInstance& instance : instances_) {
        instance.age += deltaTime;
        for (EffectComponentInstance& component : instance.components) {
            component.age += deltaTime;
        }
    }

    instances_.erase(
        std::remove_if(
            instances_.begin(),
            instances_.end(),
            [](const EffectInstance& instance) {
                float totalLifetime = instance.asset != nullptr ? instance.asset->lifetime : 0.0f;
                if (instance.asset != nullptr) {
                    instance.asset->Components().ForEachComponentCommon(
                        [&totalLifetime](const EffectComponentCommon& component) {
                            const float componentEnd = component.startTime + component.duration;
                            if (componentEnd > totalLifetime) {
                                totalLifetime = componentEnd;
                            }
                        });
                }
                return instance.asset == nullptr ||
                       (totalLifetime > 0.0f && instance.age >= totalLifetime);
            }),
        instances_.end());
}

void EffectSystem::ClearInstances() {
    instances_.clear();
}

EffectInstance* EffectSystem::FindInstance(uint32_t id) {
    for (EffectInstance& instance : instances_) {
        if (instance.id == id) {
            return &instance;
        }
    }
    return nullptr;
}

void EffectSystem::RestartInstance(uint32_t id) {
    EffectInstance* instance = FindInstance(id);
    if (instance != nullptr) {
        instance->age = 0.0f;
        for (EffectComponentInstance& component : instance->components) {
            component.age = 0.0f;
            component.active = true;
        }
    }
}

void EffectSystem::EnsureDefaultComponent(EffectAsset& asset) {
    EffectAssetComponentStorage& components = asset.MutableComponents();
    if (components.HasPackedComponentsForNormalization()) {
        for (uint32_t index = 0; index < components.PackedComponentCountForNormalization(); ++index) {
            ComponentNormalizationBuffer component =
                MakeComponentBufferForNormalization(asset, components.PackedComponentAtForNormalization(index));
            NormalizeComponentCommon(asset, index, ComponentCommon(component));
            std::visit(
                [&components, index](const auto& typedComponent) {
                    using Component = std::decay_t<decltype(typedComponent)>;
                    if constexpr (std::is_same_v<Component, ParticleComponentAsset>) {
                        components.ReplaceParticleComponentAtForAuthoring(index, typedComponent);
                    } else if constexpr (std::is_same_v<Component, TrailComponentAsset>) {
                        components.ReplaceTrailComponentAtForAuthoring(index, typedComponent);
                    } else if constexpr (std::is_same_v<Component, BeamComponentAsset>) {
                        components.ReplaceBeamComponentAtForAuthoring(index, typedComponent);
                    } else if constexpr (std::is_same_v<Component, DistortionComponentAsset>) {
                        components.ReplaceDistortionComponentAtForAuthoring(index, typedComponent);
                    }
                },
                component);
        }
        components.SyncTypedStorageFromPackedForNormalization();
        return;
    }

    EffectComponentCommon common = EffectComponentAssetBuilder::MakeCommon(
        asset,
        EffectComponentType::Particle,
        1);
    common.name = asset.name + "_particle";
    components.Add(EffectComponentAssetBuilder::MakeParticle(asset, common));
}
