#include "AppSceneResources.h"

#include <cassert>
#include <cstring>
#include <numbers>
#include <vector>

#include "AppRenderResources.h"
#include "AppRuntimeUtils.h"
#include "ModelLoaderAssimp.h"

using Microsoft::WRL::ComPtr;

namespace {

    struct SphereVertex {
        float position[4];
        float texcoord[2];
        float normal[3];
    };

    std::vector<SphereVertex> BuildSphereVertices(uint32_t stackCount, uint32_t sliceCount) {
        std::vector<SphereVertex> v;
        v.reserve(stackCount * sliceCount * 6);

        for (uint32_t y = 0; y < stackCount; ++y) {
            float v0 = (float)y / (float)stackCount;
            float v1 = (float)(y + 1) / (float)stackCount;
            float phi0 = v0 * std::numbers::pi_v<float>;
            float phi1 = v1 * std::numbers::pi_v<float>;

            for (uint32_t x = 0; x < sliceCount; ++x) {
                float u0 = (float)x / (float)sliceCount;
                float u1 = (float)(x + 1) / (float)sliceCount;
                float theta0 = u0 * (std::numbers::pi_v<float> *2.0f);
                float theta1 = u1 * (std::numbers::pi_v<float> *2.0f);

                auto MakeV = [](float phi, float theta, float u, float vTex) {
                    SphereVertex sv{};
                    float sx = std::sin(phi) * std::cos(theta);
                    float sy = std::cos(phi);
                    float sz = std::sin(phi) * std::sin(theta);

                    sv.position[0] = sx;
                    sv.position[1] = sy;
                    sv.position[2] = sz;
                    sv.position[3] = 1.0f;

                    sv.texcoord[0] = u;
                    sv.texcoord[1] = vTex;

                    sv.normal[0] = sx;
                    sv.normal[1] = sy;
                    sv.normal[2] = sz;
                    return sv;
                    };

                SphereVertex a = MakeV(phi0, theta0, u0, v0);
                SphereVertex b = MakeV(phi0, theta1, u1, v0);
                SphereVertex c = MakeV(phi1, theta0, u0, v1);
                SphereVertex d = MakeV(phi1, theta1, u1, v1);

                v.push_back(a);
                v.push_back(c);
                v.push_back(b);
                v.push_back(b);
                v.push_back(c);
                v.push_back(d);
            }
        }

        return v;
    }

} // namespace

