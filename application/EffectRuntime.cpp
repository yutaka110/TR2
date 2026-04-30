#include "EffectRuntime.h"

#include <algorithm>

namespace {
const std::unordered_map<std::string, EffectAsset>& EmptyAssets() {
    static const std::unordered_map<std::string, EffectAsset> assets;
    return assets;
}

std::unordered_map<std::string, EffectAsset>& EmptyMutableAssets() {
    static std::unordered_map<std::string, EffectAsset> assets;
    assets.clear();
    return assets;
}

const std::vector<EffectInstance>& EmptyInstances() {
    static const std::vector<EffectInstance> instances;
    return instances;
}

std::vector<EffectInstance>& EmptyMutableInstances() {
    static std::vector<EffectInstance> instances;
    instances.clear();
    return instances;
}

template <typename Queue>
void SortRenderQueue(Queue& queue) {
    std::sort(
        queue.begin(),
        queue.end(),
        [](const auto& left, const auto& right) {
            if (left.common.renderQueue != right.common.renderQueue) {
                return left.common.renderQueue < right.common.renderQueue;
            }
            const uint32_t leftId = left.common.instance != nullptr ? left.common.instance->id : 0;
            const uint32_t rightId = right.common.instance != nullptr ? right.common.instance->id : 0;
            return leftId < rightId;
        });
}
} // namespace

EffectRuntime::EffectRuntime(EffectSystem* effectSystem)
    : effectSystem_(effectSystem) {
}

void EffectRuntime::AttachSystem(EffectSystem* effectSystem) {
    effectSystem_ = effectSystem;
}

bool EffectRuntime::IsAttached() const {
    return effectSystem_ != nullptr;
}

uint32_t EffectRuntime::PlayEffect(std::string_view name, const Vector3& position) {
    if (effectSystem_ == nullptr) {
        return 0;
    }
    return effectSystem_->PlayEffect(name, position);
}

uint32_t EffectRuntime::PlayEffectWithParams(
    std::string_view name,
    const Vector3& position,
    const Vector4& color,
    const Vector3& scale) {
    if (effectSystem_ == nullptr) {
        return 0;
    }
    return effectSystem_->PlayEffectWithParams(name, position, color, scale);
}

void EffectRuntime::StopEffect(uint32_t id) {
    if (effectSystem_ != nullptr) {
        effectSystem_->StopEffect(id);
    }
}

void EffectRuntime::RestartInstance(uint32_t id) {
    if (effectSystem_ != nullptr) {
        effectSystem_->RestartInstance(id);
    }
}

void EffectRuntime::ClearInstances() {
    if (effectSystem_ != nullptr) {
        effectSystem_->ClearInstances();
    }
}

void EffectRuntime::Update(float deltaTime) {
    if (effectSystem_ == nullptr || paused_) {
        return;
    }
    effectSystem_->Update(deltaTime * speedMultiplier_);
}

EffectRuntimeFrame EffectRuntime::BuildFrame() const {
    if (effectSystem_ == nullptr) {
        return {};
    }

    EffectRuntimeFrame frame{};
    frame.activeInstanceCount = static_cast<uint32_t>(effectSystem_->Instances().size());
    frame.particleQueue = BuildParticleQueue();
    frame.trailQueue = BuildTrailQueue();
    frame.beamQueue = BuildBeamQueue();
    frame.distortionQueue = BuildDistortionQueue();
    frame.activeComponentCount =
        static_cast<uint32_t>(
            frame.particleQueue.size() +
            frame.trailQueue.size() +
            frame.beamQueue.size() +
            frame.distortionQueue.size());
    return frame;
}

ParticleRenderFallback EffectRuntime::FindPrimaryParticleFallback() const {
    if (effectSystem_ == nullptr) {
        return {};
    }

    const ParticleRenderQueue queue = BuildParticleQueue();
    if (!queue.empty()) {
        return {
            queue.front().common.componentCommon,
            queue.front().settings
        };
    }

    ParticleComponentAssetView selected{};
    for (const auto& [assetName, asset] : effectSystem_->Assets()) {
        (void)assetName;
        ::ForEachParticleComponent(asset.Components().ParticleStorageView(), [&selected](const ParticleComponentAssetView& particle) {
            if (!selected ||
                particle.common->passState.renderQueue < selected.common->passState.renderQueue ||
                (particle.common->passState.renderQueue == selected.common->passState.renderQueue &&
                    particle.common->name < selected.common->name)) {
                selected = particle;
            }
        });
    }

    if (!selected) {
        return {};
    }

    return {
        selected.common,
        selected.settings
    };
}

