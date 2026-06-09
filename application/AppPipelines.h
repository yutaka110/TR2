#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxcapi.h>
#include <filesystem>
#include <unordered_map>
#include <wrl/client.h>

#include "core/ShaderCompiler.h"

// AppMain.cpp から RootSignature / PSO / ShaderCompile を切り出す。
// 対象: Object3D, ReceivedVideoNv12(CS), Particle。
class AppPipelines {
public:
    bool Initialize(ID3D12Device* device);
    bool HotReloadIfNeeded(ID3D12Device* device);

    ID3D12RootSignature* GetMainRootSignature() const { return mainRootSignature_.Get(); }
    ID3D12RootSignature* GetSpriteRootSignature() const { return spriteRootSignature_.Get(); }
    ID3D12RootSignature* GetParticleRootSignature() const { return particleRootSignature_.Get(); }
    ID3D12RootSignature* GetComputeRootSignature() const { return computeRootSignature_.Get(); }

    ID3D12PipelineState* GetMainPSO() const { return mainPso_.Get(); }
    ID3D12PipelineState* GetMainOpaquePSO() const { return mainOpaquePso_.Get(); }
    ID3D12PipelineState* GetMainAlphaPSO() const { return mainAlphaPso_.Get(); }
    ID3D12PipelineState* GetSpritePSO() const { return spritePso_.Get(); }

    ID3D12PipelineState* GetComputePSO() const { return computePso_.Get(); }
    ID3D12RootSignature* GetGpuParticleComputeRootSignature() const { return gpuParticleComputeRootSignature_.Get(); }
    ID3D12PipelineState* GetGpuParticleComputePSO() const { return gpuParticleComputePso_.Get(); }
    ID3D12RootSignature* GetCompositeRootSignature() const { return compositeRootSignature_.Get(); }
    ID3D12PipelineState* GetCompositePSO() const { return compositePso_.Get(); }
    ID3D12PipelineState* GetBloomExtractPSO() const { return bloomExtractPso_.Get(); }
    ID3D12PipelineState* GetBloomDownsamplePSO() const { return bloomDownsamplePso_.Get(); }
    ID3D12PipelineState* GetBloomUpsamplePSO() const { return bloomUpsamplePso_.Get(); }
    ID3D12PipelineState* GetBlurHorizontalPSO() const { return blurHorizontalPso_.Get(); }
    ID3D12PipelineState* GetBlurVerticalPSO() const { return blurVerticalPso_.Get(); }
    ID3D12PipelineState* GetDistortionCompositePSO() const { return distortionCompositePso_.Get(); }
    ID3D12PipelineState* GetToneMappingPSO() const { return toneMappingPso_.Get(); }
    ID3D12PipelineState* GetGlowCompositePSO() const { return glowCompositePso_.Get(); }
    ID3D12PipelineState* GetDebugDepthPreviewPSO() const { return debugDepthPreviewPso_.Get(); }
    ID3D12PipelineState* GetDebugEmissivePreviewPSO() const { return debugEmissivePreviewPso_.Get(); }

    ID3D12PipelineState* GetParticlePSO() const { return particlePso_.Get(); }
    ID3D12PipelineState* GetParticleOpaquePSO() const { return particleOpaquePso_.Get(); }
    ID3D12PipelineState* GetParticleAlphaPSO() const { return particleAlphaPso_.Get(); }
    ID3D12PipelineState* GetTrailMeshPSO() const { return trailMeshPso_.Get(); }
    ID3D12PipelineState* GetDistortionSpritePSO() const { return distortionSpritePso_.Get(); }

private:
    // RootSignature
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mainRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> spriteRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> particleRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> gpuParticleComputeRootSignature_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> compositeRootSignature_;

    // PSO
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mainPso_;      // 元の graphicsPipelineState
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mainOpaquePso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mainAlphaPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> spritePso_;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> computePso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> gpuParticleComputePso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> compositePso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> bloomExtractPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> bloomDownsamplePso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> bloomUpsamplePso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> blurHorizontalPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> blurVerticalPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> distortionCompositePso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> toneMappingPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> glowCompositePso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> debugDepthPreviewPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> debugEmissivePreviewPso_;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> particlePso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> particleOpaquePso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> particleAlphaPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> trailMeshPso_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> distortionSpritePso_;

    // Shader blobs are kept alive while PSO uses them
    ge3::core::ShaderCompiler shaderCompiler_;
    Microsoft::WRL::ComPtr<IDxcBlob> vs_;
    Microsoft::WRL::ComPtr<IDxcBlob> ps_;
    Microsoft::WRL::ComPtr<IDxcBlob> spriteVs_;
    Microsoft::WRL::ComPtr<IDxcBlob> spritePs_;
    Microsoft::WRL::ComPtr<IDxcBlob> cs_;
    Microsoft::WRL::ComPtr<IDxcBlob> particleVs_;
    Microsoft::WRL::ComPtr<IDxcBlob> particlePs_;
    Microsoft::WRL::ComPtr<IDxcBlob> trailMeshVs_;
    Microsoft::WRL::ComPtr<IDxcBlob> trailMeshPs_;
    Microsoft::WRL::ComPtr<IDxcBlob> distortionSpriteVs_;
    Microsoft::WRL::ComPtr<IDxcBlob> distortionSpritePs_;
    Microsoft::WRL::ComPtr<IDxcBlob> gpuParticleCs_;
    Microsoft::WRL::ComPtr<IDxcBlob> compositeVs_;
    Microsoft::WRL::ComPtr<IDxcBlob> compositePs_;
    Microsoft::WRL::ComPtr<IDxcBlob> bloomExtractPs_;
    Microsoft::WRL::ComPtr<IDxcBlob> bloomDownsamplePs_;
    Microsoft::WRL::ComPtr<IDxcBlob> bloomUpsamplePs_;
    Microsoft::WRL::ComPtr<IDxcBlob> blurHorizontalPs_;
    Microsoft::WRL::ComPtr<IDxcBlob> blurVerticalPs_;
    Microsoft::WRL::ComPtr<IDxcBlob> distortionCompositePs_;
    Microsoft::WRL::ComPtr<IDxcBlob> toneMappingPs_;
    Microsoft::WRL::ComPtr<IDxcBlob> glowCompositePs_;
    Microsoft::WRL::ComPtr<IDxcBlob> debugDepthPreviewPs_;
    Microsoft::WRL::ComPtr<IDxcBlob> debugEmissivePreviewPs_;
    std::unordered_map<std::wstring, std::filesystem::file_time_type> shaderWriteTimes_;

    Microsoft::WRL::ComPtr<IDxcBlob> Compile_(const std::wstring& filePath, const wchar_t* profile);
    void TrackShader_(const std::wstring& filePath);
    bool ShaderChanged_(const std::wstring& filePath) const;
};
