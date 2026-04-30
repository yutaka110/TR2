#include "vfx/DistortionRenderer.h"

#include "../../AppPipelines.h"
#include "VfxComponentDraw.h"
#include "vfx/VfxResources.h"

void DistortionRenderer::Draw(
    ID3D12GraphicsCommandList* commandList,
    const VfxRenderContext& context,
    const DistortionRenderInput& input) const {
    const vfx::ComponentDrawParams drawParams =
        vfx::ResolveDistortionDrawParams(
            input.settings,
            {0.03f, 1.0f, 0.5f, 1.35f});
    const vfx::VfxRendererResourceSet* rendererResources =
        context.typedResources != nullptr ? &context.typedResources->distortion.renderer : nullptr;
    vfx::DrawIndirectSpriteComponents(
        commandList,
        context,
        context.appPipelines != nullptr ? context.appPipelines->GetDistortionSpritePSO() : nullptr,
        drawParams,
        rendererResources);
}
