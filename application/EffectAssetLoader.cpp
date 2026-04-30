#include "EffectAssetLoader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <variant>

namespace {
using ComponentEditBuffer = std::variant<
    ParticleComponentAsset,
    TrailComponentAsset,
    BeamComponentAsset,
    DistortionComponentAsset>;

std::string Trim(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

float ToFloat(const std::string& value, float fallback) {
    try {
        return std::stof(value);
    } catch (...) {
        return fallback;
    }
}

bool SplitTypedKey(const std::string& key, std::string& scope, std::string& field) {
    const size_t dot = key.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= key.size()) {
        return false;
    }

    scope = key.substr(0, dot);
    field = key.substr(dot + 1);
    return true;
}

bool ApplyParticleSettingsKey(
    EffectParticleSettings& settings,
    const std::string& key,
    const std::string& value) {
    if (key == "emissive") {
        settings.emissive = ToFloat(value, settings.emissive);
    } else if (key == "distortion" || key == "distortionStrength") {
        settings.distortionStrength = ToFloat(value, settings.distortionStrength);
    } else if (key == "noise" || key == "noiseStrength") {
        settings.noiseStrength = ToFloat(value, settings.noiseStrength);
    } else if (key == "uvScroll" || key == "uvScrollSpeed") {
        settings.uvScrollSpeed = ToFloat(value, settings.uvScrollSpeed);
    } else if (key == "pulseSpeed") {
        settings.pulseSpeed = ToFloat(value, settings.pulseSpeed);
    } else if (key == "spawnRadius") {
        settings.spawnRadius = ToFloat(value, settings.spawnRadius);
    } else if (key == "depthFadeSoftness" || key == "softness") {
        settings.depthFadeSoftness = ToFloat(value, settings.depthFadeSoftness);
    } else if (key == "particleEdgeSoftness" || key == "edgeSoftness") {
        settings.edgeSoftness = ToFloat(value, settings.edgeSoftness);
    } else {
        return false;
    }
    return true;
}

bool ApplyTrailSettingsKey(
    EffectTrailSettings& settings,
    const std::string& key,
    const std::string& value) {
    if (key == "depthFadeSoftness" || key == "softness") {
        settings.depthFadeSoftness = ToFloat(value, settings.depthFadeSoftness);
    } else if (key == "trailTailFade" || key == "tailFade") {
        settings.trailTailFade = ToFloat(value, settings.trailTailFade);
    } else {
        return false;
    }
    return true;
}

bool ApplyBeamSettingsKey(
    EffectBeamSettings& settings,
    const std::string& key,
    const std::string& value) {
    if (key == "emissive") {
        settings.emissive = ToFloat(value, settings.emissive);
    } else {
        return false;
    }
    return true;
}

bool ApplyDistortionSettingsKey(
    EffectDistortionSettings& settings,
    const std::string& key,
    const std::string& value) {
    if (key == "distortion" || key == "strength") {
        settings.strength = ToFloat(value, settings.strength);
    } else if (key == "noise" || key == "noiseStrength") {
        settings.noiseStrength = ToFloat(value, settings.noiseStrength);
    } else if (key == "uvScroll" || key == "uvScrollSpeed") {
        settings.uvScrollSpeed = ToFloat(value, settings.uvScrollSpeed);
    } else if (key == "depthFadeSoftness" || key == "softness") {
        settings.depthFadeSoftness = ToFloat(value, settings.depthFadeSoftness);
    } else if (key == "distortionDepthAttenuation" || key == "distortionAttenuation" || key == "depthAttenuation") {
        settings.depthAttenuation = ToFloat(value, settings.depthAttenuation);
    } else {
        return false;
    }
    return true;
}

bool ApplyTypedDefaultKey(
    EffectAsset& asset,
    const std::string& scope,
    const std::string& field,
    const std::string& value) {
    if (scope == "particle") {
        return ApplyParticleSettingsKey(asset.defaultParticle, field, value);
    }
    if (scope == "trail") {
        return ApplyTrailSettingsKey(asset.defaultTrail, field, value);
    }
    if (scope == "beam") {
        return ApplyBeamSettingsKey(asset.defaultBeam, field, value);
    }
    if (scope == "distortion") {
        return ApplyDistortionSettingsKey(asset.defaultDistortion, field, value);
    }
    return false;
}

bool ApplyTypedComponentKey(
    ComponentEditBuffer& component,
    const std::string& scope,
    const std::string& field,
    const std::string& value) {
    if (scope == "particle") {
        if (auto* particle = std::get_if<ParticleComponentAsset>(&component)) {
            return ApplyParticleSettingsKey(particle->settings, field, value);
        }
        return false;
    }
    if (scope == "trail") {
        if (auto* trail = std::get_if<TrailComponentAsset>(&component)) {
            return ApplyTrailSettingsKey(trail->settings, field, value);
        }
        return false;
    }
    if (scope == "beam") {
        if (auto* beam = std::get_if<BeamComponentAsset>(&component)) {
            return ApplyBeamSettingsKey(beam->settings, field, value);
        }
        return false;
    }
    if (scope == "distortion") {
        if (auto* distortion = std::get_if<DistortionComponentAsset>(&component)) {
            return ApplyDistortionSettingsKey(distortion->settings, field, value);
        }
        return false;
    }
    return false;
}

bool ApplyLegacyAssetSettingsKey(
    EffectAsset& asset,
    const std::string& key,
    const std::string& value) {
    if (key == "emissive") {
        asset.defaultParticle.emissive = ToFloat(value, asset.defaultParticle.emissive);
        asset.defaultBeam.emissive = asset.defaultParticle.emissive;
    } else if (key == "distortion") {
        asset.defaultParticle.distortionStrength = ToFloat(value, asset.defaultParticle.distortionStrength);
        asset.defaultDistortion.strength = asset.defaultParticle.distortionStrength;
    } else if (key == "noise") {
        asset.defaultParticle.noiseStrength = ToFloat(value, asset.defaultParticle.noiseStrength);
        asset.defaultDistortion.noiseStrength = asset.defaultParticle.noiseStrength;
    } else if (key == "uvScroll") {
        asset.defaultParticle.uvScrollSpeed = ToFloat(value, asset.defaultParticle.uvScrollSpeed);
        asset.defaultDistortion.uvScrollSpeed = asset.defaultParticle.uvScrollSpeed;
    } else if (key == "pulseSpeed") {
        asset.defaultParticle.pulseSpeed = ToFloat(value, asset.defaultParticle.pulseSpeed);
    } else if (key == "spawnRadius") {
        asset.defaultParticle.spawnRadius = ToFloat(value, asset.defaultParticle.spawnRadius);
    } else if (key == "depthFadeSoftness" || key == "softness") {
        asset.defaultParticle.depthFadeSoftness = ToFloat(value, asset.defaultParticle.depthFadeSoftness);
        asset.defaultTrail.depthFadeSoftness = asset.defaultParticle.depthFadeSoftness;
        asset.defaultDistortion.depthFadeSoftness = asset.defaultParticle.depthFadeSoftness;
    } else if (key == "distortionDepthAttenuation" || key == "distortionAttenuation") {
        asset.defaultDistortion.depthAttenuation = ToFloat(value, asset.defaultDistortion.depthAttenuation);
    } else if (key == "particleEdgeSoftness" || key == "edgeSoftness") {
        asset.defaultParticle.edgeSoftness = ToFloat(value, asset.defaultParticle.edgeSoftness);
    } else if (key == "trailTailFade" || key == "tailFade") {
        asset.defaultTrail.trailTailFade = ToFloat(value, asset.defaultTrail.trailTailFade);
    } else {
        return false;
    }
    return true;
}

bool ApplyLegacyTypedSettingsKey(
    ParticleComponentAsset& component,
    const std::string& key,
    const std::string& value) {
    return ApplyParticleSettingsKey(component.settings, key, value);
}

bool ApplyLegacyTypedSettingsKey(
    TrailComponentAsset& component,
    const std::string& key,
    const std::string& value) {
    return ApplyTrailSettingsKey(component.settings, key, value);
}

bool ApplyLegacyTypedSettingsKey(
    BeamComponentAsset& component,
    const std::string& key,
    const std::string& value) {
    return ApplyBeamSettingsKey(component.settings, key, value);
}

bool ApplyLegacyTypedSettingsKey(
    DistortionComponentAsset& component,
    const std::string& key,
    const std::string& value) {
    return ApplyDistortionSettingsKey(component.settings, key, value);
}

bool ApplyLegacyComponentSettingsKey(
    ComponentEditBuffer& component,
    const std::string& key,
    const std::string& value) {
    return std::visit(
        [&key, &value](auto& typedComponent) {
            return ApplyLegacyTypedSettingsKey(typedComponent, key, value);
        },
        component);
}

ge3::graphics::BlendMode ParseBlend(const std::string& value) {
    if (value == "alpha") {
        return ge3::graphics::BlendMode::Alpha;
    }
    if (value == "additive") {
        return ge3::graphics::BlendMode::Additive;
    }
    if (value == "distortion") {
        return ge3::graphics::BlendMode::Distortion;
    }
    return ge3::graphics::BlendMode::Opaque;
}

EffectLayer ParseLayer(const std::string& value) {
    if (value == "opaque") {
        return EffectLayer::OpaqueFx;
    }
    if (value == "distortion") {
        return EffectLayer::DistortionFx;
    }
    if (value == "trail") {
        return EffectLayer::TrailFx;
    }
    if (value == "volumetric") {
        return EffectLayer::VolumetricFx;
    }
    return EffectLayer::AdditiveFx;
}

EffectComponentType ParseComponentType(const std::string& value) {
    if (value == "trail") {
        return EffectComponentType::Trail;
    }
    if (value == "beam") {
        return EffectComponentType::Beam;
    }
    if (value == "distortion") {
        return EffectComponentType::Distortion;
    }
    return EffectComponentType::Particle;
}

EffectTechnique ParseTechnique(const std::string& value) {
    if (value == "TrailRibbon") {
        return EffectTechnique::TrailRibbon;
    }
    if (value == "BeamLightning") {
        return EffectTechnique::BeamLightning;
    }
    if (value == "DistortionSprite") {
        return EffectTechnique::DistortionSprite;
    }
    return EffectTechnique::ParticleAdditive;
}

EffectComponentType TypeFromTechnique(EffectTechnique technique) {
    switch (technique) {
    case EffectTechnique::TrailRibbon:
        return EffectComponentType::Trail;
    case EffectTechnique::BeamLightning:
        return EffectComponentType::Beam;
    case EffectTechnique::DistortionSprite:
        return EffectComponentType::Distortion;
    case EffectTechnique::ParticleAdditive:
    default:
        return EffectComponentType::Particle;
    }
}

void ApplyRoutingFromTechnique(EffectComponentCommon& common) {
    common.type = TypeFromTechnique(common.technique);
    if (common.technique == EffectTechnique::TrailRibbon) {
        common.rendererType = EffectRendererType::TrailRenderer;
        common.simulationType = EffectSimulationType::CpuTimeline;
        common.layer = EffectLayer::TrailFx;
    } else if (common.technique == EffectTechnique::BeamLightning) {
        common.rendererType = EffectRendererType::BeamRenderer;
        common.simulationType = EffectSimulationType::CpuTimeline;
        common.layer = EffectLayer::AdditiveFx;
    } else if (common.technique == EffectTechnique::DistortionSprite) {
        common.rendererType = EffectRendererType::DistortionRenderer;
        common.simulationType = EffectSimulationType::None;
        common.layer = EffectLayer::DistortionFx;
        common.passState.blend = ge3::graphics::BlendMode::Distortion;
    } else {
        common.rendererType = EffectRendererType::ParticleRenderer;
        common.simulationType = EffectSimulationType::CpuSpawnGpuSim;
        common.layer = EffectLayer::AdditiveFx;
    }
}

EffectComponentCommon MakeComponentCommonFromAsset(
    const EffectAsset& asset,
    EffectComponentType type,
    uint32_t id) {
    EffectComponentCommon common = EffectComponentAssetBuilder::MakeCommon(asset, type, id);
    ApplyRoutingFromTechnique(common);
    if (type == EffectComponentType::Trail) common.passState.renderQueue += 100;
    if (type == EffectComponentType::Beam) common.passState.renderQueue += 200;
    if (type == EffectComponentType::Distortion) common.passState.renderQueue += 300;
    return common;
}

ComponentEditBuffer MakeComponentBufferFromAsset(
    const EffectAsset& asset,
    EffectComponentType type,
    uint32_t id) {
    EffectComponentCommon common = MakeComponentCommonFromAsset(asset, type, id);
    switch (type) {
    case EffectComponentType::Trail:
        return EffectComponentAssetBuilder::MakeTrail(asset, common);
    case EffectComponentType::Beam:
        return EffectComponentAssetBuilder::MakeBeam(asset, common);
    case EffectComponentType::Distortion:
        return EffectComponentAssetBuilder::MakeDistortion(asset, common);
    case EffectComponentType::Particle:
    default:
        return EffectComponentAssetBuilder::MakeParticle(asset, common);
    }
}

EffectComponentCommon& ComponentCommon(ComponentEditBuffer& component) {
    return std::visit(
        [](auto& typedComponent) -> EffectComponentCommon& {
            return typedComponent.common;
        },
        component);
}

void ResetComponentPayloadForCommon(const EffectAsset& asset, ComponentEditBuffer& component) {
    EffectComponentCommon common = ComponentCommon(component);
    switch (common.type) {
    case EffectComponentType::Trail:
        component = EffectComponentAssetBuilder::MakeTrail(asset, common);
        break;
    case EffectComponentType::Beam:
        component = EffectComponentAssetBuilder::MakeBeam(asset, common);
        break;
    case EffectComponentType::Distortion:
        component = EffectComponentAssetBuilder::MakeDistortion(asset, common);
        break;
    case EffectComponentType::Particle:
    default:
        component = EffectComponentAssetBuilder::MakeParticle(asset, common);
        break;
    }
}

void AddComponentBuffer(EffectAsset& asset, const ComponentEditBuffer& component) {
    std::visit(
        [&asset](const auto& typedComponent) {
            asset.MutableComponents().Add(typedComponent);
        },
        component);
}

bool ApplyComponentKeyValue(
    const EffectAsset& asset,
    ComponentEditBuffer& component,
    const std::string& key,
    const std::string& value) {
    // Authoring order: common component keys, typed settings, then legacy flat keys.
    std::string scope;
    std::string field;
    if (SplitTypedKey(key, scope, field) &&
        ApplyTypedComponentKey(component, scope, field, value)) {
        return true;
    }

    EffectComponentCommon& common = ComponentCommon(component);
    if (key == "name") {
        common.name = value;
    } else if (key == "technique") {
        common.technique = ParseTechnique(value);
        ApplyRoutingFromTechnique(common);
        ResetComponentPayloadForCommon(asset, component);
    } else if (key == "shader") {
        common.shader = value;
    } else if (key == "texture") {
        common.texture = value;
    } else if (key == "blend") {
        common.passState.blend = ParseBlend(value);
    } else if (key == "layer") {
        common.layer = ParseLayer(value);
    } else if (key == "start" || key == "startTime") {
        common.startTime = ToFloat(value, common.startTime);
    } else if (key == "duration" || key == "lifetime") {
        common.duration = ToFloat(value, common.duration);
    } else if (ApplyLegacyComponentSettingsKey(component, key, value)) {
    } else if (key == "renderQueue") {
        common.passState.renderQueue = static_cast<uint32_t>(
            ToFloat(value, static_cast<float>(common.passState.renderQueue)));
    } else if (key == "size") {
        std::stringstream stream(value);
        char comma = ',';
        float x = common.size.x;
        float y = common.size.y;
        float z = common.size.z;
        if (stream >> x) {
            if (stream >> comma >> y >> comma >> z) {
                common.size = {x, y, z};
            } else {
                common.size = {x, x, x};
            }
        }
    } else if (key == "color") {
        std::stringstream stream(value);
        char comma = ',';
        stream >> common.color.x >> comma >> common.color.y >> comma >> common.color.z;
        if (stream >> comma >> common.color.w) {
        }
    } else {
        return false;
    }
    return true;
}
} // namespace

