#include "AppRunLoop.h"

#include "../../externals/imgui/imgui.h"
#include <DirectXMath.h>

#include "AppFrameRenderer.h"
#include "AppImGuiLayer.h"
#include "AppParticleSystem.h"
#include "AppPipelines.h"
#include "AppRenderResources.h"
#include "AppRuntimeState.h"
#include "AppSceneResources.h"
#include "EngineContext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cstring>

using namespace DirectX;
using namespace Microsoft::WRL;

namespace {
double ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

uint64_t NowMicroseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

double AgeMs(uint64_t nowUs, uint64_t thenUs) {
    if (thenUs == 0 || nowUs < thenUs) {
        return 0.0;
    }
    return static_cast<double>(nowUs - thenUs) / 1000.0;
}

void UpdateTimingEwma(double& value, bool& hasValue, double sampleMs) {
    constexpr double kAlpha = 0.20;
    if (!hasValue) {
        value = sampleMs;
        hasValue = true;
        return;
    }

    value = (value * (1.0 - kAlpha)) + (sampleMs * kAlpha);
}

void TransitionResource(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    if (commandList == nullptr || resource == nullptr || before == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}

bool CopyPlaneToUploadBuffer(
    ID3D12Device* device,
    ID3D12Resource* uploadBuffer,
    ID3D12Resource* texture,
    const uint8_t* src,
    uint32_t srcPitch,
    uint32_t widthBytes,
    uint32_t height,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT& outFootprint) {
    if (device == nullptr ||
        uploadBuffer == nullptr ||
        texture == nullptr ||
        src == nullptr ||
        srcPitch < widthBytes ||
        widthBytes == 0 ||
        height == 0) {
        return false;
    }

    const D3D12_RESOURCE_DESC textureDesc = texture->GetDesc();
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(
        &textureDesc,
        0,
        1,
        0,
        &outFootprint,
        &numRows,
        &rowSizeInBytes,
        &totalBytes);

    uint8_t* mapped = nullptr;
    const HRESULT mapHr = uploadBuffer->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&mapped));
    if (FAILED(mapHr) || mapped == nullptr) {
        return false;
    }

    uint8_t* dst = mapped + outFootprint.Offset;
    const size_t dstPitch =
        static_cast<size_t>(outFootprint.Footprint.RowPitch);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(
            dst + static_cast<size_t>(y) * dstPitch,
            src + static_cast<size_t>(y) * srcPitch,
            widthBytes);
    }

    uploadBuffer->Unmap(0, nullptr);
    return true;
}

void TransitionSceneDepthIfNeeded(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* depthResource,
    D3D12_RESOURCE_STATES& currentState,
    D3D12_RESOURCE_STATES nextState) {
    if (commandList == nullptr || depthResource == nullptr || currentState == nextState) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = depthResource;
    barrier.Transition.StateBefore = currentState;
    barrier.Transition.StateAfter = nextState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
    currentState = nextState;
}

bool IsSameAuthoredComponent(
    const EffectComponentCommon* source,
    const EffectComponentCommon* destination) {
    if (source == nullptr || destination == nullptr) {
        return false;
    }
    if (source->id != 0 && source->id == destination->id) {
        return true;
    }
    return source->name == destination->name && source->type == destination->type;
}

void ApplyLiveTuningToComponent(
    const ParticleComponentAssetView& source,
    EffectParticleSettings& destination) {
    destination.depthFadeSoftness = source.settings->depthFadeSoftness;
    destination.edgeSoftness = source.settings->edgeSoftness;
}

void ApplyLiveTuningToComponent(
    const TrailComponentAssetView& source,
    EffectTrailSettings& destination) {
    destination.depthFadeSoftness = source.settings->depthFadeSoftness;
    destination.trailTailFade = source.settings->trailTailFade;
}

void ApplyLiveTuningToComponent(
    const DistortionComponentAssetView& source,
    EffectDistortionSettings& destination) {
    destination.depthFadeSoftness = source.settings->depthFadeSoftness;
    destination.depthAttenuation = source.settings->depthAttenuation;
}

void PreserveParticleLiveTuning(
    const EffectAsset& currentAsset,
    EffectAsset& reloadedAsset) {
    std::vector<ParticleComponentAsset> replacements;
    ForEachParticleComponent(reloadedAsset.Components().ParticleStorageView(), [&currentAsset, &replacements](const ParticleComponentAssetView& reloadedParticle) {
        bool applied = false;
        ForEachParticleComponent(currentAsset.Components().ParticleStorageView(), [&applied, &reloadedParticle, &replacements](const ParticleComponentAssetView& currentParticle) {
            if (applied) {
                return;
            }
            if (IsSameAuthoredComponent(currentParticle.common, reloadedParticle.common)) {
                ParticleComponentAsset replacement{*reloadedParticle.common, *reloadedParticle.settings};
                ApplyLiveTuningToComponent(currentParticle, replacement.settings);
                replacements.push_back(replacement);
                applied = true;
            }
        });
    });

    const MutableParticleComponentStorageView storage = reloadedAsset.MutableComponents().MutableParticleStorageView();
    for (const ParticleComponentAsset& replacement : replacements) {
        ReplaceParticleComponentAndSyncPacked(storage, replacement);
    }
}

void PreserveTrailLiveTuning(
    const EffectAsset& currentAsset,
    EffectAsset& reloadedAsset) {
    std::vector<TrailComponentAsset> replacements;
    ForEachTrailComponent(reloadedAsset.Components().TrailStorageView(), [&currentAsset, &replacements](const TrailComponentAssetView& reloadedTrail) {
        bool applied = false;
        ForEachTrailComponent(currentAsset.Components().TrailStorageView(), [&applied, &reloadedTrail, &replacements](const TrailComponentAssetView& currentTrail) {
            if (applied) {
                return;
            }
            if (IsSameAuthoredComponent(currentTrail.common, reloadedTrail.common)) {
                TrailComponentAsset replacement{*reloadedTrail.common, *reloadedTrail.settings};
                ApplyLiveTuningToComponent(currentTrail, replacement.settings);
                replacements.push_back(replacement);
                applied = true;
            }
        });
    });

    const MutableTrailComponentStorageView storage = reloadedAsset.MutableComponents().MutableTrailStorageView();
    for (const TrailComponentAsset& replacement : replacements) {
        ReplaceTrailComponentAndSyncPacked(storage, replacement);
    }
}

