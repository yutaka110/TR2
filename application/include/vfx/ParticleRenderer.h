#pragma once

#include <vector>

#include "vfx/VfxRenderInputs.h"
#include "vfx/VfxRenderContext.h"

class ParticleRenderer {
public:
    void Simulate(
        ID3D12GraphicsCommandList* commandList,
        const VfxRenderContext& context,
        const ParticleRenderInput& input) const;

    void Draw(
        ID3D12GraphicsCommandList* commandList,
        const VfxRenderContext& context,
        const ParticleRenderInput& input) const;
};
