#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

namespace ge3::resources {

struct NamedRenderTarget {
    std::string name;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_GPU_DESCRIPTOR_HANDLE srv{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
};

class ResourceRegistry {
public:
    void Clear();
    void RegisterRenderTarget(NamedRenderTarget renderTarget);
    const NamedRenderTarget* FindRenderTarget(std::string_view name) const;

private:
    std::vector<NamedRenderTarget> renderTargets_;
    std::unordered_map<std::string, size_t> renderTargetLookup_;
};

struct EffectTextureResource {
    std::string name;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    uint32_t width = 0;
    uint32_t height = 0;
};

class EffectResourceCache {
public:
    void BeginFrame();
    void RegisterTexture(EffectTextureResource texture);
    const EffectTextureResource* FindTexture(std::string_view name) const;
    D3D12_GPU_DESCRIPTOR_HANDLE ResolveTexture(
        std::string_view name,
        D3D12_GPU_DESCRIPTOR_HANDLE fallback) const;
    size_t TextureCount() const { return textures_.size(); }

private:
    std::vector<EffectTextureResource> textures_;
    std::unordered_map<std::string, size_t> textureLookup_;
};

class FrameTransientAllocator {
public:
    void BeginFrame();
    void Track(Microsoft::WRL::ComPtr<ID3D12Resource> resource);
    size_t ResourceCount() const { return frameResources_.size(); }

private:
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> frameResources_;
};

} // namespace ge3::resources