std::vector<LoadedEffectAsset> EffectAssetLoader::LoadDirectory(
    const std::filesystem::path& directory) const {
    std::vector<LoadedEffectAsset> assets;
    if (!std::filesystem::exists(directory)) {
        return assets;
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".effect") {
            continue;
        }

        LoadedEffectAsset loaded{};
        if (LoadFile(entry.path(), loaded)) {
            assets.push_back(std::move(loaded));
        }
    }
    return assets;
}

bool EffectAssetLoader::LoadFile(const std::filesystem::path& path, LoadedEffectAsset& outAsset) const {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    EffectAsset asset{};
    asset.name = path.stem().string();
    asset.passState.blend = ge3::graphics::BlendMode::Additive;
    asset.passState.depth = ge3::graphics::DepthMode::ReadOnly;
    ComponentEditBuffer currentComponent{};
    bool hasCurrentComponent = false;
    uint32_t nextComponentId = 1;

    auto flushCurrentComponent = [&]() {
        if (!hasCurrentComponent) {
            return;
        }
        AddComponentBuffer(asset, currentComponent);
        hasCurrentComponent = false;
    };

    std::string line;
    while (std::getline(file, line)) {
        const size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }

        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = Trim(line.substr(0, separator));
        const std::string value = Trim(line.substr(separator + 1));
        if (key == "component") {
            flushCurrentComponent();
            const EffectComponentType type = ParseComponentType(value);
            currentComponent = MakeComponentBufferFromAsset(asset, type, nextComponentId++);
            hasCurrentComponent = true;
            continue;
        }

        if (key == "technique" && !hasCurrentComponent) {
            const EffectTechnique technique = ParseTechnique(value);
            currentComponent = MakeComponentBufferFromAsset(asset, TypeFromTechnique(technique), nextComponentId++);
            hasCurrentComponent = true;
            EffectComponentCommon& common = ComponentCommon(currentComponent);
            common.technique = technique;
            ApplyRoutingFromTechnique(common);
            ResetComponentPayloadForCommon(asset, currentComponent);
            continue;
        }

        if (hasCurrentComponent) {
            ApplyComponentKeyValue(asset, currentComponent, key, value);
        } else {
            ApplyKeyValue(asset, key, value);
        }
    }

    flushCurrentComponent();
    asset.MutableComponents().SyncTypedStorageFromPackedForNormalization();
    outAsset.asset = std::move(asset);
    outAsset.path = path;
    outAsset.lastWriteTime = std::filesystem::last_write_time(path);
    return true;
}