void PreserveDistortionLiveTuning(
    const EffectAsset& currentAsset,
    EffectAsset& reloadedAsset) {
    std::vector<DistortionComponentAsset> replacements;
    ForEachDistortionComponent(reloadedAsset.Components().DistortionStorageView(), [&currentAsset, &replacements](const DistortionComponentAssetView& reloadedDistortion) {
        bool applied = false;
        ForEachDistortionComponent(currentAsset.Components().DistortionStorageView(), [&applied, &reloadedDistortion, &replacements](const DistortionComponentAssetView& currentDistortion) {
            if (applied) {
                return;
            }
            if (IsSameAuthoredComponent(currentDistortion.common, reloadedDistortion.common)) {
                DistortionComponentAsset replacement{*reloadedDistortion.common, *reloadedDistortion.settings};
                ApplyLiveTuningToComponent(currentDistortion, replacement.settings);
                replacements.push_back(replacement);
                applied = true;
            }
        });
    });

    const MutableDistortionComponentStorageView storage = reloadedAsset.MutableComponents().MutableDistortionStorageView();
    for (const DistortionComponentAsset& replacement : replacements) {
        ReplaceDistortionComponentAndSyncPacked(storage, replacement);
    }
}

void PreserveLiveTuning(
    const EffectAsset& currentAsset,
    EffectAsset& reloadedAsset) {
    reloadedAsset.defaultParticle = currentAsset.defaultParticle;
    reloadedAsset.defaultTrail = currentAsset.defaultTrail;
    reloadedAsset.defaultBeam = currentAsset.defaultBeam;
    reloadedAsset.defaultDistortion = currentAsset.defaultDistortion;

    PreserveParticleLiveTuning(currentAsset, reloadedAsset);
    PreserveTrailLiveTuning(currentAsset, reloadedAsset);
    PreserveDistortionLiveTuning(currentAsset, reloadedAsset);
}
} // namespace

AppRunLoop::AppRunLoop(
    DebugCamera& debugCamera,
    AppRuntimeState& runtimeState,
    AppSceneResources& scene,
    AppParticleSystem& particleSystem,
    AppImGuiLayer& imguiLayer,
    AppFrameRenderer& frameRenderer,
    AppPipelines& appPipelines,
    AppRenderResources& renderResources,
    graphics::SwapChain& swapChain,
    core::CommandListPool& clPool,
    EngineContext& engineContext,
    ge3::core::DescriptorHeapSet& heaps,
    core::Device& dev,
    ComPtr<ID3D12DescriptorHeap> srvDescriptorHeap,
    Matrix4x4* wvpData,
    uint32_t windowWidth,
    uint32_t windowHeight,
    FrameLoopState& frameState,
    ID3D12CommandQueue* commandQueue,
    ID3D12Fence* fence,
    HANDLE fenceEvent)
    : debugCamera_(debugCamera),
      runtimeState_(runtimeState),
      scene_(scene),
      particleSystem_(particleSystem),
      imguiLayer_(imguiLayer),
      frameRenderer_(frameRenderer),
      appPipelines_(appPipelines),
      renderResources_(renderResources),
      swapChain_(swapChain),
      clPool_(clPool),
      engineContext_(engineContext),
      heaps_(heaps),
      dev_(dev),
      srvDescriptorHeap_(srvDescriptorHeap),
      wvpData_(wvpData),
      windowWidth_(windowWidth),
      windowHeight_(windowHeight),
      frameState_(frameState),
      commandQueue_(commandQueue),
      fence_(fence),
      fenceEvent_(fenceEvent) {
    frameFenceValues_.assign(swapChain_.BufferCount(), 0);
    postProcessStack_.ResetToVfxDefaults();

    EffectAsset additiveParticle{};
    additiveParticle.name = "particle_additive";
    additiveParticle.shader = "Particle";
    additiveParticle.texture = "default";
    additiveParticle.passState.blend = ge3::graphics::BlendMode::Additive;
    additiveParticle.passState.depth = ge3::graphics::DepthMode::ReadOnly;
    additiveParticle.layer = EffectLayer::AdditiveFx;
    additiveParticle.lifetime = 2.0f;
    additiveParticle.defaultParticle.emissive = 1.5f;
    additiveParticle.defaultBeam.emissive = additiveParticle.defaultParticle.emissive;
    effectSystem_.RegisterAsset(std::move(additiveParticle));
    effectRuntime_.AttachSystem(&effectSystem_);

    loadedEffectAssets_ = effectAssetLoader_.LoadDirectory("Resources/effects");
    for (const LoadedEffectAsset& loaded : loadedEffectAssets_) {
        effectSystem_.RegisterAsset(loaded.asset);
    }
}

void AppRunLoop::InitializeBeam(
    ID3D12Device* device,
    ID3D12DescriptorHeap* srvDescriptorHeap,
    uint32_t descriptorSizeSRV,
    DXGI_FORMAT rtvFormat,
    DXGI_FORMAT dsvFormat) {
    beam_.Initialize(
        device,
        srvDescriptorHeap,
        descriptorSizeSRV,
        scene_.textureSrvHandleCPU,
        scene_.textureSrvHandleCPU2,
        rtvFormat,
        dsvFormat);
}

void AppRunLoop::Shutdown() {
    FlushGpu();
    beam_.Shutdown();
}

void AppRunLoop::SetNetworkStatsProvider(std::function<net::NetworkStatsSnapshot()> provider) {
    networkStatsProvider_ = std::move(provider);
}

void AppRunLoop::SetJitterBufferTargetDelaySetter(std::function<void(uint32_t)> setter) {
    jitterBufferTargetDelaySetter_ = std::move(setter);
}

void AppRunLoop::SetJitterBufferAutoModeSetter(std::function<void(bool)> setter) {
    jitterBufferAutoModeSetter_ = std::move(setter);
}

void AppRunLoop::SetNetworkConditionSetter(
    std::function<void(const net::NetworkCondition&)> setter
) {
    networkConditionSetter_ = std::move(setter);
}

void AppRunLoop::SetAdaptiveControlModeSetter(std::function<void(int)> setter) {
    adaptiveControlModeSetter_ = std::move(setter);
}

void AppRunLoop::SetCongestionControlModeSetter(std::function<void(int)> setter) {
    congestionControlModeSetter_ = std::move(setter);
}

