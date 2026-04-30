#pragma once

#include "../../EffectSystem.h"

struct VfxComponentInputCommon {
    const EffectRenderItemCommon* primaryItem = nullptr;
    const EffectAsset* asset = nullptr;
    const EffectComponentCommon* componentCommon = nullptr;
    const EffectInstance* instance = nullptr;
    const EffectComponentInstance* componentInstance = nullptr;
    float normalizedAge = 0.0f;
    uint32_t renderQueue = 0;
};

struct ParticleRenderInput {
    VfxComponentInputCommon primary{};
    const EffectComponentCommon* fallbackCommon = nullptr;
    const EffectParticleSettings* settings = nullptr;
    const EffectParticleSettings* fallbackSettings = nullptr;
};

struct TrailRenderInput {
    VfxComponentInputCommon primary{};
    const EffectTrailSettings* settings = nullptr;
};

struct BeamRenderInput {
    VfxComponentInputCommon primary{};
    const EffectBeamSettings* settings = nullptr;
};

struct DistortionRenderInput {
    VfxComponentInputCommon primary{};
    const EffectDistortionSettings* settings = nullptr;
};
