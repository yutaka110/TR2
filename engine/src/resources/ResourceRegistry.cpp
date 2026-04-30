#include "resources/ResourceRegistry.h"

#include <utility>

namespace ge3::resources {

void ResourceRegistry::Clear() {
    renderTargets_.clear();
    renderTargetLookup_.clear();
}

void ResourceRegistry::RegisterRenderTarget(NamedRenderTarget renderTarget) {
    if (renderTarget.name.empty()) {
        return;
    }

    const std::string name = renderTarget.name;
    const auto found = renderTargetLookup_.find(name);
    if (found != renderTargetLookup_.end()) {
        renderTargets_[found->second] = std::move(renderTarget);
        return;
    }

    const size_t index = renderTargets_.size();
    renderTargets_.push_back(std::move(renderTarget));
    renderTargetLookup_.emplace(name, index);
}

const NamedRenderTarget* ResourceRegistry::FindRenderTarget(std::string_view name) const {
    const auto found = renderTargetLookup_.find(std::string(name));
    if (found == renderTargetLookup_.end()) {
        return nullptr;
    }
    return &renderTargets_[found->second];
}

void EffectResourceCache::BeginFrame() {
    textures_.clear();
    textureLookup_.clear();
}

void EffectResourceCache::RegisterTexture(EffectTextureResource texture) {
    if (texture.name.empty() || texture.gpu.ptr == 0) {
        return;
    }

    const std::string name = texture.name;
    const auto found = textureLookup_.find(name);
    if (found != textureLookup_.end()) {
        textures_[found->second] = texture;
        return;
    }

    const size_t index = textures_.size();
    textures_.push_back(std::move(texture));
    textureLookup_.emplace(name, index);
}

const EffectTextureResource* EffectResourceCache::FindTexture(std::string_view name) const {
    const auto found = textureLookup_.find(std::string(name));
    if (found == textureLookup_.end()) {
        return nullptr;
    }
    return &textures_[found->second];
}

D3D12_GPU_DESCRIPTOR_HANDLE EffectResourceCache::ResolveTexture(
    std::string_view name,
    D3D12_GPU_DESCRIPTOR_HANDLE fallback) const {
    const EffectTextureResource* texture = FindTexture(name);
    if (texture == nullptr) {
        return fallback;
    }
    return texture->gpu;
}

void FrameTransientAllocator::BeginFrame() {
    frameResources_.clear();
}

void FrameTransientAllocator::Track(Microsoft::WRL::ComPtr<ID3D12Resource> resource) {
    if (resource != nullptr) {
        frameResources_.push_back(std::move(resource));
    }
}

} // namespace ge3::resources
