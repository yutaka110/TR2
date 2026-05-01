// D3D12Helpers.cpp
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <cassert>

using Microsoft::WRL::ComPtr;

// 例外や ThrowIfFailed を使わない方針なので HRESULT をチェックして失敗時は nullptr を返す

// ComPtr<ID3D12Resource> CreateTextureResourceResolution(ComPtr<ID3D12Device> device, UINT w, UINT h, DXGI_FORMAT fmt)
ComPtr<ID3D12Resource> CreateTextureResourceResolution(ComPtr<ID3D12Device> device, UINT w, UINT h, DXGI_FORMAT fmt)
{
    ComPtr<ID3D12Resource> tex;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;            // 例: DXGI_FORMAT_NV12 / R8G8B8A8_UNORM など
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->CreateCommittedResource(
        &heapDefault,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, // SRV前提の初期状態
        nullptr,
        IID_PPV_ARGS(&tex)
    );
    if (FAILED(hr)) return nullptr;

    return tex;
}

// void CreateTextureSRV(ComPtr<ID3D12Device> device, ComPtr<ID3D12Resource> res, D3D12_CPU_DESCRIPTOR_HANDLE handle)
void CreateTextureSRV(ComPtr<ID3D12Device> device, ComPtr<ID3D12Resource> res, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    if (!device || !res) return;

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    const auto desc = res->GetDesc();
    sd.Format = desc.Format;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;

    // NV12 をそのまま SRV にする場合
    // ※実際のシェーダでY/UVを分けて使うなら PlaneSlice を使った2つのSRVを作る設計にすること。
    device->CreateShaderResourceView(res.Get(), &sd, handle);
}

// ComPtr<ID3D12Resource> CreateTextureWithUAV(ComPtr<ID3D12Device> device, UINT w, UINT h, DXGI_FORMAT fmt)
ComPtr<ID3D12Resource> CreateTextureWithUAV(ComPtr<ID3D12Device> device, UINT w, UINT h, DXGI_FORMAT fmt)
{
    ComPtr<ID3D12Resource> tex;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = fmt;            // UAV対応フォーマットを指定
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heapDefault = {};
    heapDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->CreateCommittedResource(
        &heapDefault,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, // UAV前提の初期状態
        nullptr,
        IID_PPV_ARGS(&tex)
    );
    if (FAILED(hr)) return nullptr;

    return tex;
}