void AppRunLoop::SetReceivedFrameProvider(
    std::function<bool(net::DecodedVideoFrame&)> provider) {
    receivedFrameProvider_ = std::move(provider);
}

void AppRunLoop::SetNetworkFrameDisplayNotifier(std::function<void()> notifier) {
    networkFrameDisplayNotifier_ = std::move(notifier);
}

void AppRunLoop::SetReceivedVideoTexture(
    Microsoft::WRL::ComPtr<ID3D12Resource> texture,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadBuffers,
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpuHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle,
    uint32_t width,
    uint32_t height) {
    receivedVideoTexture_ = std::move(texture);
    receivedVideoUploadBuffers_ = std::move(uploadBuffers);
    receivedVideoUploadFenceValues_.assign(receivedVideoUploadBuffers_.size(), 0);
    receivedVideoUploadCursor_ = 0;
    activeReceivedVideoUploadBufferIndex_ = -1;
    receivedVideoUavGpuHandle_ = uavGpuHandle;
    receivedVideoSrvGpuHandle_ = srvGpuHandle;
    receivedVideoWidth_ = width;
    receivedVideoHeight_ = height;
}

void AppRunLoop::SetReceivedVideoNv12Textures(
    Microsoft::WRL::ComPtr<ID3D12Resource> yTexture,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> yUploadBuffers,
    D3D12_GPU_DESCRIPTOR_HANDLE ySrvGpuHandle,
    Microsoft::WRL::ComPtr<ID3D12Resource> uvTexture,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uvUploadBuffers,
    D3D12_GPU_DESCRIPTOR_HANDLE uvSrvGpuHandle) {
    receivedVideoNv12YTexture_ = std::move(yTexture);
    receivedVideoNv12YUploadBuffers_ = std::move(yUploadBuffers);
    receivedVideoNv12YSrvGpuHandle_ = ySrvGpuHandle;
    receivedVideoNv12UVTexture_ = std::move(uvTexture);
    receivedVideoNv12UVUploadBuffers_ = std::move(uvUploadBuffers);
    receivedVideoNv12UVSrvGpuHandle_ = uvSrvGpuHandle;
}

void AppRunLoop::PopulateNetworkRenderTimings(
    net::NetworkStatsSnapshot& stats) const {
    stats.receiveUploadBufferWaitMs = receiveUploadBufferWaitMs_;
    stats.receiveDisplayFrameId = receiveDisplayFrameId_;
    stats.receiveDisplayCameraFrameAgeMs = receiveDisplayCameraFrameAgeMs_;
    stats.receiveDisplayEncoderOutputAgeMs =
        receiveDisplayEncoderOutputAgeMs_;
    stats.receiveDisplayDecodedFrameAgeMs = receiveDisplayDecodedFrameAgeMs_;
    stats.textureUploadMs = textureUploadMs_;
    stats.presentGpuWaitMs = presentGpuWaitMs_;
    stats.renderFramePacingWaitMs = renderFramePacingWaitMs_;
    stats.waitableSwapChainWaitMs = waitableSwapChainWaitMs_;
    stats.presentMs = presentMs_;
    stats.presentSyncInterval =
        runtimeState_.lowLatencyPresentMode ? 0u : 1u;
    stats.lowLatencyPresentMode = runtimeState_.lowLatencyPresentMode;
    stats.waitableSwapChainPacingEnabled =
        runtimeState_.waitableSwapChainPacingEnabled &&
        !runtimeState_.lowLatencyPresentMode &&
        swapChain_.HasFrameLatencyWaitableObject();
    stats.waitableSwapChainAvailable =
        swapChain_.HasFrameLatencyWaitableObject();
    stats.swapChainBufferCount = swapChain_.BufferCount();
}

void AppRunLoop::UpdateFrame() {
    appPipelines_.HotReloadIfNeeded(dev_.GetDevice());
    runtimeState_.viewport.Width = static_cast<float>(windowWidth_);
    runtimeState_.viewport.Height = static_cast<float>(windowHeight_);
    runtimeState_.viewport.TopLeftX = 0.0f;
    runtimeState_.viewport.TopLeftY = 0.0f;
    runtimeState_.viewport.MinDepth = 0.0f;
    runtimeState_.viewport.MaxDepth = 1.0f;
    runtimeState_.scissorRect.left = 0;
    runtimeState_.scissorRect.top = 0;
    runtimeState_.scissorRect.right = static_cast<LONG>(windowWidth_);
    runtimeState_.scissorRect.bottom = static_cast<LONG>(windowHeight_);

    const bool uiToggleKeyDown = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (uiToggleKeyDown && !uiToggleKeyWasDown_) {
        runtimeState_.showImGui = !runtimeState_.showImGui;
    }
    uiToggleKeyWasDown_ = uiToggleKeyDown;

    if (runtimeState_.autoPlayVfxDemo &&
        runtimeState_.enableVfxRenderPasses &&
        !runtimeState_.networkExperimentMode) {
        runtimeState_.enableParticles = true;
        runtimeState_.autoPlayVfxTimer -= 0.016f;
        runtimeState_.autoPlayVfxAngle += 0.9f * 0.016f;
        if (runtimeState_.autoPlayVfxTimer <= 0.0f) {
            const float radius = (std::max)(0.0f, runtimeState_.autoPlayVfxRadius);
            const float angle = runtimeState_.autoPlayVfxAngle;
            const Vector3 effectPosition = {
                std::cos(angle) * radius,
                std::sin(angle * 1.7f) * 0.65f,
                std::sin(angle) * radius
            };
            effectRuntime_.PlayEffectWithParams(
                "warp_core",
                effectPosition,
                {1.0f, 0.8f, 0.45f, 1.0f},
                {1.15f, 1.15f, 1.15f});
            runtimeState_.autoPlayVfxTimer = (std::max)(0.1f, runtimeState_.autoPlayVfxInterval);
        }
    }

    debugCamera_.Update();
    runtimeState_.cameraWorldPosition = debugCamera_.translation_;
    scene_.UpdateCameraWorldPosition(runtimeState_.cameraWorldPosition);
    frameState_.viewMatrix = debugCamera_.GetViewMatrix();
    frameState_.projMatrix = debugCamera_.GetProjectionMatrix();

    beamTime_ += 0.016f;
    beam_.SetTime(beamTime_);
    if (runtimeState_.enableVfxRenderPasses) {
        effectRuntime_.Update(0.016f);
    }

    for (LoadedEffectAsset& loaded : loadedEffectAssets_) {
        if (!std::filesystem::exists(loaded.path)) {
            continue;
        }

        const std::filesystem::file_time_type lastWriteTime =
            std::filesystem::last_write_time(loaded.path);
        if (lastWriteTime == loaded.lastWriteTime) {
            continue;
        }

        LoadedEffectAsset reloaded{};
        if (effectAssetLoader_.LoadFile(loaded.path, reloaded)) {
            if (const EffectAsset* currentAsset = effectSystem_.FindAsset(reloaded.asset.name)) {
                PreserveLiveTuning(*currentAsset, reloaded.asset);
            }
            effectSystem_.RegisterAsset(reloaded.asset);
            loaded = std::move(reloaded);
        }
    }

    BYTE key[256] = {};
    (void)key;

    frameState_.viewProjectionMatrix = Multiply(frameState_.viewMatrix, frameState_.projMatrix);
    frameState_.deltaTime += 0.016f;
    frameState_.drawCount = particleSystem_.UpdateInstances(
        frameState_.viewProjectionMatrix,
        frameState_.deltaTime);
}

