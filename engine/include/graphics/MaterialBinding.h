#pragma once

#include <array>
#include <cstdint>

#include <d3d12.h>

namespace ge3::graphics {

enum class BlendMode {
    Opaque,
    Alpha,
    Additive,
    Distortion,
};

enum class DepthMode {
    ReadWrite,
    ReadOnly,
    Disabled,
};

struct PassState {
    BlendMode blend = BlendMode::Opaque;
    DepthMode depth = DepthMode::ReadWrite;
    D3D12_CULL_MODE cullMode = D3D12_CULL_MODE_BACK;
    uint32_t renderQueue = 0;
};

struct ShaderParameterBlock {
    static constexpr uint32_t kMaxCbvSlots = 16;
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, kMaxCbvSlots> cbv{};
};

struct TextureSet {
    static constexpr uint32_t kMaxSrvSlots = 16;
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxSrvSlots> srv{};
};

struct MaterialInstance {
    const char* name = "";
    PassState passState{};
    ShaderParameterBlock parameters{};
    TextureSet textures{};
};

} // namespace ge3::graphics
