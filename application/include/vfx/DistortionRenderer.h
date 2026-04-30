#pragma once

#include <vector>

#include "vfx/VfxRenderInputs.h"
#include "vfx/VfxRenderContext.h"

class DistortionRenderer {
public:
    void Draw(
        ID3D12GraphicsCommandList* commandList,
        const VfxRenderContext& context,
        const DistortionRenderInput& input) const;
};
