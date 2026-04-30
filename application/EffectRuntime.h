#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "EffectSystem.h"
#include "utils/math/MathUtils.h"

class EffectRuntime {
public:
    explicit EffectRuntime(EffectSystem* effectSystem = nullptr);

    void AttachSystem(EffectSystem* effectSystem);
    bool IsAttached() const;

    uint32_t PlayEffect(std::string_view name, const Vector3& position);
    uint32_t PlayEffectWithParams(
        std::string_view name,
        const Vector3& position,
        const Vector4& color,
        const Vector3& scale);
    void StopEffect(uint32_t id);
    void RestartInstance(uint32_t id);
    void ClearInstances();

    void Update(float deltaTime);
    EffectRuntimeFrame BuildFrame() const;
    ParticleRenderFallback FindPrimaryParticleFallback() const;

    void SetPaused(bool paused);
    bool IsPaused() const;
    void SetSpeedMultiplier(float speedMultiplier);
    float SpeedMultiplier() const;

    const std::unordered_map<std::string, EffectAsset>& Assets() const;
    std::unordered_map<std::string, EffectAsset>& MutableAssets();
    const std::vector<EffectInstance>& Instances() const;
    std::vector<EffectInstance>& MutableInstances();

private:
    struct ActiveComponentCore {
        EffectRenderItemCommon common{};
    };

    struct ParticleActiveComponent {
        EffectRenderItemCommon common{};
        const EffectParticleSettings* settings = nullptr;
    };

    struct TrailActiveComponent {
        EffectRenderItemCommon common{};
        const EffectTrailSettings* settings = nullptr;
    };

    struct BeamActiveComponent {
        EffectRenderItemCommon common{};
        const EffectBeamSettings* settings = nullptr;
    };

    struct DistortionActiveComponent {
        EffectRenderItemCommon common{};
        const EffectDistortionSettings* settings = nullptr;
    };

    void ForEachActiveInstanceComponent(
        const std::function<void(const EffectInstance&, const EffectComponentInstance&)>& visitor) const;
    static bool BuildActiveComponentCore(
        const EffectInstance& instance,
        const EffectComponentInstance& componentInstance,
        const EffectComponentCommon& componentCommon,
        ActiveComponentCore& outCore);
    void ForEachParticleAssetComponent(
        const std::function<void(const ActiveComponentCore&, const ParticleComponentAssetView&)>& visitor) const;
    void ForEachTrailAssetComponent(
        const std::function<void(const ActiveComponentCore&, const TrailComponentAssetView&)>& visitor) const;
    void ForEachBeamAssetComponent(
        const std::function<void(const ActiveComponentCore&, const BeamComponentAssetView&)>& visitor) const;
    void ForEachDistortionAssetComponent(
        const std::function<void(const ActiveComponentCore&, const DistortionComponentAssetView&)>& visitor) const;
    void ForEachParticleComponent(const std::function<void(const ParticleActiveComponent&)>& visitor) const;
    void ForEachTrailComponent(const std::function<void(const TrailActiveComponent&)>& visitor) const;
    void ForEachBeamComponent(const std::function<void(const BeamActiveComponent&)>& visitor) const;
    void ForEachDistortionComponent(const std::function<void(const DistortionActiveComponent&)>& visitor) const;
    ParticleRenderQueue BuildParticleQueue() const;
    TrailRenderQueue BuildTrailQueue() const;
    BeamRenderQueue BuildBeamQueue() const;
    DistortionRenderQueue BuildDistortionQueue() const;

    EffectSystem* effectSystem_ = nullptr;
    bool paused_ = false;
    float speedMultiplier_ = 1.0f;
};
