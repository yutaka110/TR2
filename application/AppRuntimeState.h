#pragma once

#include <cstdint>
#include <d3d12.h>

#include "../network/NetworkRuntimeMode.h"
#include "utils/math/MathUtils.h"

struct RuntimeAabbState {
    Vector3 min{};
    Vector3 max{};
};

struct RuntimeEmitterState {
    Transform transform{};
    uint32_t count = 0;
    float frequency = 0.0f;
    float frequencyTime = 0.0f;
};

struct RuntimeAccelerationFieldState {
    Vector3 acceleration{};
    RuntimeAabbState area{};
};

struct AppRuntimeState {
    Transform transform{};
    Transform transformSprite{};
    Transform uvTransformSprite{};
    Material materialData{};

    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissorRect{};
    float clearColor[4] = { 0.35f, 0.5f, 0.8f, 1.0f };

    RuntimeEmitterState emitter{};
    RuntimeAccelerationFieldState accelerationField{};

    DirectionalLight directionalLightData{};
    PointLight pointLightData{};
    SpotLight spotLight{};
    Vector3 cameraWorldPosition{};

    bool useMonsterBall = true;
    // RNVPで受信した映像テクスチャをゲーム画面内のSpriteに表示する
    bool showReceivedVideoInGame = true;
    bool showReceivedVideoPreviewWindow = true;
    bool lowLatencyPresentMode = false;
    bool waitableSwapChainPacingEnabled = false;
    bool showImGui = true;
    bool networkExperimentMode = false;
    net::NetworkRuntimeMode networkRuntimeMode =
        net::NetworkRuntimeMode::Loopback;
    bool enableVfxRenderPasses = true;
    bool enablePostProcessPasses = true;
    bool enableDebugPreviewPasses = true;
    bool enableParticles = false;
    bool autoPlayVfxDemo = true;
    float autoPlayVfxInterval = 0.6f;
    float autoPlayVfxRadius = 2.5f;
    float autoPlayVfxTimer = 0.0f;
    float autoPlayVfxAngle = 0.0f;
    float debugDepthPreviewNear = 0.1f;
    float debugDepthPreviewFar = 25.0f;
    float debugDepthPreviewPower = 1.35f;
    float debugEmissivePreviewBoost = 2.0f;
};