bool EffectAssetLoader::ApplyKeyValue(
    EffectAsset& asset,
    const std::string& key,
    const std::string& value) {
    // Authoring order: common asset keys, typed defaults, then legacy flat keys.
    std::string scope;
    std::string field;
    if (SplitTypedKey(key, scope, field) &&
        ApplyTypedDefaultKey(asset, scope, field, value)) {
        return true;
    }

    if (key == "name") {
        asset.name = value;
    } else if (key == "shader") {
        asset.shader = value;
    } else if (key == "texture") {
        asset.texture = value;
    } else if (key == "blend") {
        asset.passState.blend = ParseBlend(value);
    } else if (key == "layer") {
        asset.layer = ParseLayer(value);
    } else if (key == "lifetime") {
        asset.lifetime = ToFloat(value, asset.lifetime);
    } else if (ApplyLegacyAssetSettingsKey(asset, key, value)) {
    } else if (key == "renderQueue") {
        asset.passState.renderQueue = static_cast<uint32_t>(ToFloat(value, static_cast<float>(asset.passState.renderQueue)));
    } else if (key == "size") {
        std::stringstream stream(value);
        char comma = ',';
        float x = asset.size.x;
        float y = asset.size.y;
        float z = asset.size.z;
        if (stream >> x) {
            if (stream >> comma >> y >> comma >> z) {
                asset.size = {x, y, z};
            } else {
                asset.size = {x, x, x};
            }
        }
    } else if (key == "color") {
        std::stringstream stream(value);
        char comma = ',';
        stream >> asset.color.x >> comma >> asset.color.y >> comma >> asset.color.z;
        if (stream >> comma >> asset.color.w) {
        }
    } else {
        return false;
    }
    return true;
}

