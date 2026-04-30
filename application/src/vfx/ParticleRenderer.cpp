#include "vfx/ParticleRenderer.h"

#include "../../AppGpuParticleSystem.h"
#include "../../AppPipelines.h"
#include "../../AppFrameState.h"
#include "VfxComponentDraw.h"
#include "vfx/VfxResources.h"

void ParticleRenderer::Simulate(
    ID3D12GraphicsCommandList* commandList,
    const VfxRenderContext& context,
    const ParticleRenderInput& input) const {
    if (commandList == nullptr ||
        context.srvDescriptorHeap == nullptr ||
        context.appPipelines == nullptr ||
        context.gpuParticleSystem == nullptr ||
        context.frameState == nullptr) {
        return;
    }

    ID3D12DescriptorHeap* descriptorHeaps[] = { context.srvDescriptorHeap };
    commandList->SetDescriptorHeaps(1, descriptorHeaps);

    Vector4 tint = {1.0f, 1.0f, 1.0f, 1.0f};
    Vector3 scale = {1.0f, 1.0f, 1.0f};
    float emissive = 1.0f;
    float turbulence = 0.0f;
    float pulseSpeed = 5.0f;
    float spawnRadius = 4.0f;
    float uvScrollSpeed = 0.0f;

    if (input.primary.instance != nullptr &&
        input.primary.componentCommon != nullptr &&
        input.settings != nullptr) {
        const EffectInstance& instance = *input.primary.instance;
        const EffectComponentCommon& component = *input.primary.componentCommon;
        const EffectParticleSettings& settings = *input.settings;
        tint = {
            instance.color.x * component.color.x,
            instance.color.y * component.color.y,
            instance.color.z * component.color.z,
            instance.color.w * component.color.w,
        };
        scale = instance.transform.scale;
        emissive = settings.emissive;
        turbulence = settings.noiseStrength + settings.distortionStrength;
        pulseSpeed = settings.pulseSpeed;
        spawnRadius = settings.spawnRadius;
        uvScrollSpeed = settings.uvScrollSpeed;
    } else if (input.fallbackCommon != nullptr && input.fallbackSettings != nullptr) {
        tint = input.fallbackCommon->color;
        scale = input.fallbackCommon->size;
        emissive = input.fallbackSettings->emissive;
        turbulence = input.fallbackSettings->noiseStrength + input.fallbackSettings->distortionStrength;
        pulseSpeed = input.fallbackSettings->pulseSpeed;
        spawnRadius = input.fallbackSettings->spawnRadius;
        uvScrollSpeed = input.fallbackSettings->uvScrollSpeed;
    }

    context.gpuParticleSystem->Simulate(
        commandList,
        context.appPipelines->GetGpuParticleComputeRootSignature(),
        context.appPipelines->GetGpuParticleComputePSO(),
        context.frameState->viewProjectionMatrix,
        0.016f,
        context.beamTime,
        tint,
        scale,
        emissive,
        turbulence,
        pulseSpeed,
        spawnRadius,
        uvScrollSpeed);
}

void ParticleRenderer::Draw(
    ID3D12GraphicsCommandList* commandList,
    const VfxRenderContext& context,
    const ParticleRenderInput& input) const {
    const vfx::ComponentDrawParams drawParams =
        vfx::ResolveParticleDrawParams(
            input.settings,
            input.fallbackSettings,
            {0.02f, 1.0f, 0.5f, 1.35f});
    const vfx::VfxRendererResourceSet* rendererResources =
        context.typedResources != nullptr ? &context.typedResources->particle.renderer : nullptr;
    vfx::DrawIndirectSpriteComponents(
        commandList,
        context,
        context.appPipelines != nullptr ? context.appPipelines->GetParticleAlphaPSO() : nullptr,
        drawParams,
        rendererResources);
}
