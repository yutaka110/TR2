#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <string>

#include "../../externals/DirectXTex/DirectXTex.h"
#include "AppRuntimeState.h"
#include "utils/math/MathUtils.h"
#include "utils/math/Vector.h"
#include "utils/dx12/BufferHelper.h"
#include "ModelLoaderAssimp.h"
struct SphereMeshData {
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexResource;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    Microsoft::WRL::ComPtr<ID3D12Resource> cbvResource;
    TransformationMatrix* mappedCBV = nullptr;
    UINT vertexCount = 0;
};

struct CameraForGPU {
    Vector3 worldPosition;
    float padding;
};

class AppSceneResources {
public:
    bool Initialize(
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvDescriptorHeap,
        uint32_t descriptorSizeSRV);

    void UpdateCameraWorldPosition(const Vector3& worldPosition);
    void UpdateTransforms(
        const AppRuntimeState& runtimeState,
        Matrix4x4* wvpData,
        const Matrix4x4& viewMatrix,
        const Matrix4x4& projMatrix,
        uint32_t windowWidth,
        uint32_t windowHeight);
    void SyncRuntimeState(AppRuntimeState& runtimeState, float deltaTime);

public:
    // Sprite
    Microsoft::WRL::ComPtr<ID3D12Resource> indexResourceSprite;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexResourceSprite;
    D3D12_VERTEX_BUFFER_VIEW vertexBufferViewSprite{};
    D3D12_INDEX_BUFFER_VIEW indexBufferViewSprite{};
    Microsoft::WRL::ComPtr<ID3D12Resource> transformationMatrixResourceSprite;
    TransformationMatrix* transformationMatrixDataSprite = nullptr;

    // Material
    Microsoft::WRL::ComPtr<ID3D12Resource> materialResource;
    Material* materialData = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> materialResourceSprite;
    Material* materialDataSprite = nullptr;

    // Lights
    DirectionalLight directionalLightData{};
    Microsoft::WRL::ComPtr<ID3D12Resource> directionalLightResource;
    DirectionalLight* mappedLight = nullptr;

    PointLight pointLightData{};
    Microsoft::WRL::ComPtr<ID3D12Resource> pointLightResource;
    PointLight* mappedPointLight = nullptr;

    SpotLight spotLight{};
    Microsoft::WRL::ComPtr<ID3D12Resource> spotLightResource;
    SpotLight* mappedSpotLight = nullptr;

    // Camera
    Microsoft::WRL::ComPtr<ID3D12Resource> cameraResource;
    CameraForGPU* mappedCamera = nullptr;

    // Texture
    Microsoft::WRL::ComPtr<ID3D12Resource> textureResource;
    Microsoft::WRL::ComPtr<ID3D12Resource> textureResource2;
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPU{};
    D3D12_GPU_DESCRIPTOR_HANDLE textureSrvHandleGPU2{};
    D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU{};
    D3D12_CPU_DESCRIPTOR_HANDLE textureSrvHandleCPU2{};

    // Sphere
    SphereMeshData sphere{};

    // Model
    ModelData modelData{};
    Microsoft::WRL::ComPtr<ID3D12Resource> modelVertexResource;
    D3D12_VERTEX_BUFFER_VIEW modelVBV{};
    UINT modelVertexCount = 0;
};