void AppRunLoop::BeginFrameSystems() {
    imguiLayer_.BeginFrame();
    frameTransientAllocator_.BeginFrame();
    resourceRegistry_.Clear();
    effectResourceCache_.BeginFrame();
    renderGraph_.Clear();
    renderGraph_.ClearResources();
}

void AppRunLoop::WaitForFrameResource(UINT frameIndex) {
    if (frameIndex >= frameFenceValues_.size() ||
        fence_ == nullptr ||
        fenceEvent_ == nullptr) {
        return;
    }

    const uint64_t fenceValue = frameFenceValues_[frameIndex];
    if (fenceValue == 0 ||
        fence_->GetCompletedValue() >= fenceValue) {
        return;
    }

    fence_->SetEventOnCompletion(fenceValue, fenceEvent_);
    WaitForSingleObject(fenceEvent_, INFINITE);
}

void AppRunLoop::SignalFrameResource(UINT frameIndex) {
    if (frameIndex >= frameFenceValues_.size() ||
        commandQueue_ == nullptr ||
        fence_ == nullptr) {
        return;
    }

    const uint64_t fenceValue = engineContext_.GetFenceValue() + 1;
    engineContext_.SetFenceValue(fenceValue);
    commandQueue_->Signal(fence_, fenceValue);
    frameFenceValues_[frameIndex] = fenceValue;
    if (activeReceivedVideoUploadBufferIndex_ >= 0) {
        const auto uploadIndex =
            static_cast<size_t>(activeReceivedVideoUploadBufferIndex_);
        if (uploadIndex < receivedVideoUploadFenceValues_.size()) {
            receivedVideoUploadFenceValues_[uploadIndex] = fenceValue;
        }
        activeReceivedVideoUploadBufferIndex_ = -1;
    }
}

