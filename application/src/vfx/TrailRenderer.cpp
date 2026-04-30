#include "vfx/TrailRenderer.h"

#include "../../AppPipelines.h"
#include "VfxComponentDraw.h"
#include "vfx/VfxResources.h"

void TrailRenderer::Draw(
    ID3D12GraphicsCommandList* commandList,
    const VfxRenderContext& context,
    const TrailRenderInput& input) const {
    const vfx::ComponentDrawParams drawParams =
        vfx::ResolveTrailDrawParams(
            input.settings,
            {0.02f, 1.0f, 0.5f, 1.35f});
    const vfx::VfxRendererResourceSet* rendererResources =
        context.typedResources != nullptr ? &context.typedResources->trail.renderer : nullptr;
    vfx::DrawIndirectSpriteComponents(
        commandList,
        context,
        context.appPipelines != nullptr ? context.appPipelines->GetTrailMeshPSO() : nullptr,
        drawParams,
        rendererResources);
}
