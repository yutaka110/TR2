#include "AppRenderResources.h"

#include "AppRuntimeUtils.h"

#include "utils/dx12/BufferHelper.h"
#include "utils/math/Vector.h"

using Microsoft::WRL::ComPtr;

ComPtr<ID3D12DescriptorHeap> AppRenderResources::CreateDescriptorHeap(
    ComPtr<ID3D12Device> device,
    D3D12_DESCRIPTOR_HEAP_TYPE heapType,
    UINT numDescriptors,
    bool shaderVisible) {
    ComPtr<ID3D12DescriptorHeap> descriptorHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
    descriptorHeapDesc.Type = heapType;
    descriptorHeapDesc.NumDescriptors = numDescriptors;
    descriptorHeapDesc.Flags = shaderVisible
        ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
        : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    HRESULT hr = device->CreateDescriptorHeap(
        &descriptorHeapDesc,
        IID_PPV_ARGS(&descriptorHeap));
    assert(SUCCEEDED(hr));
    return descriptorHeap;
}

DirectX::ScratchImage AppRenderResources::LoadTexture(const std::string& filePath) {
    DirectX::ScratchImage image{};
    std::wstring filePathW = ConvertString(filePath);
    HRESULT hr = DirectX::LoadFromWICFile(
        filePathW.c_str(),
        DirectX::WIC_FLAGS_FORCE_SRGB,
        nullptr,
        image);
    assert(SUCCEEDED(hr));

    DirectX::ScratchImage mipImages{};
    hr = DirectX::GenerateMipMaps(
        image.GetImages(),
        image.GetImageCount(),
        image.GetMetadata(),
        DirectX::TEX_FILTER_SRGB,
        0,
        mipImages);
    assert(SUCCEEDED(hr));

    return mipImages;
}

ComPtr<ID3D12Resource> AppRenderResources::CreateTextureResource(
    ComPtr<ID3D12Device> device,
    const DirectX::TexMetadata& metadata) {
    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Width = UINT(metadata.width);
    resourceDesc.Height = UINT(metadata.height);
    resourceDesc.MipLevels = UINT16(metadata.mipLevels);
    resourceDesc.DepthOrArraySize = UINT16(metadata.arraySize);
    resourceDesc.Format = metadata.format;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION(metadata.dimension);

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_CUSTOM;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;

    ComPtr<ID3D12Resource> resource = nullptr;
    HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&resource));

    assert(SUCCEEDED(hr));
    return resource;
}

void AppRenderResources::UploadTextureData(
    ComPtr<ID3D12Resource> texture,
    const DirectX::ScratchImage& mipImages) {
    const DirectX::TexMetadata& metadata = mipImages.GetMetadata();

    for (size_t mipLevel = 0; mipLevel < metadata.mipLevels; ++mipLevel) {
        const DirectX::Image* img = mipImages.GetImage(mipLevel, 0, 0);
        HRESULT hr = texture->WriteToSubresource(
            UINT(mipLevel),
            nullptr,
            img->pixels,
            UINT(img->rowPitch),
            UINT(img->slicePitch));
        assert(SUCCEEDED(hr));
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE AppRenderResources::GetCPUDescriptorHandle(
    ComPtr<ID3D12DescriptorHeap> descriptorHeap,
    uint32_t descriptorSize,
    uint32_t index) {
    D3D12_CPU_DESCRIPTOR_HANDLE handleCPU =
        descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    handleCPU.ptr += descriptorSize * index;
    return handleCPU;
}

D3D12_GPU_DESCRIPTOR_HANDLE AppRenderResources::GetGPUDescriptorHandle(
    ComPtr<ID3D12DescriptorHeap> descriptorHeap,
    uint32_t descriptorSize,
    uint32_t index) {
    D3D12_GPU_DESCRIPTOR_HANDLE handleGPU =
        descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    handleGPU.ptr += descriptorSize * index;
    return handleGPU;
}

bool AppRenderResources::InitializeParticleQuad(ComPtr<ID3D12Device> device) {
    particleVertexResource_ = CreateBufferResource(device, sizeof(VertexData) * 6);
    if (particleVertexResource_ == nullptr) {
        return false;
    }

    VertexData* vertices = nullptr;
    HRESULT hr = particleVertexResource_->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&vertices));
    if (FAILED(hr) || vertices == nullptr) {
        return false;
    }

    vertices[0].position = {-0.5f, -0.5f, 0.0f, 1.0f};
    vertices[0].texcoord = {0.0f, 1.0f};

    vertices[1].position = {-0.5f, 0.5f, 0.0f, 1.0f};
    vertices[1].texcoord = {0.0f, 0.0f};

    vertices[2].position = {0.5f, -0.5f, 0.0f, 1.0f};
    vertices[2].texcoord = {1.0f, 1.0f};

    vertices[3].position = {-0.5f, 0.5f, 0.0f, 1.0f};
    vertices[3].texcoord = {0.0f, 0.0f};

    vertices[4].position = {0.5f, 0.5f, 0.0f, 1.0f};
    vertices[4].texcoord = {1.0f, 0.0f};

    vertices[5].position = {0.5f, -0.5f, 0.0f, 1.0f};
    vertices[5].texcoord = {1.0f, 1.0f};

    particleVertexResource_->Unmap(0, nullptr);

    particleVertexBufferView_.BufferLocation = particleVertexResource_->GetGPUVirtualAddress();
    particleVertexBufferView_.SizeInBytes = sizeof(VertexData) * 6;
    particleVertexBufferView_.StrideInBytes = sizeof(VertexData);
    return true;
}

const D3D12_VERTEX_BUFFER_VIEW& AppRenderResources::ParticleVertexBufferView() const {
    return particleVertexBufferView_;
}