void AppRunLoop::FlushGpu() {
    if (commandQueue_ == nullptr ||
        fence_ == nullptr ||
        fenceEvent_ == nullptr) {
        return;
    }

    const uint64_t fenceValue = engineContext_.GetFenceValue() + 1;
    engineContext_.SetFenceValue(fenceValue);
    commandQueue_->Signal(fence_, fenceValue);

    if (fence_->GetCompletedValue() < fenceValue) {
        fence_->SetEventOnCompletion(fenceValue, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }

    std::fill(frameFenceValues_.begin(), frameFenceValues_.end(), fenceValue);
    std::fill(
        receivedVideoUploadFenceValues_.begin(),
        receivedVideoUploadFenceValues_.end(),
        fenceValue);
}

void AppRunLoop::WaitForReceivedVideoUploadBuffer(UINT uploadBufferIndex) {
    if (uploadBufferIndex >= receivedVideoUploadFenceValues_.size() ||
        commandQueue_ == nullptr ||
        fence_ == nullptr ||
        fenceEvent_ == nullptr) {
        return;
    }

    const uint64_t fenceValue =
        receivedVideoUploadFenceValues_[uploadBufferIndex];
    if (fenceValue == 0 || fence_->GetCompletedValue() >= fenceValue) {
        return;
    }

    fence_->SetEventOnCompletion(fenceValue, fenceEvent_);
    WaitForSingleObject(fenceEvent_, INFINITE);
}

void AppRunLoop::UploadReceivedVideoFrame(
    ID3D12GraphicsCommandList* commandList,
    UINT frameIndex) {
    if (commandList == nullptr ||
        !receivedFrameProvider_ ||
        !receivedVideoTexture_ ||
        receivedVideoUploadBuffers_.empty() ||
        receivedVideoWidth_ == 0 ||
        receivedVideoHeight_ == 0) {
        return;
    }
    (void)frameIndex;

    net::DecodedVideoFrame frame{};
    if (!receivedFrameProvider_(frame)) {
        return;
    }

    const uint64_t displaySampleUs = NowMicroseconds();
    receiveDisplayFrameId_ = frame.frameId;
    if (frame.cameraCaptureCompletedTimeUs != 0) {
        UpdateTimingEwma(
            receiveDisplayCameraFrameAgeMs_,
            hasReceiveDisplayCameraFrameAgeMs_,
            AgeMs(displaySampleUs, frame.cameraCaptureCompletedTimeUs));
    }
    if (frame.encoderOutputTimeUs != 0) {
        UpdateTimingEwma(
            receiveDisplayEncoderOutputAgeMs_,
            hasReceiveDisplayEncoderOutputAgeMs_,
            AgeMs(displaySampleUs, frame.encoderOutputTimeUs));
    }
    if (frame.decodedTimeUs != 0) {
        UpdateTimingEwma(
            receiveDisplayDecodedFrameAgeMs_,
            hasReceiveDisplayDecodedFrameAgeMs_,
            AgeMs(displaySampleUs, frame.decodedTimeUs));
    }

    if (frame.format == net::DecodedVideoFrameFormat::Nv12) {
        if (!receivedVideoNv12YTexture_ ||
            !receivedVideoNv12UVTexture_ ||
            receivedVideoNv12YUploadBuffers_.empty() ||
            receivedVideoNv12UVUploadBuffers_.empty() ||
            receivedVideoNv12YSrvGpuHandle_.ptr == 0 ||
            receivedVideoNv12UVSrvGpuHandle_.ptr == 0 ||
            receivedVideoUavGpuHandle_.ptr == 0 ||
            frame.width != receivedVideoWidth_ ||
            frame.height != receivedVideoHeight_ ||
            frame.nv12YPitch < frame.width ||
            frame.nv12UVPitch < frame.width ||
            frame.nv12Y.size() <
                static_cast<size_t>(frame.nv12YPitch) * frame.height ||
            frame.nv12UV.size() <
                static_cast<size_t>(frame.nv12UVPitch) * (frame.height / 2u)) {
            static uint32_t invalidNv12FrameLogCount = 0;
            if (invalidNv12FrameLogCount < 10) {
                OutputDebugStringA("[ReceivedVideo] NV12 frame/resources invalid. Skip upload.\n");
                invalidNv12FrameLogCount++;
            }
            return;
        }

        const auto uploadBufferWaitStart = std::chrono::steady_clock::now();
        const UINT uploadBufferIndex =
            receivedVideoUploadCursor_ %
            static_cast<UINT>(receivedVideoUploadBuffers_.size());
        receivedVideoUploadCursor_ =
            (receivedVideoUploadCursor_ + 1u) %
            static_cast<UINT>(receivedVideoUploadBuffers_.size());

        WaitForReceivedVideoUploadBuffer(uploadBufferIndex);
        UpdateTimingEwma(
            receiveUploadBufferWaitMs_,
            hasReceiveUploadBufferWaitMs_,
            ElapsedMs(uploadBufferWaitStart));

        if (uploadBufferIndex >= receivedVideoNv12YUploadBuffers_.size() ||
            uploadBufferIndex >= receivedVideoNv12UVUploadBuffers_.size()) {
            return;
        }

        ID3D12Resource* yUpload =
            receivedVideoNv12YUploadBuffers_[uploadBufferIndex].Get();
        ID3D12Resource* uvUpload =
            receivedVideoNv12UVUploadBuffers_[uploadBufferIndex].Get();
        if (yUpload == nullptr || uvUpload == nullptr) {
            return;
        }

        activeReceivedVideoUploadBufferIndex_ =
            static_cast<int>(uploadBufferIndex);

        const auto textureUploadStart = std::chrono::steady_clock::now();

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT yFootprint{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT uvFootprint{};
        const bool yCopied = CopyPlaneToUploadBuffer(
            dev_.GetDevice(),
            yUpload,
            receivedVideoNv12YTexture_.Get(),
            frame.nv12Y.data(),
            frame.nv12YPitch,
            frame.width,
            frame.height,
            yFootprint);
        const bool uvCopied = CopyPlaneToUploadBuffer(
            dev_.GetDevice(),
            uvUpload,
            receivedVideoNv12UVTexture_.Get(),
            frame.nv12UV.data(),
            frame.nv12UVPitch,
            frame.width,
            frame.height / 2u,
            uvFootprint);
        if (!yCopied || !uvCopied) {
            OutputDebugStringA("[ReceivedVideo] NV12 upload buffer copy failed.\n");
            UpdateTimingEwma(
                textureUploadMs_,
                hasTextureUploadMs_,
                ElapsedMs(textureUploadStart));
            return;
        }

        TransitionResource(
            commandList,
            receivedVideoNv12YTexture_.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
        TransitionResource(
            commandList,
            receivedVideoNv12UVTexture_.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION yDst{};
        yDst.pResource = receivedVideoNv12YTexture_.Get();
        yDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        yDst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION ySrc{};
        ySrc.pResource = yUpload;
        ySrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        ySrc.PlacedFootprint = yFootprint;
        commandList->CopyTextureRegion(&yDst, 0, 0, 0, &ySrc, nullptr);

        D3D12_TEXTURE_COPY_LOCATION uvDst{};
        uvDst.pResource = receivedVideoNv12UVTexture_.Get();
        uvDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        uvDst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION uvSrc{};
        uvSrc.pResource = uvUpload;
        uvSrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        uvSrc.PlacedFootprint = uvFootprint;
        commandList->CopyTextureRegion(&uvDst, 0, 0, 0, &uvSrc, nullptr);

        TransitionResource(
            commandList,
            receivedVideoNv12YTexture_.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            commandList,
            receivedVideoNv12UVTexture_.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            commandList,
            receivedVideoTexture_.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12DescriptorHeap* descriptorHeaps[] = { srvDescriptorHeap_.Get() };
        commandList->SetDescriptorHeaps(1, descriptorHeaps);
        commandList->SetPipelineState(appPipelines_.GetComputePSO());
        commandList->SetComputeRootSignature(appPipelines_.GetComputeRootSignature());
        commandList->SetComputeRootDescriptorTable(
            0,
            receivedVideoNv12YSrvGpuHandle_);
        commandList->SetComputeRootDescriptorTable(
            1,
            receivedVideoUavGpuHandle_);
        commandList->Dispatch(
            (receivedVideoWidth_ + 7u) / 8u,
            (receivedVideoHeight_ + 7u) / 8u,
            1);

        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = receivedVideoTexture_.Get();
        commandList->ResourceBarrier(1, &uavBarrier);

        TransitionResource(
            commandList,
            receivedVideoTexture_.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            commandList,
            receivedVideoNv12YTexture_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(
            commandList,
            receivedVideoNv12UVTexture_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        UpdateTimingEwma(
            textureUploadMs_,
            hasTextureUploadMs_,
            ElapsedMs(textureUploadStart));

        if (networkFrameDisplayNotifier_) {
            networkFrameDisplayNotifier_();
        }
        return;
    }

    if (frame.rgba.empty() || frame.width == 0 || frame.height == 0) {
        return;
    }

    const uint8_t* src = frame.rgba.data();
    const uint32_t srcWidth = frame.width;
    const uint32_t srcHeight = frame.height;
    const size_t srcPayloadBytes = frame.rgba.size();

    const size_t requiredSize =
        static_cast<size_t>(srcWidth) *
        static_cast<size_t>(srcHeight) *
        4u;

    if (srcPayloadBytes < requiredSize) {
        static uint32_t shortFrameLogCount = 0;
        if (shortFrameLogCount < 10) {
            OutputDebugStringA("[ReceivedVideo] Raw frame is too small. Skip upload.\n");
            shortFrameLogCount++;
        }
        return;
    }

    const auto uploadBufferWaitStart = std::chrono::steady_clock::now();
    const UINT uploadBufferIndex =
        receivedVideoUploadCursor_ %
        static_cast<UINT>(receivedVideoUploadBuffers_.size());
    receivedVideoUploadCursor_ =
        (receivedVideoUploadCursor_ + 1u) %
        static_cast<UINT>(receivedVideoUploadBuffers_.size());

    WaitForReceivedVideoUploadBuffer(uploadBufferIndex);
    UpdateTimingEwma(
        receiveUploadBufferWaitMs_,
        hasReceiveUploadBufferWaitMs_,
        ElapsedMs(uploadBufferWaitStart));

    ID3D12Resource* uploadBuffer =
        receivedVideoUploadBuffers_[uploadBufferIndex].Get();
    if (uploadBuffer == nullptr) {
        return;
    }
    activeReceivedVideoUploadBufferIndex_ =
        static_cast<int>(uploadBufferIndex);

    const auto textureUploadStart = std::chrono::steady_clock::now();

    const D3D12_RESOURCE_DESC textureDesc =
        receivedVideoTexture_->GetDesc();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;

    dev_.GetDevice()->GetCopyableFootprints(
        &textureDesc,
        0,
        1,
        0,
        &footprint,
        &numRows,
        &rowSizeInBytes,
        &totalBytes
    );

    uint8_t* mapped = nullptr;
    const HRESULT mapHr = uploadBuffer->Map(
        0,
        nullptr,
        reinterpret_cast<void**>(&mapped)
    );

    if (FAILED(mapHr) || mapped == nullptr) {
        OutputDebugStringA("[ReceivedVideo] UploadBuffer Map failed.\n");
        UpdateTimingEwma(
            textureUploadMs_,
            hasTextureUploadMs_,
            ElapsedMs(textureUploadStart));
        return;
    }

    uint8_t* dst = mapped + footprint.Offset;

    const size_t srcRowPitch =
        static_cast<size_t>(srcWidth) * 4u;

    const size_t dstRowPitch =
        static_cast<size_t>(footprint.Footprint.RowPitch);

    if (srcWidth == receivedVideoWidth_ && srcHeight == receivedVideoHeight_) {
        const size_t copyRowBytes = static_cast<size_t>(receivedVideoWidth_) * 4u;
        for (uint32_t y = 0; y < receivedVideoHeight_; ++y) {
            std::memcpy(
                dst + static_cast<size_t>(y) * dstRowPitch,
                src + static_cast<size_t>(y) * srcRowPitch,
                copyRowBytes);
        }
    }
    else {
        for (uint32_t y = 0; y < receivedVideoHeight_; ++y) {
            const double srcYf =
                receivedVideoHeight_ <= 1
                ? 0.0
                : (static_cast<double>(y) * static_cast<double>(srcHeight - 1u)) /
                    static_cast<double>(receivedVideoHeight_ - 1u);
            const uint32_t y0 =
                std::min<uint32_t>(srcHeight - 1u, static_cast<uint32_t>(srcYf));
            const uint32_t y1 = std::min<uint32_t>(srcHeight - 1u, y0 + 1u);
            const double wy = srcYf - static_cast<double>(y0);

            for (uint32_t x = 0; x < receivedVideoWidth_; ++x) {
                uint8_t* dstPixel =
                    dst +
                    static_cast<size_t>(y) * dstRowPitch +
                    static_cast<size_t>(x) * 4u;

                const double srcXf =
                    receivedVideoWidth_ <= 1
                    ? 0.0
                    : (static_cast<double>(x) * static_cast<double>(srcWidth - 1u)) /
                        static_cast<double>(receivedVideoWidth_ - 1u);
                const uint32_t x0 =
                    std::min<uint32_t>(srcWidth - 1u, static_cast<uint32_t>(srcXf));
                const uint32_t x1 = std::min<uint32_t>(srcWidth - 1u, x0 + 1u);
                const double wx = srcXf - static_cast<double>(x0);

                const uint8_t* p00 =
                    src + static_cast<size_t>(y0) * srcRowPitch +
                    static_cast<size_t>(x0) * 4u;
                const uint8_t* p10 =
                    src + static_cast<size_t>(y0) * srcRowPitch +
                    static_cast<size_t>(x1) * 4u;
                const uint8_t* p01 =
                    src + static_cast<size_t>(y1) * srcRowPitch +
                    static_cast<size_t>(x0) * 4u;
                const uint8_t* p11 =
                    src + static_cast<size_t>(y1) * srcRowPitch +
                    static_cast<size_t>(x1) * 4u;

                for (uint32_t c = 0; c < 4u; ++c) {
                    const double top =
                        static_cast<double>(p00[c]) * (1.0 - wx) +
                        static_cast<double>(p10[c]) * wx;
                    const double bottom =
                        static_cast<double>(p01[c]) * (1.0 - wx) +
                        static_cast<double>(p11[c]) * wx;
                    const double value = top * (1.0 - wy) + bottom * wy;
                    dstPixel[c] =
                        static_cast<uint8_t>(std::clamp(
                            static_cast<int>(std::lround(value)),
                            0,
                            255
                        ));
                }
            }
        }
    }

    uploadBuffer->Unmap(0, nullptr);

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toCopy.Transition.pResource = receivedVideoTexture_.Get();
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION dstLocation{};
    dstLocation.pResource = receivedVideoTexture_.Get();
    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLocation{};
    srcLocation.pResource = uploadBuffer;
    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint = footprint;

    commandList->CopyTextureRegion(
        &dstLocation,
        0,
        0,
        0,
        &srcLocation,
        nullptr
    );

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    toSrv.Transition.pResource = receivedVideoTexture_.Get();
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toSrv);

    UpdateTimingEwma(
        textureUploadMs_,
        hasTextureUploadMs_,
        ElapsedMs(textureUploadStart));

    if (networkFrameDisplayNotifier_) {
        networkFrameDisplayNotifier_();
    }
}

void AppRunLoop::RenderFrame() {
    const bool useWaitableSwapChain =
        runtimeState_.waitableSwapChainPacingEnabled &&
        !runtimeState_.lowLatencyPresentMode &&
        swapChain_.HasFrameLatencyWaitableObject();
    swapChain_.SetMaximumFrameLatency(
        useWaitableSwapChain ? 1u : swapChain_.BufferCount());
    const auto waitableSwapChainStart = std::chrono::steady_clock::now();
    if (useWaitableSwapChain) {
        WaitForSingleObject(
            swapChain_.FrameLatencyWaitableObject(),
            INFINITE);
    }
    UpdateTimingEwma(
        waitableSwapChainWaitMs_,
        hasWaitableSwapChainWaitMs_,
        ElapsedMs(waitableSwapChainStart));

    UINT backBufferIndex = swapChain_.CurrentIndex();
    if (frameFenceValues_.size() != swapChain_.BufferCount()) {
        frameFenceValues_.assign(swapChain_.BufferCount(), 0);
    }

    const auto frameSyncStart = std::chrono::steady_clock::now();
    WaitForFrameResource(backBufferIndex);
    const double renderFramePacingWaitMs = ElapsedMs(frameSyncStart);
    UpdateTimingEwma(
        renderFramePacingWaitMs_,
        hasRenderFramePacingWaitMs_,
        renderFramePacingWaitMs);

    BeginFrameSystems();

    ComPtr<ID3D12GraphicsCommandList> commandList =
        clPool_.Begin(backBufferIndex, appPipelines_.GetMainPSO());
    gpuParticleSystem_.Initialize(
        dev_.GetDevice(),
        commandList.Get(),
        heaps_);

    ID3D12Resource* backBuffer = swapChain_.BackBuffer(backBufferIndex);
    auto dsvHandle = heaps_.dsv.GetHandle(engineContext_.GetMainDsvIndex()).cpu;
    auto readOnlyDsvHandle = heaps_.dsv.GetHandle(engineContext_.GetReadOnlyDsvIndex()).cpu;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapChain_.RTV(backBufferIndex);

    UpdateFrame();

    if (runtimeState_.showReceivedVideoInGame ||
        runtimeState_.showReceivedVideoPreviewWindow) {
        UploadReceivedVideoFrame(commandList.Get(), backBufferIndex);
    }

    scene_.UpdateTransforms(
        runtimeState_,
        wvpData_,
        frameState_.viewMatrix,
        frameState_.projMatrix,
        windowWidth_,
        windowHeight_);

    const PostProcessExecutionPlan postExecutionPlan =
        runtimeState_.enablePostProcessPasses
        ? postProcessStack_.BuildExecutionPlan()
        : PostProcessExecutionPlan{};
    const std::string postPreviewResource =
        runtimeState_.enablePostProcessPasses &&
        !postExecutionPlan.finalOutputResource.empty()
        ? postExecutionPlan.finalOutputResource
        : "SceneColor";

    net::NetworkStatsSnapshot networkStatsSnapshot{};
    const net::NetworkStatsSnapshot* networkStatsPtr = nullptr;
    if (networkStatsProvider_) {
        networkStatsSnapshot = networkStatsProvider_();
        networkStatsPtr = &networkStatsSnapshot;
    }

    imguiLayer_.BuildUi(
        runtimeState_,
        effectRuntime_,
        postProcessStack_,
        lastRenderGraphDescription_,
        lastRenderGraphError_,
        lastRenderPassDebugInfo_,
        lastTransientTargetCount_,
        lastTransientTargetStorageCount_,
        lastTransientBufferCount_,
        lastTransientBufferStorageCount_,
        vfxRenderTargets_.GetSrvHandle("SceneColor"),
        vfxRenderTargets_.GetSrvHandle("VfxAccumulation"),
        vfxRenderTargets_.GetSrvHandle(postPreviewResource),
        vfxRenderTargets_.GetSrvHandle("DebugDepthPreview"),
        vfxRenderTargets_.GetSrvHandle("DebugEmissivePreview"),
        receivedVideoSrvGpuHandle_,
        networkStatsPtr,
        jitterBufferTargetDelaySetter_,
        jitterBufferAutoModeSetter_,
        networkConditionSetter_,
        adaptiveControlModeSetter_,
        congestionControlModeSetter_,
        [&]() {
        Emitter emitterState{};
        emitterState.transform = runtimeState_.emitter.transform;
        emitterState.count = runtimeState_.emitter.count;
        emitterState.frequency = runtimeState_.emitter.frequency;
        emitterState.frequencyTime = runtimeState_.emitter.frequencyTime;
        particleSystem_.Emit(emitterState);
        });
    imguiLayer_.EndFrame();

    scene_.SyncRuntimeState(runtimeState_, frameState_.deltaTime);
    particleSystem_.SetAccelerationField({
        runtimeState_.accelerationField.acceleration,
        {runtimeState_.accelerationField.area.min, runtimeState_.accelerationField.area.max}
    });

    effectResourceCache_.RegisterTexture({"default", scene_.textureSrvHandleCPU, scene_.textureSrvHandleGPU, 1, 1});
    effectResourceCache_.RegisterTexture({"monsterBall", scene_.textureSrvHandleCPU2, scene_.textureSrvHandleGPU2, 1, 1});
    effectResourceCache_.RegisterTexture({"streakNoise", scene_.textureSrvHandleCPU, scene_.textureSrvHandleGPU, 1, 1});

    D3D12_GPU_DESCRIPTOR_HANDLE spriteTextureHandle =
        runtimeState_.useMonsterBall ? scene_.textureSrvHandleGPU2 : scene_.textureSrvHandleGPU;

    // RNVP受信テクスチャが有効なら、ゲーム画面内Spriteに表示する
    if (runtimeState_.showReceivedVideoInGame &&
        receivedVideoSrvGpuHandle_.ptr != 0) {
        spriteTextureHandle = receivedVideoSrvGpuHandle_;
    }
    const EffectRuntimeFrame effectRuntimeFrame = effectRuntime_.BuildFrame();
    const ParticleRenderQueue& particleQueue = effectRuntimeFrame.particleQueue;
    const ParticleRenderFallback primaryParticleFx =
        !particleQueue.empty() ? effectRuntimeFrame.PrimaryParticleFallback()
                               : effectRuntime_.FindPrimaryParticleFallback();
    const D3D12_GPU_DESCRIPTOR_HANDLE vfxTextureHandle =
        primaryParticleFx.common != nullptr
            ? effectResourceCache_.ResolveTexture(primaryParticleFx.common->texture, spriteTextureHandle)
            : spriteTextureHandle;

    AppFrameGraphBuildContext graphContext{};
    graphContext.renderGraph = &renderGraph_;
    graphContext.runtimeState = &runtimeState_;
    graphContext.frameRenderer = &frameRenderer_;
    graphContext.imguiLayer = &imguiLayer_;
    graphContext.appPipelines = &appPipelines_;
    graphContext.renderResources = &renderResources_;
    graphContext.scene = &scene_;
    graphContext.vfxRenderTargets = &vfxRenderTargets_;
    graphContext.gpuParticleSystem = &gpuParticleSystem_;
    graphContext.vfxRenderers = &vfxRenderers_;
    graphContext.postProcessStack = &postProcessStack_;
    graphContext.frameState = &frameState_;
    graphContext.srvDescriptorHeap = srvDescriptorHeap_.Get();
    graphContext.backBuffer = backBuffer;
    graphContext.rtv = rtv;
    graphContext.dsv = dsvHandle;
    graphContext.spriteTextureHandle = spriteTextureHandle;
    graphContext.vfxTextureHandle = vfxTextureHandle;
    graphContext.depthTextureHandle = engineContext_.GetDepthSrvGpuHandle();
    graphContext.receivedTextureHandle = receivedVideoSrvGpuHandle_;
    graphContext.effectRuntime = &effectRuntimeFrame;
    graphContext.primaryParticleFx = primaryParticleFx;
    graphContext.beamTime = beamTime_;
    frameGraphBuilder_.Build(graphContext);
    gpuParticleSystem_.EnsureGraphBuffers(dev_.GetDevice(), renderGraph_);

    const std::vector<ge3::graphics::TransientRenderTargetDesc> transientRenderTargetPlan =
        renderGraph_.BuildTransientRenderTargetPlan();
    const std::vector<ge3::graphics::TransientBufferDesc> transientBufferPlan =
        renderGraph_.BuildTransientBufferPlan();
    std::unordered_set<std::string> transientTargetStorages;
    std::unordered_set<std::string> transientBufferStorages;
    for (const auto& target : transientRenderTargetPlan) {
        if (target.transient) {
            transientTargetStorages.insert(target.storageName);
        }
    }
    for (const auto& buffer : transientBufferPlan) {
        if (buffer.transient) {
            transientBufferStorages.insert(buffer.storageName);
        }
    }

    vfxRenderTargets_.ResetRequests();
    for (const auto& renderTarget : transientRenderTargetPlan) {
        if (renderTarget.transient) {
            vfxRenderTargets_.RequestTransientTarget(
                renderTarget,
                renderTarget.clearColor,
                renderTarget.initialState,
                renderTarget.name.find("PostColor") == 0);
            continue;
        }

        vfxRenderTargets_.RequestTarget(
            renderTarget.name,
            renderTarget.resolutionScale,
            renderTarget.format,
            renderTarget.clearColor,
            renderTarget.initialState);
    }
    vfxRenderTargets_.Initialize(
        dev_.GetDevice(),
        heaps_,
        windowWidth_,
        windowHeight_);

    resourceRegistry_.RegisterRenderTarget({
        "BackBuffer",
        {},
        rtv,
        {},
        DXGI_FORMAT_R8G8B8A8_UNORM,
        windowWidth_,
        windowHeight_
    });
    vfxRenderTargets_.Register(resourceRegistry_);

    TransitionSceneDepthIfNeeded(
        commandList.Get(),
        engineContext_.GetDepthStencil(),
        sceneDepthState_,
        D3D12_RESOURCE_STATE_DEPTH_WRITE);

    frameRenderer_.BeginFrame(
        commandList.Get(),
        backBuffer,
        rtv,
        dsvHandle,
        runtimeState_.clearColor);
    vfxRenderTargets_.BeginScene(commandList.Get(), dsvHandle);
    renderGraph_.RegisterResource("BackBuffer", backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
    renderGraph_.RegisterResource(
        "SceneDepth",
        engineContext_.GetDepthStencil(),
        D3D12_RESOURCE_STATE_DEPTH_WRITE);
    renderGraph_.RegisterRenderTargetBinding("BackBuffer", rtv, windowWidth_, windowHeight_);
    vfxRenderTargets_.RegisterGraphResources(renderGraph_);
    vfxRenderTargets_.RegisterDepthBinding(renderGraph_, dsvHandle);
    renderGraph_.RegisterDepthTargetBinding("SceneDepthReadOnly", readOnlyDsvHandle);
    gpuParticleSystem_.RegisterGraphResources(renderGraph_);
    renderGraph_.SetResourceStateChangedCallback(
        [&](std::string_view name, D3D12_RESOURCE_STATES state) {
            if (name == "SceneDepth") {
                sceneDepthState_ = state;
            }
            vfxRenderTargets_.SetResourceState(name, state);
            gpuParticleSystem_.SetResourceState(name, state);
        });

    std::string renderGraphError;
    if (!renderGraph_.Validate(&renderGraphError)) {
        OutputDebugStringA("[RenderGraph] ");
        OutputDebugStringA(renderGraphError.c_str());
        OutputDebugStringA("\n");
    }
    lastRenderPassDebugInfo_ = renderGraph_.BuildPassDebugInfo();
    lastRenderGraphDescription_ = renderGraph_.Describe();
    lastRenderGraphError_ = renderGraphError;
    lastTransientTargetCount_ = static_cast<uint32_t>(std::count_if(
        transientRenderTargetPlan.begin(),
        transientRenderTargetPlan.end(),
        [](const auto& target) { return target.transient; }));
    lastTransientTargetStorageCount_ = static_cast<uint32_t>(transientTargetStorages.size());
    lastTransientBufferCount_ = static_cast<uint32_t>(std::count_if(
        transientBufferPlan.begin(),
        transientBufferPlan.end(),
        [](const auto& buffer) { return buffer.transient; }));
    lastTransientBufferStorageCount_ = static_cast<uint32_t>(transientBufferStorages.size());
    renderGraph_.Execute(commandList.Get());
    frameRenderer_.EndFrame(commandList.Get(), backBuffer);

    clPool_.EndAndExecute(dev_);
    const auto presentSignalStart = std::chrono::steady_clock::now();
    SignalFrameResource(backBufferIndex);
    const UINT presentSyncInterval =
        runtimeState_.lowLatencyPresentMode ? 0u : 1u;
    swapChain_.Present(dev_, presentSyncInterval);
    const double presentMs = ElapsedMs(presentSignalStart);
    UpdateTimingEwma(
        presentMs_,
        hasPresentMs_,
        presentMs);
    UpdateTimingEwma(
        presentGpuWaitMs_,
        hasPresentGpuWaitMs_,
        renderFramePacingWaitMs + presentMs);
}
