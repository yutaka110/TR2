#include "AppPostProcessPipeline.h"

#include <string>

#include "AppFrameGraphBuilder.h"
#include "AppFrameRenderer.h"
#include "AppPipelines.h"
#include "AppRuntimeState.h"
#include "AppSceneResources.h"
#include "AppVfxRenderTargets.h"
#include "PostProcessStack.h"
#include "graphics/RenderGraph.h"

namespace {
ID3D12PipelineState* ResolvePostProcessPso(const AppPipelines& pipelines, const std::string& name) {
    if (name == "BloomExtract") {
        return pipelines.GetBloomExtractPSO();
    }
    if (name == "BloomDownsample") {
        return pipelines.GetBloomDownsamplePSO();
    }
    if (name == "BloomUpsample") {
        return pipelines.GetBloomUpsamplePSO();
    }
    if (name == "BlurHorizontal") {
        return pipelines.GetBlurHorizontalPSO();
    }
    if (name == "BlurVertical") {
        return pipelines.GetBlurVerticalPSO();
    }
    if (name == "DistortionComposite") {
        return pipelines.GetDistortionCompositePSO();
    }
    if (name == "ToneMapping") {
        return pipelines.GetToneMappingPSO();
    }
    if (name == "GlowComposite") {
        return pipelines.GetGlowCompositePSO();
    }
    return nullptr;
}

void BuildPassParams(const PostProcessPass& postPass, float passParams[8]) {
    passParams[0] = postPass.intensity;
    passParams[1] = 0.0f;
    passParams[2] = 0.0f;
    passParams[3] = 0.0f;
    passParams[4] = 0.0f;
    passParams[5] = 0.0f;
    passParams[6] = 0.0f;
    passParams[7] = 0.0f;

    if (postPass.pipeline == "BloomExtract") {
        passParams[1] = postPass.parameters.bloomThresholdMin;
        passParams[2] = postPass.parameters.bloomThresholdMax;
        passParams[3] = postPass.parameters.bloomSoftKnee;
        return;
    }
    if (postPass.pipeline == "BloomDownsample") {
        return;
    }
    if (postPass.pipeline == "BloomUpsample") {
        passParams[1] = postPass.parameters.bloomUpsampleBlend;
        passParams[2] = postPass.parameters.bloomUpsampleSoftKnee;
        return;
    }
    if (postPass.pipeline == "BlurHorizontal" || postPass.pipeline == "BlurVertical") {
        passParams[1] = postPass.parameters.blurRadius;
        return;
    }
    if (postPass.pipeline == "DistortionComposite") {
        passParams[1] = postPass.parameters.distortionScale;
        return;
    }
    if (postPass.pipeline == "ToneMapping") {
        passParams[1] = postPass.parameters.toneExposure;
        return;
    }
    if (postPass.pipeline == "GlowComposite") {
        passParams[1] = postPass.parameters.glowWeight;
        passParams[2] = postPass.parameters.glowTintR;
        passParams[3] = postPass.parameters.glowTintG;
        passParams[4] = postPass.parameters.glowTintB;
        return;
    }
}

} // namespace

