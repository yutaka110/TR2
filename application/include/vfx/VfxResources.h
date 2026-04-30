#pragma once

#include <vector>

#include "graphics/RenderGraph.h"

namespace vfx {
struct VfxSimulationResourceSet {
    const char* stateBuffer = "";
    const char* renderBuffer = "";
    const char* indirectArgs = "";
    bool usesCompute = false;
};

struct VfxRendererResourceSet {
    const char* sceneColor = "SceneColor";
    const char* sceneDepth = "SceneDepth";
    const char* renderBuffer = "";
    const char* indirectArgs = "";
    const char* accumulationTarget = "VfxAccumulation";
    bool usesIndirectSprite = false;
};

struct VfxPassRoutingResourceSet {
    const char* simulationPass = "";
    const char* drawPass = "";
    const char* depthTarget = "SceneDepthReadOnly";
};

struct ParticleVfxResourceSet {
    VfxSimulationResourceSet simulation{};
    VfxRendererResourceSet renderer{};
    VfxPassRoutingResourceSet routing{};
};

struct TrailVfxResourceSet {
    VfxSimulationResourceSet simulation{};
    VfxRendererResourceSet renderer{};
    VfxPassRoutingResourceSet routing{};
};

struct BeamVfxResourceSet {
    VfxSimulationResourceSet simulation{};
    VfxRendererResourceSet renderer{};
    VfxPassRoutingResourceSet routing{};
};

struct DistortionVfxResourceSet {
    VfxSimulationResourceSet simulation{};
    VfxRendererResourceSet renderer{};
    VfxPassRoutingResourceSet routing{};
};

struct VfxTypedResourceSet {
    ParticleVfxResourceSet particle{};
    TrailVfxResourceSet trail{};
    BeamVfxResourceSet beam{};
    DistortionVfxResourceSet distortion{};
};

inline VfxTypedResourceSet MakeLegacySharedIndirectVfxResources() {
    VfxTypedResourceSet resources{};

    resources.particle.simulation = {
        "ParticleState",
        "ParticleRenderBuffer",
        "ParticleIndirectArgs",
        true
    };
    resources.particle.renderer = {
        "SceneColor",
        "SceneDepth",
        "ParticleRenderBuffer",
        "ParticleIndirectArgs",
        "VfxAccumulation",
        true
    };
    resources.particle.routing = {
        "VFX.ParticleSimulation",
        "VFX.Particles",
        "SceneDepthReadOnly"
    };

    resources.trail.renderer = resources.particle.renderer;
    resources.trail.routing = {
        "",
        "VFX.Trails",
        "SceneDepthReadOnly"
    };

    resources.beam.renderer = {
        "SceneColor",
        "SceneDepth",
        "",
        "",
        "VfxAccumulation",
        false
    };
    resources.beam.routing = {
        "",
        "VFX.Beam",
        "SceneDepthReadOnly"
    };

    resources.distortion.renderer = {
        "SceneColor",
        "SceneDepth",
        "DistortionRenderBuffer",
        "DistortionIndirectArgs",
        "VfxAccumulation",
        true
    };
    resources.distortion.routing = {
        "",
        "VFX.Distortion",
        "SceneDepthReadOnly"
    };

    return resources;
}

inline std::vector<ge3::graphics::RenderPassResourceAccess> SimulationAccesses(
    const VfxSimulationResourceSet& resources) {
    using ge3::graphics::RenderPassResourceAccess;
    using ge3::graphics::RenderResourceAccessType;

    std::vector<RenderPassResourceAccess> accesses;
    if (!resources.usesCompute) {
        return accesses;
    }

    accesses.push_back({resources.stateBuffer, RenderResourceAccessType::ReadUav});
    accesses.push_back({resources.stateBuffer, RenderResourceAccessType::WriteUav});
    accesses.push_back({resources.renderBuffer, RenderResourceAccessType::WriteUav});
    return accesses;
}

inline std::vector<ge3::graphics::RenderPassResourceAccess> DrawAccesses(
    const VfxRendererResourceSet& resources) {
    using ge3::graphics::RenderPassResourceAccess;
    using ge3::graphics::RenderResourceAccessType;

    std::vector<RenderPassResourceAccess> accesses = {
        {resources.sceneColor, RenderResourceAccessType::ReadSrv},
        {resources.sceneDepth, RenderResourceAccessType::ReadDepth},
    };

    if (resources.usesIndirectSprite) {
        accesses.push_back({resources.renderBuffer, RenderResourceAccessType::ReadSrv});
        accesses.push_back({resources.indirectArgs, RenderResourceAccessType::ReadIndirect});
    }

    accesses.push_back({resources.accumulationTarget, RenderResourceAccessType::WriteRtv});
    return accesses;
}
} // namespace vfx
