#include "AppFrameGraphBuilder.h"

#include "AppImGuiLayer.h"
#include "AppPipelines.h"
#include "AppPostProcessPipeline.h"
#include "AppRuntimeState.h"
#include "AppSceneRenderPipeline.h"
#include "AppVfxRenderPipeline.h"
#include "AppVfxRenderTargets.h"
#include "PostProcessStack.h"
#include "graphics/RenderGraph.h"
#include "vfx/AppVfxRendererSet.h"

namespace {
bool IsValidContext(const AppFrameGraphBuildContext& context) {
    return context.renderGraph != nullptr &&
        context.runtimeState != nullptr &&
        context.frameRenderer != nullptr &&
        context.imguiLayer != nullptr &&
        context.appPipelines != nullptr &&
        context.renderResources != nullptr &&
        context.scene != nullptr &&
        context.vfxRenderTargets != nullptr &&
        context.gpuParticleSystem != nullptr &&
        context.vfxRenderers != nullptr &&
        context.vfxRenderers->IsValid() &&
        context.postProcessStack != nullptr &&
        context.frameState != nullptr &&
        context.effectRuntime != nullptr;
}
} // namespace

void AppFrameGraphBuilder::Build(const AppFrameGraphBuildContext& context) const {
    if (!IsValidContext(context)) {
        return;
    }

    const AppFrameGraphBuildContext ctx = context;
    const bool enableVfx =
        ctx.runtimeState != nullptr &&
        ctx.runtimeState->enableVfxRenderPasses;
    const bool enablePostProcess =
        ctx.runtimeState != nullptr &&
        ctx.runtimeState->enablePostProcessPasses;
    const bool enableDebugPreview =
        ctx.runtimeState != nullptr &&
        ctx.runtimeState->enableDebugPreviewPasses;

    const PostProcessExecutionPlan postExecutionPlan =
        enablePostProcess
        ? ctx.postProcessStack->BuildExecutionPlan()
        : PostProcessExecutionPlan{};
    const std::string finalPostOutput =
        enablePostProcess && !postExecutionPlan.finalOutputResource.empty()
        ? postExecutionPlan.finalOutputResource
        : "SceneColor";
    AppSceneRenderPipeline{}.RegisterPasses(ctx);
    if (enableVfx) {
        AppVfxRenderPipeline{}.RegisterPasses(ctx);
    }
    AppPostProcessPipeline{}.RegisterPasses(ctx);

    const float debugClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (enableDebugPreview) {
        ctx.renderGraph->DeclarePersistentRenderTarget(
            "DebugDepthPreview",
            0.5f,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            debugClear,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        ctx.renderGraph->DeclarePersistentRenderTarget(
            "DebugEmissivePreview",
            0.5f,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            debugClear,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    ge3::graphics::RenderPassDesc depthPreviewPass{};
    depthPreviewPass.name = "Debug.DepthPreview";
    depthPreviewPass.layer = ge3::graphics::RenderPassLayer::Debug;
    depthPreviewPass.accesses = {
        {"SceneDepth", ge3::graphics::RenderResourceAccessType::ReadDepth},
        {"DebugDepthPreview", ge3::graphics::RenderResourceAccessType::WriteRtv},
    };
    depthPreviewPass.execute =
        [ctx](ge3::graphics::RenderPassContext& passContext) {
            ID3D12DescriptorHeap* descriptorHeaps[] = { ctx.srvDescriptorHeap };
            passContext.commandList->SetDescriptorHeaps(1, descriptorHeaps);
            float debugParams[8] = {
                ctx.runtimeState->debugDepthPreviewNear,
                ctx.runtimeState->debugDepthPreviewFar,
                ctx.runtimeState->debugDepthPreviewPower,
                1.0f,
                0.0f,
                0.0f,
                0.0f,
                0.0f
            };
            ctx.vfxRenderTargets->ExecuteDebugPreviewPass(
                passContext.commandList,
                "DebugDepthPreview",
                ctx.appPipelines->GetCompositeRootSignature(),
                ctx.appPipelines->GetDebugDepthPreviewPSO(),
                ctx.depthTextureHandle,
                ctx.vfxRenderTargets->GetSrvHandle("SceneColor"),
                ctx.vfxRenderTargets->GetSrvHandle("VfxAccumulation"),
                debugParams);
        };
    depthPreviewPass.forceExecute = true;
    if (enableDebugPreview) {
        ctx.renderGraph->AddPass(std::move(depthPreviewPass));
    }

    ge3::graphics::RenderPassDesc emissivePreviewPass{};
    emissivePreviewPass.name = "Debug.EmissiveIsolation";
    emissivePreviewPass.layer = ge3::graphics::RenderPassLayer::Debug;
    emissivePreviewPass.accesses = {
        {"SceneColor", ge3::graphics::RenderResourceAccessType::ReadSrv},
        {"VfxAccumulation", ge3::graphics::RenderResourceAccessType::ReadSrv},
        {finalPostOutput, ge3::graphics::RenderResourceAccessType::ReadSrv},
        {"DebugEmissivePreview", ge3::graphics::RenderResourceAccessType::WriteRtv},
    };
    emissivePreviewPass.execute =
        [ctx, finalPostOutput](ge3::graphics::RenderPassContext& passContext) {
            ID3D12DescriptorHeap* descriptorHeaps[] = { ctx.srvDescriptorHeap };
            passContext.commandList->SetDescriptorHeaps(1, descriptorHeaps);
            float debugParams[8] = {
                ctx.runtimeState->debugEmissivePreviewBoost,
                1.0f,
                1.0f,
                0.45f,
                0.0f,
                0.0f,
                0.0f,
                0.0f
            };
            ctx.vfxRenderTargets->ExecuteDebugPreviewPass(
                passContext.commandList,
                "DebugEmissivePreview",
                ctx.appPipelines->GetCompositeRootSignature(),
                ctx.appPipelines->GetDebugEmissivePreviewPSO(),
                ctx.vfxRenderTargets->GetSrvHandle("SceneColor"),
                ctx.vfxRenderTargets->GetSrvHandle("VfxAccumulation"),
                ctx.vfxRenderTargets->GetSrvHandle(finalPostOutput),
                debugParams);
        };
    emissivePreviewPass.forceExecute = true;
    if (enableDebugPreview) {
        ctx.renderGraph->AddPass(std::move(emissivePreviewPass));
    }

    ctx.renderGraph->AddPass({
        "UI.ImGui",
        ge3::graphics::RenderPassLayer::Ui,
        {
            {"BackBuffer", ge3::graphics::RenderResourceAccessType::WriteRtv},
        },
        "",
        [ctx](ge3::graphics::RenderPassContext& passContext) {
            ctx.imguiLayer->Render(passContext.commandList);
        }});
}
