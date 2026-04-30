#include "AppVfxRenderPipeline.h"

#include "AppFrameGraphBuilder.h"
#include "AppGpuParticleSystem.h"
#include "AppPipelines.h"
#include "AppRuntimeState.h"
#include "AppSceneResources.h"
#include "AppVfxRenderTargets.h"
#include "graphics/RenderGraph.h"
#include "vfx/AppVfxRendererSet.h"
#include "vfx/BeamRenderer.h"
#include "vfx/DistortionRenderer.h"
#include "vfx/ParticleRenderer.h"
#include "vfx/TrailRenderer.h"
#include "vfx/VfxRenderContext.h"
#include "vfx/VfxRenderInputs.h"
#include "vfx/VfxResources.h"

namespace {
const vfx::VfxTypedResourceSet& LegacySharedVfxResources() {
    static const vfx::VfxTypedResourceSet resources = vfx::MakeLegacySharedIndirectVfxResources();
    return resources;
}

VfxRenderContext BuildVfxRenderContext(const AppFrameGraphBuildContext& ctx) {
    return {
        ctx.appPipelines,
        ctx.renderResources,
        ctx.scene,
        ctx.gpuParticleSystem,
        ctx.vfxRenderers->beam,
        ctx.frameState,
        ctx.srvDescriptorHeap,
        ctx.vfxTextureHandle,
        ctx.depthTextureHandle,
        ctx.beamTime,
        &LegacySharedVfxResources()
    };
}
} // namespace

void AppVfxRenderPipeline::RegisterPasses(const AppFrameGraphBuildContext& ctx) const {
    const float transparentBlack[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx.renderGraph->DeclarePersistentRenderTarget(
        "VfxAccumulation",
        1.0f,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        transparentBlack,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    ctx.gpuParticleSystem->DeclareGraphBuffers(*ctx.renderGraph);

    const vfx::VfxTypedResourceSet& vfxResources = LegacySharedVfxResources();

    ctx.renderGraph->AddPass({
        vfxResources.particle.routing.simulationPass,
        ge3::graphics::RenderPassLayer::Vfx,
        vfx::SimulationAccesses(vfxResources.particle.simulation),
        "",
        [ctx](ge3::graphics::RenderPassContext& passContext) {
            ctx.vfxRenderers->particle->Simulate(
                passContext.commandList,
                BuildVfxRenderContext(ctx),
                ctx.effectRuntime->ParticleInput(ctx.primaryParticleFx));
        }});

    ctx.renderGraph->AddPass({
        "VFX.BeginAccumulation",
        ge3::graphics::RenderPassLayer::Vfx,
        {
            {"SceneDepth", ge3::graphics::RenderResourceAccessType::ReadDepth},
            {"VfxAccumulation", ge3::graphics::RenderResourceAccessType::WriteRtv},
        },
        "SceneDepth",
        [ctx](ge3::graphics::RenderPassContext& passContext) {
            ctx.vfxRenderTargets->BeginVfx(passContext.commandList, ctx.dsv);
        }});

    ctx.renderGraph->AddPass({
        vfxResources.particle.routing.drawPass,
        ge3::graphics::RenderPassLayer::Vfx,
        vfx::DrawAccesses(vfxResources.particle.renderer),
        vfxResources.particle.routing.depthTarget,
        [ctx](ge3::graphics::RenderPassContext& passContext) {
            const ParticleRenderQueue& queue = ctx.effectRuntime->particleQueue;
            if (!ctx.runtimeState->enableParticles && queue.empty()) {
                return;
            }
            ctx.vfxRenderers->particle->Draw(
                passContext.commandList,
                BuildVfxRenderContext(ctx),
                ctx.effectRuntime->ParticleInput(ctx.primaryParticleFx));
        }});

    ctx.renderGraph->AddPass({
        vfxResources.trail.routing.drawPass,
        ge3::graphics::RenderPassLayer::Vfx,
        vfx::DrawAccesses(vfxResources.trail.renderer),
        vfxResources.trail.routing.depthTarget,
        [ctx](ge3::graphics::RenderPassContext& passContext) {
            const TrailRenderQueue& queue = ctx.effectRuntime->trailQueue;
            if (queue.empty()) {
                return;
            }
            ctx.vfxRenderers->trail->Draw(
                passContext.commandList,
                BuildVfxRenderContext(ctx),
                ctx.effectRuntime->TrailInput());
        }});

    ctx.renderGraph->AddPass({
        vfxResources.beam.routing.drawPass,
        ge3::graphics::RenderPassLayer::Vfx,
        vfx::DrawAccesses(vfxResources.beam.renderer),
        vfxResources.beam.routing.depthTarget,
        [ctx](ge3::graphics::RenderPassContext& passContext) {
            const BeamRenderQueue& queue = ctx.effectRuntime->beamQueue;
            if (queue.empty()) {
                return;
            }

            ctx.vfxRenderers->beam->Draw(
                passContext.commandList,
                ctx.effectRuntime->BeamInput(),
                BuildVfxRenderContext(ctx));
        }});

    ctx.renderGraph->AddPass({
        vfxResources.distortion.routing.drawPass,
        ge3::graphics::RenderPassLayer::Vfx,
        vfx::DrawAccesses(vfxResources.distortion.renderer),
        vfxResources.distortion.routing.depthTarget,
        [ctx](ge3::graphics::RenderPassContext& passContext) {
            const DistortionRenderQueue& queue = ctx.effectRuntime->distortionQueue;
            if (queue.empty()) {
                return;
            }
            ctx.vfxRenderers->distortion->Draw(
                passContext.commandList,
                BuildVfxRenderContext(ctx),
                ctx.effectRuntime->DistortionInput());
        }});
}