void EffectRuntime::ForEachActiveInstanceComponent(
    const std::function<void(const EffectInstance&, const EffectComponentInstance&)>& visitor) const {
    if (effectSystem_ == nullptr) {
        return;
    }

    for (const EffectInstance& instance : effectSystem_->Instances()) {
        if (instance.asset == nullptr) {
            continue;
        }

        for (const EffectComponentInstance& componentInstance : instance.components) {
            if (!componentInstance.active) {
                continue;
            }

            visitor(instance, componentInstance);
        }
    }
}

bool EffectRuntime::BuildActiveComponentCore(
    const EffectInstance& instance,
    const EffectComponentInstance& componentInstance,
    const EffectComponentCommon& componentCommon,
    ActiveComponentCore& outCore) {
    const float localAge = instance.age - componentCommon.startTime;
    if (localAge < 0.0f ||
        (componentCommon.duration > 0.0f && localAge >= componentCommon.duration)) {
        return false;
    }

    float normalizedAge = 0.0f;
    if (componentCommon.duration > 0.0f) {
        normalizedAge = localAge / componentCommon.duration;
        if (normalizedAge > 1.0f) {
            normalizedAge = 1.0f;
        }
    }

    outCore = {
        {
            instance.asset,
            &componentCommon,
            &instance,
            &componentInstance,
            componentCommon.passState.renderQueue,
            normalizedAge
        }
    };
    return true;
}

void EffectRuntime::ForEachParticleAssetComponent(
    const std::function<void(const ActiveComponentCore&, const ParticleComponentAssetView&)>& visitor) const {
    ForEachActiveInstanceComponent(
        [&visitor](const EffectInstance& instance, const EffectComponentInstance& componentInstance) {
            if (instance.asset == nullptr) {
                return;
            }
            if (const ParticleComponentAssetView particle =
                    FindParticleComponent(instance.asset->Components().ParticleStorageView(), componentInstance.componentId)) {
                ActiveComponentCore core{};
                if (BuildActiveComponentCore(instance, componentInstance, *particle.common, core)) {
                    visitor(core, particle);
                }
            }
        });
}

void EffectRuntime::ForEachTrailAssetComponent(
    const std::function<void(const ActiveComponentCore&, const TrailComponentAssetView&)>& visitor) const {
    ForEachActiveInstanceComponent(
        [&visitor](const EffectInstance& instance, const EffectComponentInstance& componentInstance) {
            if (instance.asset == nullptr) {
                return;
            }
            if (const TrailComponentAssetView trail =
                    FindTrailComponent(instance.asset->Components().TrailStorageView(), componentInstance.componentId)) {
                ActiveComponentCore core{};
                if (BuildActiveComponentCore(instance, componentInstance, *trail.common, core)) {
                    visitor(core, trail);
                }
            }
        });
}

void EffectRuntime::ForEachBeamAssetComponent(
    const std::function<void(const ActiveComponentCore&, const BeamComponentAssetView&)>& visitor) const {
    ForEachActiveInstanceComponent(
        [&visitor](const EffectInstance& instance, const EffectComponentInstance& componentInstance) {
            if (instance.asset == nullptr) {
                return;
            }
            if (const BeamComponentAssetView beam =
                    FindBeamComponent(instance.asset->Components().BeamStorageView(), componentInstance.componentId)) {
                ActiveComponentCore core{};
                if (BuildActiveComponentCore(instance, componentInstance, *beam.common, core)) {
                    visitor(core, beam);
                }
            }
        });
}

