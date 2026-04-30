#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef> // size_t

// Upload ヒープ上にバッファリソースを 1 本作るヘルパー。
// main.cpp に書いてあった CreateBufferResource を外出ししたもの。
Microsoft::WRL::ComPtr<ID3D12Resource> CreateBufferResource(
    Microsoft::WRL::ComPtr<ID3D12Device> device,
    size_t sizeInBytes);
