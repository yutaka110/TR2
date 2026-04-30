#include "AppSceneRenderPipeline.h"

#include <Windows.h>

#include "AppFrameGraphBuilder.h"
#include "AppFrameRenderer.h"
#include "AppPipelines.h"
#include "AppRuntimeState.h"
#include "AppSceneResources.h"
#include "graphics/RenderGraph.h"

void AppSceneRenderPipeline::RegisterPasses(const AppFrameGraphBuildContext& ctx) const {
    const float opaqueBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx.renderGraph->DeclarePersistentRenderTarget(
        "SceneColor",
        1.0f,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        opaqueBlack,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    ctx.renderGraph->DeclarePersistentDepthTarget(
        "SceneDepth",
        DXGI_FORMAT_D24_UNORM_S8_UINT,
        1.0f,
        0,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ctx.renderGraph->DeclarePersistentDepthTarget(
        "SceneDepthReadOnly",
        DXGI_FORMAT_D24_UNORM_S8_UINT,
        1.0f,
        0,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);

    ctx.renderGraph->AddPass({
        "Geometry.Sprite",
        ge3::graphics::RenderPassLayer::Geometry,
        {
            {"SceneColor", ge3::graphics::RenderResourceAccessType::WriteRtv},
            {"SceneDepth", ge3::graphics::RenderResourceAccessType::WriteDepth},
        },
        "SceneDepth",
        [ctx](ge3::graphics::RenderPassContext& passContext) {
            const bool spritePassReady = ctx.frameRenderer->PrepareMainPass(
                passContext.commandList,
                ctx.runtimeState->viewport,
                ctx.runtimeState->scissorRect,
                ctx.appPipelines->GetSpriteRootSignature(),
                ctx.appPipelines->GetSpritePSO());

            if (spritePassReady &&
                ctx.scene->materialResourceSprite &&
                ctx.scene->transformationMatrixResourceSprite) {
                ctx.frameRenderer->DrawSprite(
                    passContext.commandList,
                    ctx.srvDescriptorHeap,
                    ctx.scene->indexBufferViewSprite,
                    ctx.scene->vertexBufferViewSprite,
                    ctx.scene->materialResourceSprite->GetGPUVirtualAddress(),
                    ctx.scene->transformationMatrixResourceSprite->GetGPUVirtualAddress(),
                    ctx.spriteTextureHandle);
            } else {
                OutputDebugStringA("[AppSceneRenderPipeline] Sprite pass skipped because pipeline or resources are not ready.\n");
            }
        }});

    ctx.renderGraph->AddPass({
        "Geometry.MainModel",
        ge3::graphics::RenderPassLayer::Geometry,
        {
            {"SceneColor", ge3::graphics::RenderResourceAccessType::WriteRtv},
            {"SceneDepth", ge3::graphics::RenderResourceAccessType::WriteDepth},
        },
        "SceneDepth",
        [ctx](ge3::graphics::RenderPassContext& passContext) {
            ctx.frameRenderer->PrepareMainPass(
                passContext.commandList,
                ctx.runtimeState->viewport,
                ctx.runtimeState->scissorRect,
                ctx.appPipelines->GetMainRootSignature(),
                ctx.appPipelines->GetMainPSO());

            ctx.frameRenderer->DrawMainModel(
                passContext.commandList,
                ctx.scene->modelVBV,
                ctx.scene->materialResource->GetGPUVirtualAddress(),
                ctx.scene->sphere.cbvResource->GetGPUVirtualAddress(),
                ctx.scene->textureSrvHandleGPU2,
                ctx.scene->textureSrvHandleGPU2,
                ctx.scene->textureSrvHandleGPU2,
                ctx.scene->directionalLightResource->GetGPUVirtualAddress(),
                ctx.scene->cameraResource->GetGPUVirtualAddress(),
                ctx.scene->pointLightResource->GetGPUVirtualAddress(),
                ctx.scene->spotLightResource->GetGPUVirtualAddress(),
                ctx.scene->modelVertexCount);
        }});
}