bool AppSceneResources::Initialize(
    ComPtr<ID3D12Device> device,
    ComPtr<ID3D12DescriptorHeap> srvDescriptorHeap,
    uint32_t descriptorSizeSRV) {

    // =========================================================
    // Sprite geometry
    // =========================================================
    indexResourceSprite = CreateBufferResource(device, sizeof(uint32_t) * 6);
    vertexResourceSprite = CreateBufferResource(device, sizeof(VertexData) * 6);

    transformationMatrixResourceSprite =
        CreateBufferResource(device, sizeof(TransformationMatrix));
    transformationMatrixResourceSprite->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&transformationMatrixDataSprite));
    transformationMatrixDataSprite->WVP = MakeIdentity4x4();
    transformationMatrixDataSprite->World = MakeIdentity4x4();
    transformationMatrixDataSprite->WorldInverseTranspose = MakeIdentity4x4();

    indexBufferViewSprite.BufferLocation =
        indexResourceSprite->GetGPUVirtualAddress();
    indexBufferViewSprite.SizeInBytes = sizeof(uint32_t) * 6;
    indexBufferViewSprite.Format = DXGI_FORMAT_R32_UINT;

    vertexBufferViewSprite.BufferLocation =
        vertexResourceSprite->GetGPUVirtualAddress();
    vertexBufferViewSprite.SizeInBytes = sizeof(VertexData) * 6;
    vertexBufferViewSprite.StrideInBytes = sizeof(VertexData);

    uint32_t* indexDataSprite = nullptr;
    indexResourceSprite->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&indexDataSprite));
    indexDataSprite[0] = 0;
    indexDataSprite[1] = 1;
    indexDataSprite[2] = 2;
    indexDataSprite[3] = 3;
    indexDataSprite[4] = 4;
    indexDataSprite[5] = 5;

    VertexData* vertexDataSprite = nullptr;
    vertexResourceSprite->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&vertexDataSprite));

    vertexDataSprite[0].position = { -0.5f, -0.5f, 0.0f, 1.0f };
    vertexDataSprite[0].texcoord = { 0.0f, 1.0f };

    vertexDataSprite[1].position = { -0.5f,  0.5f, 0.0f, 1.0f };
    vertexDataSprite[1].texcoord = { 0.0f, 0.0f };

    vertexDataSprite[2].position = { 0.5f, -0.5f, 0.0f, 1.0f };
    vertexDataSprite[2].texcoord = { 1.0f, 1.0f };

    vertexDataSprite[3].position = { -0.5f,  0.5f, 0.0f, 1.0f };
    vertexDataSprite[3].texcoord = { 0.0f, 0.0f };

    vertexDataSprite[4].position = { 0.5f,  0.5f, 0.0f, 1.0f };
    vertexDataSprite[4].texcoord = { 1.0f, 0.0f };

    vertexDataSprite[5].position = { 0.5f, -0.5f, 0.0f, 1.0f };
    vertexDataSprite[5].texcoord = { 1.0f, 1.0f };

    // =========================================================
    // Materials
    // =========================================================
    materialResource = CreateBufferResource(device, sizeof(Material));
    materialResource->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
    materialData->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    materialData->enableLighting = true;
    materialData->shininess = 3.0f;
    materialData->uvTransform = MakeIdentity4x4();

    materialResourceSprite = CreateBufferResource(device, sizeof(Material));
    materialResourceSprite->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&materialDataSprite));
    materialDataSprite->color = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    materialDataSprite->enableLighting = false;
    materialDataSprite->shininess = 1.0f;
    materialDataSprite->uvTransform = MakeIdentity4x4();

    // =========================================================
    // Lights
    // =========================================================
    directionalLightData.color = { 1.0f, 1.0f, 1.0f, 1.0f };
    Vector3 dir = { 0.0f, -1.0f, 0.0f };
    dir = Normalize(dir);
    directionalLightData.direction = dir;
    directionalLightData.intensity = 1.5f;

    directionalLightResource = CreateBufferResource(device, sizeof(DirectionalLight));
    directionalLightResource->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&mappedLight));
    *mappedLight = directionalLightData;

    pointLightData.color = { 1,1,1,1 };
    pointLightData.position = { 0.0f, 2.0f, 0.0f };
    pointLightData.intensity = 1.0f;
    pointLightData.radius = 10.0f;
    pointLightData.decay = 2.0f;

    pointLightResource = CreateBufferResource(device, sizeof(PointLight));
    pointLightResource->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&mappedPointLight));
    *mappedPointLight = pointLightData;

    spotLight.color = { 1,1,1,1 };
    spotLight.position = { 2.0f, 1.25f, 0.0f };
    spotLight.direction = Normalize(Vector3{ -1.0f, -1.0f, 0.0f });
    spotLight.distance = 7.0f;
    spotLight.intensity = 5.0f;
    spotLight.decay = 2.0f;
    spotLight.cosAngle = std::cos(std::numbers::pi_v<float> / 3.0f);

    spotLightResource = CreateBufferResource(device, sizeof(SpotLight));
    spotLightResource->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&mappedSpotLight));
    *mappedSpotLight = spotLight;

    // =========================================================
    // Camera
    // =========================================================
    cameraResource = CreateBufferResource(device, sizeof(CameraForGPU));
    cameraResource->Map(0, nullptr, reinterpret_cast<void**>(&mappedCamera));
    mappedCamera->worldPosition = Vector3{ 0.0f, 0.0f, -5.0f };
    mappedCamera->padding = 0.0f;

    // =========================================================
    // Texture 2枚
    // slot 1, 2 を使用（slot 0 は ImGui 用の前提）
    // =========================================================
    DirectX::ScratchImage mipImages = AppRenderResources::LoadTexture("resources/monsterBall.png");
    const DirectX::TexMetadata& metadata = mipImages.GetMetadata();
    textureResource = AppRenderResources::CreateTextureResource(device, metadata);
    AppRenderResources::UploadTextureData(textureResource, mipImages);

    DirectX::ScratchImage mipImages2 = AppRenderResources::LoadTexture("resources/monsterBall.png");
    const DirectX::TexMetadata& metadata2 = mipImages2.GetMetadata();
    textureResource2 = AppRenderResources::CreateTextureResource(device, metadata2);
    AppRenderResources::UploadTextureData(textureResource2, mipImages2);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = metadata.format;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = UINT(metadata.mipLevels);

    textureSrvHandleCPU = AppRenderResources::GetCPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 1);
    textureSrvHandleGPU = AppRenderResources::GetGPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 1);
    device->CreateShaderResourceView(textureResource.Get(), &srvDesc, textureSrvHandleCPU);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc2{};
    srvDesc2.Format = metadata2.format;
    srvDesc2.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc2.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc2.Texture2D.MipLevels = UINT(metadata2.mipLevels);

    textureSrvHandleCPU2 = AppRenderResources::GetCPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 2);
    textureSrvHandleGPU2 = AppRenderResources::GetGPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 2);
    device->CreateShaderResourceView(textureResource2.Get(), &srvDesc2, textureSrvHandleCPU2);

    // =========================================================
    // Sphere mesh
    // =========================================================
    {
        const uint32_t sphereStacks = 32;
        const uint32_t sphereSlices = 64;
        std::vector<SphereVertex> sphereVerts =
            BuildSphereVertices(sphereStacks, sphereSlices);

        sphere.vertexCount = (UINT)sphereVerts.size();
        sphere.vertexResource =
            CreateBufferResource(device, sizeof(SphereVertex) * sphere.vertexCount);

        SphereVertex* mappedVB = nullptr;
        sphere.vertexResource->Map(
            0,
            nullptr,
            reinterpret_cast<void**>(&mappedVB));
        memcpy(mappedVB, sphereVerts.data(), sizeof(SphereVertex) * sphere.vertexCount);

        sphere.vbv.BufferLocation = sphere.vertexResource->GetGPUVirtualAddress();
        sphere.vbv.SizeInBytes = (UINT)(sizeof(SphereVertex) * sphere.vertexCount);
        sphere.vbv.StrideInBytes = sizeof(SphereVertex);

        sphere.cbvResource = CreateBufferResource(device, sizeof(TransformationMatrix));
        sphere.cbvResource->Map(
            0,
            nullptr,
            reinterpret_cast<void**>(&sphere.mappedCBV));
        sphere.mappedCBV->WVP = MakeIdentity4x4();
        sphere.mappedCBV->World = MakeIdentity4x4();
        sphere.mappedCBV->WorldInverseTranspose = MakeIdentity4x4();
    }

    // =========================================================
    // Assimp model VB
    // =========================================================
    modelData = LoadObjFile_Assimp("Resources/ball", "ball.obj");
    assert(!modelData.vertices.empty());

    modelVertexResource =
        CreateBufferResource(device, sizeof(VertexData) * modelData.vertices.size());

    VertexData* mapped = nullptr;
    modelVertexResource->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    memcpy(mapped, modelData.vertices.data(), sizeof(VertexData) * modelData.vertices.size());

    modelVBV.BufferLocation = modelVertexResource->GetGPUVirtualAddress();
    modelVBV.SizeInBytes = UINT(sizeof(VertexData) * modelData.vertices.size());
    modelVBV.StrideInBytes = sizeof(VertexData);

    modelVertexCount = UINT(modelData.vertices.size());

    return true;
}