void AppPostProcessPipeline::RegisterPasses(const AppFrameGraphBuildContext& ctx) const {
    const bool enablePostProcess =
        ctx.runtimeState != nullptr &&
        ctx.runtimeState->enablePostProcessPasses;

    const PostProcessExecutionPlan executionPlan =
        enablePostProcess
        ? ctx.postProcessStack->BuildExecutionPlan()
        : PostProcessExecutionPlan{};
    const std::string finalOutputResource =
        enablePostProcess && !executionPlan.finalOutputResource.empty()
        ? executionPlan.finalOutputResource
        : "SceneColor";
    if (enablePostProcess) {
        for (const PostProcessExecutionPass& executionPass : executionPlan.passes) {
        const PostProcessPass& postPass = executionPass.pass;

        ctx.renderGraph->DeclareTransientRenderTarget(
            postPass.outputResource,
            postPass.resolutionScale,
            DXGI_FORMAT_R8G8B8A8_UNORM);
        ctx.renderGraph->AddPass({
            std::string("PostProcess.") + postPass.name,
            ge3::graphics::RenderPassLayer::PostProcess,
            {
                {postPass.inputResource, ge3::graphics::RenderResourceAccessType::ReadSrv},
                {postPass.secondaryInputResource, ge3::graphics::RenderResourceAccessType::ReadSrv},
                {postPass.tertiaryInputResource, ge3::graphics::RenderResourceAccessType::ReadSrv},
                {postPass.outputResource, ge3::graphics::RenderResourceAccessType::WriteRtv},
            },
            "",
            [ctx, postPass](ge3::graphics::RenderPassContext& passContext) {
                ID3D12DescriptorHeap* descriptorHeaps[] = { ctx.srvDescriptorHeap };
                passContext.commandList->SetDescriptorHeaps(1, descriptorHeaps);
                ID3D12PipelineState* pipelineState = ResolvePostProcessPso(*ctx.appPipelines, postPass.pipeline);
                if (pipelineState == nullptr) {
                    return;
                }
                float passParams[8] = {};
                BuildPassParams(postPass, passParams);
                ctx.vfxRenderTargets->ExecutePostProcessPass(
                    passContext.commandList,
                    postPass.outputResource,
                    ctx.appPipelines->GetCompositeRootSignature(),
                    pipelineState,
                    postPass.inputResource,
                    postPass.secondaryInputResource,
                    postPass.tertiaryInputResource,
                    passParams);
        }});
        }
    }

    ctx.renderGraph->AddPass({
        "PostProcess.CompositeToBackBuffer",
        ge3::graphics::RenderPassLayer::PostProcess,
        {
            {"SceneColor", ge3::graphics::RenderResourceAccessType::ReadSrv},
            {"VfxAccumulation", ge3::graphics::RenderResourceAccessType::ReadSrv},
            {finalOutputResource, ge3::graphics::RenderResourceAccessType::ReadSrv},
            {"BackBuffer", ge3::graphics::RenderResourceAccessType::WriteRtv},
        },
        "",
        [ctx, finalOutputResource](ge3::graphics::RenderPassContext& passContext) {
            ID3D12DescriptorHeap* descriptorHeaps[] = { ctx.srvDescriptorHeap };
            passContext.commandList->SetDescriptorHeaps(1, descriptorHeaps);
            const float compositeParams[8] = {
                0.0f,
                0.0f,
                0.0f,
                1.0f,
                0.0f,
                0.0f,
                0.0f,
                0.0f
            };
            ctx.vfxRenderTargets->CompositeToBackBuffer(
                passContext.commandList,
                ctx.backBuffer,
                ctx.rtv,
                ctx.appPipelines->GetCompositeRootSignature(),
                ctx.appPipelines->GetCompositePSO(),
                finalOutputResource,
                compositeParams);
        },
        true});

    const bool showReceivedVideoOverlay =
        ctx.runtimeState->showReceivedVideoInGame &&
        ctx.receivedTextureHandle.ptr != 0;
    if (showReceivedVideoOverlay) {
        ctx.renderGraph->AddPass({
            "UI.ReceivedVideoOverlay",
            ge3::graphics::RenderPassLayer::Ui,
            {
                {"BackBuffer", ge3::graphics::RenderResourceAccessType::WriteRtv},
            },
            "",
            [ctx](ge3::graphics::RenderPassContext& passContext) {
                const bool overlayReady = ctx.frameRenderer->PrepareMainPass(
                    passContext.commandList,
                    ctx.runtimeState->viewport,
                    ctx.runtimeState->scissorRect,
                    ctx.appPipelines->GetSpriteRootSignature(),
                    ctx.appPipelines->GetSpritePSO());

                if (overlayReady &&
                    ctx.scene->materialResourceSprite &&
                    ctx.scene->transformationMatrixResourceSprite) {
                    ctx.frameRenderer->DrawSprite(
                        passContext.commandList,
                        ctx.srvDescriptorHeap,
                        ctx.scene->indexBufferViewSprite,
                        ctx.scene->vertexBufferViewSprite,
                        ctx.scene->materialResourceSprite->GetGPUVirtualAddress(),
                        ctx.scene->transformationMatrixResourceSprite->GetGPUVirtualAddress(),
                        ctx.receivedTextureHandle);
                }
            },
            true});
    }
}