void EffectRuntime::ForEachDistortionAssetComponent(
    const std::function<void(const ActiveComponentCore&, const DistortionComponentAssetView&)>& visitor) const {
    ForEachActiveInstanceComponent(
        [&visitor](const EffectInstance& instance, const EffectComponentInstance& componentInstance) {
            if (instance.asset == nullptr) {
                return;
            }
            if (const DistortionComponentAssetView distortion =
                    FindDistortionComponent(instance.asset->Components().DistortionStorageView(), componentInstance.componentId)) {
                ActiveComponentCore core{};
                if (BuildActiveComponentCore(instance, componentInstance, *distortion.common, core)) {
                    visitor(core, distortion);
                }
            }
        });
}

void EffectRuntime::ForEachParticleComponent(
    const std::function<void(const ParticleActiveComponent&)>& visitor) const {
    ForEachParticleAssetComponent(
        [&visitor](const ActiveComponentCore& core, const ParticleComponentAssetView& particle) {
            visitor({core.common, particle.settings});
        });
}

void EffectRuntime::ForEachTrailComponent(
    const std::function<void(const TrailActiveComponent&)>& visitor) const {
    ForEachTrailAssetComponent(
        [&visitor](const ActiveComponentCore& core, const TrailComponentAssetView& trail) {
            visitor({core.common, trail.settings});
        });
}

void EffectRuntime::ForEachBeamComponent(
    const std::function<void(const BeamActiveComponent&)>& visitor) const {
    ForEachBeamAssetComponent(
        [&visitor](const ActiveComponentCore& core, const BeamComponentAssetView& beam) {
            visitor({core.common, beam.settings});
        });
}

void EffectRuntime::ForEachDistortionComponent(
    const std::function<void(const DistortionActiveComponent&)>& visitor) const {
    ForEachDistortionAssetComponent(
        [&visitor](const ActiveComponentCore& core, const DistortionComponentAssetView& distortion) {
            visitor({core.common, distortion.settings});
        });
}

ParticleRenderQueue EffectRuntime::BuildParticleQueue() const {
    ParticleRenderQueue queue;
    ForEachParticleComponent(
        [&queue](const ParticleActiveComponent& component) {
            queue.push_back({component.common, component.settings});
        });
    SortRenderQueue(queue);
    return queue;
}

TrailRenderQueue EffectRuntime::BuildTrailQueue() const {
    TrailRenderQueue queue;
    ForEachTrailComponent(
        [&queue](const TrailActiveComponent& component) {
            queue.push_back({component.common, component.settings});
        });
    SortRenderQueue(queue);
    return queue;
}

BeamRenderQueue EffectRuntime::BuildBeamQueue() const {
    BeamRenderQueue queue;
    ForEachBeamComponent(
        [&queue](const BeamActiveComponent& component) {
            queue.push_back({component.common, component.settings});
        });
    SortRenderQueue(queue);
    return queue;
}

DistortionRenderQueue EffectRuntime::BuildDistortionQueue() const {
    DistortionRenderQueue queue;
    ForEachDistortionComponent(
        [&queue](const DistortionActiveComponent& component) {
            queue.push_back({component.common, component.settings});
        });
    SortRenderQueue(queue);
    return queue;
}

void EffectRuntime::SetPaused(bool paused) {
    paused_ = paused;
}

bool EffectRuntime::IsPaused() const {
    return paused_;
}

void EffectRuntime::SetSpeedMultiplier(float speedMultiplier) {
    speedMultiplier_ = (std::max)(0.0f, speedMultiplier);
}

float EffectRuntime::SpeedMultiplier() const {
    return speedMultiplier_;
}

const std::unordered_map<std::string, EffectAsset>& EffectRuntime::Assets() const {
    if (effectSystem_ == nullptr) {
        return EmptyAssets();
    }
    return effectSystem_->Assets();
}

std::unordered_map<std::string, EffectAsset>& EffectRuntime::MutableAssets() {
    if (effectSystem_ == nullptr) {
        return EmptyMutableAssets();
    }
    return effectSystem_->MutableAssets();
}

const std::vector<EffectInstance>& EffectRuntime::Instances() const {
    if (effectSystem_ == nullptr) {
        return EmptyInstances();
    }
    return effectSystem_->Instances();
}

std::vector<EffectInstance>& EffectRuntime::MutableInstances() {
    if (effectSystem_ == nullptr) {
        return EmptyMutableInstances();
    }
    return effectSystem_->MutableInstances();
}