void AppSceneResources::UpdateCameraWorldPosition(const Vector3& worldPosition) {
    if (mappedCamera == nullptr) {
        return;
    }

    mappedCamera->worldPosition = worldPosition;
}

void AppSceneResources::UpdateTransforms(
    const AppRuntimeState& runtimeState,
    Matrix4x4* wvpData,
    const Matrix4x4& viewMatrix,
    const Matrix4x4& projMatrix,
    uint32_t windowWidth,
    uint32_t windowHeight) {
    if (wvpData != nullptr) {
        Matrix4x4 worldMatrix = MakeAffineMatrix(
            runtimeState.transform.scale,
            runtimeState.transform.rotate,
            runtimeState.transform.translate);
        *wvpData = Multiply(worldMatrix, Multiply(viewMatrix, projMatrix));
    }

    if (transformationMatrixDataSprite != nullptr) {
        Matrix4x4 worldMatrixSprite = MakeAffineMatrix(
            runtimeState.transformSprite.scale,
            runtimeState.transformSprite.rotate,
            runtimeState.transformSprite.translate);
        Matrix4x4 projectionMatrixSprite = MakeOrthographicMatrix(
            0.0f,
            0.0f,
            float(windowWidth),
            float(windowHeight),
            0.0f,
            100.0f);
        transformationMatrixDataSprite->World = worldMatrixSprite;
        transformationMatrixDataSprite->WVP = Multiply(
            worldMatrixSprite,
            Multiply(MakeIdentity4x4(), projectionMatrixSprite));
        transformationMatrixDataSprite->WorldInverseTranspose =
            Transpose(Inverse(worldMatrixSprite));
    }

    if (materialDataSprite != nullptr) {
        Matrix4x4 uvTransformMatrix = MakeScaleMatrix(runtimeState.uvTransformSprite.scale);
        uvTransformMatrix = Multiply(
            uvTransformMatrix,
            MakeRoateZMatrix(runtimeState.uvTransformSprite.rotate.z));
        uvTransformMatrix = Multiply(
            uvTransformMatrix,
            MakeTranslateMatrix(runtimeState.uvTransformSprite.translate));
        materialDataSprite->uvTransform = uvTransformMatrix;
    }

    if (sphere.mappedCBV != nullptr) {
        Matrix4x4 worldMatrixSphere = MakeAffineMatrix(
            runtimeState.transform.scale,
            runtimeState.transform.rotate,
            runtimeState.transform.translate);
        const Matrix4x4& rootLocal = modelData.rootNode.localMatrix;
        Matrix4x4 worldWithNode = Multiply(rootLocal, worldMatrixSphere);
        Matrix4x4 wvpWithNode = Multiply(worldWithNode, Multiply(viewMatrix, projMatrix));
        sphere.mappedCBV->World = worldWithNode;
        sphere.mappedCBV->WVP = wvpWithNode;
        sphere.mappedCBV->WorldInverseTranspose = Transpose(Inverse(worldWithNode));
    }
}

void AppSceneResources::SyncRuntimeState(AppRuntimeState& runtimeState, float deltaTime) {
    directionalLightData = runtimeState.directionalLightData;
    directionalLightData.direction = Normalize(directionalLightData.direction);
    runtimeState.directionalLightData.direction = directionalLightData.direction;
    if (mappedLight != nullptr) {
        *mappedLight = directionalLightData;
    }

    pointLightData = runtimeState.pointLightData;
    pointLightData.position.x = sinf(deltaTime) * 2.0f;
    runtimeState.pointLightData.position = pointLightData.position;
    if (mappedPointLight != nullptr) {
        *mappedPointLight = pointLightData;
    }

    spotLight = runtimeState.spotLight;
    spotLight.direction = Normalize(spotLight.direction);
    runtimeState.spotLight.direction = spotLight.direction;
    if (mappedSpotLight != nullptr) {
        *mappedSpotLight = spotLight;
    }

    if (materialData != nullptr) {
        *materialData = runtimeState.materialData;
    }
}
