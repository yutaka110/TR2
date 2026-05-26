#include "AppPipelines.h"

#include <cassert>
#include <cstdio>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace {

std::filesystem::path GetModuleDirectory() {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(modulePath).parent_path();
}

std::filesystem::path ResolveShaderPath(const std::wstring& filePath) {
    const std::filesystem::path requested(filePath);
    if (std::filesystem::exists(requested)) {
        return requested;
    }

    const std::filesystem::path bases[] = {
        std::filesystem::current_path(),
        GetModuleDirectory(),
    };

    for (const auto& base : bases) {
        std::filesystem::path probe = base;
        for (int depth = 0; depth < 6; ++depth) {
            const std::filesystem::path direct = probe / requested;
            if (std::filesystem::exists(direct)) {
                return direct;
            }

            const std::filesystem::path projectRelative = probe / L"project" / requested;
            if (std::filesystem::exists(projectRelative)) {
                return projectRelative;
            }

            if (!probe.has_parent_path()) {
                break;
            }
            probe = probe.parent_path();
        }
    }

    return requested;
}

bool FailHr(const char* stage, HRESULT hr) {
    char message[256]{};
    std::snprintf(message, sizeof(message), "[AppPipelines] %s failed. HRESULT=0x%08X\n",
                  stage, static_cast<unsigned int>(hr));
    OutputDebugStringA(message);
    return false;
}

} // namespace

ComPtr<IDxcBlob> AppPipelines::Compile_(const std::wstring& filePath, const wchar_t* profile) {
    if (!shaderCompiler_.Initialize()) {
        OutputDebugStringA("[Error] ShaderCompiler.Initialize failed\n");
        return nullptr;
    }

    const std::wstring entryPoint = L"main";
    const std::filesystem::path resolvedPath = ResolveShaderPath(filePath);
    if (!std::filesystem::exists(resolvedPath)) {
        OutputDebugStringW(L"[AppPipelines] Shader file not found: ");
        OutputDebugStringW(filePath.c_str());
        OutputDebugStringW(L"\n");
    }

    auto blob = shaderCompiler_.CompileFromFile(resolvedPath.wstring(), entryPoint, profile);
    if (!blob) {
        OutputDebugStringW(L"[AppPipelines] Shader compile failed: ");
        OutputDebugStringW(resolvedPath.wstring().c_str());
        OutputDebugStringW(L"\n");
        return nullptr;
    }
    return blob;
}

void AppPipelines::TrackShader_(const std::wstring& filePath) {
    const std::filesystem::path resolvedPath = ResolveShaderPath(filePath);
    if (std::filesystem::exists(resolvedPath)) {
        shaderWriteTimes_[filePath] = std::filesystem::last_write_time(resolvedPath);
    }
}

bool AppPipelines::ShaderChanged_(const std::wstring& filePath) const {
    const std::filesystem::path resolvedPath = ResolveShaderPath(filePath);
    if (!std::filesystem::exists(resolvedPath)) {
        return false;
    }

    const auto found = shaderWriteTimes_.find(filePath);
    if (found == shaderWriteTimes_.end()) {
        return true;
    }
    return std::filesystem::last_write_time(resolvedPath) != found->second;
}

bool AppPipelines::HotReloadIfNeeded(ID3D12Device* device) {
    if (device == nullptr) {
        return false;
    }

    const std::wstring shaders[] = {
        L"resources/Object3D.VS.hlsl",
        L"resources/Object3D.PS.hlsl",
        L"resources/Sprite.VS.hlsl",
        L"resources/Sprite.PS.hlsl",
        L"resources/MotionDetect.CS.hlsl",
        L"resources/Particle.VS.hlsl",
        L"resources/Particle.PS.hlsl",
        L"resources/TrailMesh.VS.hlsl",
        L"resources/TrailMesh.PS.hlsl",
        L"resources/DistortionSprite.VS.hlsl",
        L"resources/DistortionSprite.PS.hlsl",
        L"resources/ParticleSim.CS.hlsl",
        L"resources/FullscreenComposite.VS.hlsl",
        L"resources/FullscreenComposite.PS.hlsl",
        L"resources/BloomExtract.PS.hlsl",
        L"resources/BloomDownsample.PS.hlsl",
        L"resources/BloomUpsample.PS.hlsl",
        L"resources/BlurHorizontal.PS.hlsl",
        L"resources/BlurVertical.PS.hlsl",
        L"resources/DistortionComposite.PS.hlsl",
        L"resources/ToneMapping.PS.hlsl",
        L"resources/GlowComposite.PS.hlsl",
        L"resources/DebugDepthPreview.PS.hlsl",
        L"resources/DebugEmissivePreview.PS.hlsl",
    };

    for (const std::wstring& shader : shaders) {
        if (ShaderChanged_(shader)) {
            OutputDebugStringA("[AppPipelines] Shader change detected. Rebuilding pipelines.\n");
            return Initialize(device);
        }
    }
    return true;
}

bool AppPipelines::Initialize(ID3D12Device* device) {
    if (!device) return false;

    HRESULT hr = S_OK;

    // ------------------------------
    // Main RootSignature (Object3D)
    // ------------------------------
    D3D12_ROOT_SIGNATURE_DESC descriptionRootSignature{};

    D3D12_STATIC_SAMPLER_DESC staticSamplers[1] = {};
    staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    descriptionRootSignature.pStaticSamplers = staticSamplers;
    descriptionRootSignature.NumStaticSamplers = _countof(staticSamplers);

    descriptionRootSignature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    D3D12_DESCRIPTOR_RANGE descriptorRange[1] = {};
    descriptorRange[0].BaseShaderRegister = 0;
    descriptorRange[0].NumDescriptors = 1;
    descriptorRange[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange[0].OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE receivedRange = {};
    receivedRange.BaseShaderRegister = 4;
    receivedRange.NumDescriptors = 1;
    receivedRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    receivedRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE motionMaskRange = {};
    motionMaskRange.BaseShaderRegister = 2;
    motionMaskRange.NumDescriptors = 1;
    motionMaskRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    motionMaskRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParameters[9] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[0].Descriptor.ShaderRegister = 0;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootParameters[1].Descriptor.ShaderRegister = 0;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[2].DescriptorTable.pDescriptorRanges = descriptorRange;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = _countof(descriptorRange);

    rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[3].Descriptor.ShaderRegister = 1;

    rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[4].DescriptorTable.pDescriptorRanges = &receivedRange;

    rootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[5].DescriptorTable.pDescriptorRanges = &motionMaskRange;

    rootParameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[6].Descriptor.ShaderRegister = 2;

    rootParameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[7].Descriptor.ShaderRegister = 3;

    rootParameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[8].Descriptor.ShaderRegister = 4;

    descriptionRootSignature.pParameters = rootParameters;
    descriptionRootSignature.NumParameters = _countof(rootParameters);

    ComPtr<ID3DBlob> signatureBlob;
    ComPtr<ID3DBlob> errorBlob;
    hr = D3D12SerializeRootSignature(&descriptionRootSignature, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &signatureBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                    IID_PPV_ARGS(&mainRootSignature_));
    if (FAILED(hr)) return FailHr("CreateRootSignature(Main)", hr);

    // ------------------------------
    // Sprite RootSignature
    // ------------------------------
    D3D12_DESCRIPTOR_RANGE spriteTextureRange{};
    spriteTextureRange.BaseShaderRegister = 0;
    spriteTextureRange.NumDescriptors = 1;
    spriteTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    spriteTextureRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER spriteRootParams[3] = {};
    spriteRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    spriteRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    spriteRootParams[0].Descriptor.ShaderRegister = 0;

    spriteRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    spriteRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    spriteRootParams[1].Descriptor.ShaderRegister = 0;

    spriteRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    spriteRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    spriteRootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    spriteRootParams[2].DescriptorTable.pDescriptorRanges = &spriteTextureRange;

    D3D12_ROOT_SIGNATURE_DESC spriteRsDesc{};
    spriteRsDesc.NumParameters = _countof(spriteRootParams);
    spriteRsDesc.pParameters = spriteRootParams;
    spriteRsDesc.NumStaticSamplers = _countof(staticSamplers);
    spriteRsDesc.pStaticSamplers = staticSamplers;
    spriteRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> spriteSigBlob;
    ComPtr<ID3DBlob> spriteErrBlob;
    hr = D3D12SerializeRootSignature(&spriteRsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &spriteSigBlob, &spriteErrBlob);
    if (FAILED(hr)) {
        if (spriteErrBlob) {
            OutputDebugStringA(reinterpret_cast<const char*>(spriteErrBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = device->CreateRootSignature(0, spriteSigBlob->GetBufferPointer(), spriteSigBlob->GetBufferSize(),
                                    IID_PPV_ARGS(&spriteRootSignature_));
    if (FAILED(hr)) return FailHr("CreateRootSignature(Sprite)", hr);

    // ------------------------------
    // Particle RootSignature
    // ------------------------------
    D3D12_DESCRIPTOR_RANGE particleInstancingRange{};
    particleInstancingRange.BaseShaderRegister = 0;
    particleInstancingRange.NumDescriptors = 1;
    particleInstancingRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    particleInstancingRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE particleTextureRange{};
    particleTextureRange.BaseShaderRegister = 0;
    particleTextureRange.NumDescriptors = 1;
    particleTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    particleTextureRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE particleDepthRange{};
    particleDepthRange.BaseShaderRegister = 1;
    particleDepthRange.NumDescriptors = 1;
    particleDepthRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    particleDepthRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER particleRootParams[5] = {};
    particleRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    particleRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    particleRootParams[0].Descriptor.ShaderRegister = 0;

    particleRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    particleRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    particleRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    particleRootParams[1].DescriptorTable.pDescriptorRanges = &particleInstancingRange;

    particleRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    particleRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    particleRootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    particleRootParams[2].DescriptorTable.pDescriptorRanges = &particleTextureRange;

    particleRootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    particleRootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    particleRootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    particleRootParams[3].DescriptorTable.pDescriptorRanges = &particleDepthRange;

    particleRootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    particleRootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    particleRootParams[4].Constants.ShaderRegister = 0;
    particleRootParams[4].Constants.Num32BitValues = 4;

    D3D12_ROOT_SIGNATURE_DESC particleRsDesc{};
    particleRsDesc.NumParameters = _countof(particleRootParams);
    particleRsDesc.pParameters = particleRootParams;
    particleRsDesc.NumStaticSamplers = _countof(staticSamplers);
    particleRsDesc.pStaticSamplers = staticSamplers;
    particleRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> particleSigBlob;
    ComPtr<ID3DBlob> particleErrBlob;
    hr = D3D12SerializeRootSignature(&particleRsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &particleSigBlob, &particleErrBlob);
    if (FAILED(hr)) {
        if (particleErrBlob) {
            OutputDebugStringA(reinterpret_cast<const char*>(particleErrBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = device->CreateRootSignature(0, particleSigBlob->GetBufferPointer(), particleSigBlob->GetBufferSize(),
                                    IID_PPV_ARGS(&particleRootSignature_));
    if (FAILED(hr)) return FailHr("CreateRootSignature(Particle)", hr);

    // ------------------------------
    // Compute RootSignature (MotionDetect)
    // ------------------------------
    // t4: Y, t5: UV, u0: output
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[0].BaseShaderRegister = 4;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;

    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER csParams[2] = {};
    csParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    csParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    csParams[0].DescriptorTable.NumDescriptorRanges = 1;
    csParams[0].DescriptorTable.pDescriptorRanges = &ranges[0];

    csParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    csParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    csParams[1].DescriptorTable.NumDescriptorRanges = 1;
    csParams[1].DescriptorTable.pDescriptorRanges = &ranges[1];

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
    rootSigDesc.NumParameters = _countof(csParams);
    rootSigDesc.pParameters = csParams;
    rootSigDesc.NumStaticSamplers = 0;
    rootSigDesc.pStaticSamplers = nullptr;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> csSigBlob;
    ComPtr<ID3DBlob> csErrBlob;
    hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &csSigBlob, &csErrBlob);
    if (FAILED(hr)) {
        if (csErrBlob) {
            OutputDebugStringA(reinterpret_cast<const char*>(csErrBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = device->CreateRootSignature(0, csSigBlob->GetBufferPointer(), csSigBlob->GetBufferSize(),
                                    IID_PPV_ARGS(&computeRootSignature_));
    if (FAILED(hr)) return FailHr("CreateRootSignature(Compute)", hr);

    // ------------------------------
    // GPU Particle Compute RootSignature
    // b0: simulation constants, u0: render particle output, u1: simulation state
    // ------------------------------
    D3D12_DESCRIPTOR_RANGE particleOutputUavRange{};
    particleOutputUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    particleOutputUavRange.NumDescriptors = 1;
    particleOutputUavRange.BaseShaderRegister = 0;
    particleOutputUavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE particleStateUavRange{};
    particleStateUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    particleStateUavRange.NumDescriptors = 1;
    particleStateUavRange.BaseShaderRegister = 1;
    particleStateUavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER gpuParticleParams[3] = {};
    gpuParticleParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    gpuParticleParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    gpuParticleParams[0].Constants.ShaderRegister = 0;
    gpuParticleParams[0].Constants.Num32BitValues = 32;

    gpuParticleParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    gpuParticleParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    gpuParticleParams[1].DescriptorTable.NumDescriptorRanges = 1;
    gpuParticleParams[1].DescriptorTable.pDescriptorRanges = &particleOutputUavRange;

    gpuParticleParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    gpuParticleParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    gpuParticleParams[2].DescriptorTable.NumDescriptorRanges = 1;
    gpuParticleParams[2].DescriptorTable.pDescriptorRanges = &particleStateUavRange;

    D3D12_ROOT_SIGNATURE_DESC gpuParticleRootDesc{};
    gpuParticleRootDesc.NumParameters = _countof(gpuParticleParams);
    gpuParticleRootDesc.pParameters = gpuParticleParams;
    gpuParticleRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> gpuParticleSigBlob;
    ComPtr<ID3DBlob> gpuParticleErrBlob;
    hr = D3D12SerializeRootSignature(
        &gpuParticleRootDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &gpuParticleSigBlob,
        &gpuParticleErrBlob);
    if (FAILED(hr)) {
        if (gpuParticleErrBlob) {
            OutputDebugStringA(reinterpret_cast<const char*>(gpuParticleErrBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = device->CreateRootSignature(
        0,
        gpuParticleSigBlob->GetBufferPointer(),
        gpuParticleSigBlob->GetBufferSize(),
        IID_PPV_ARGS(&gpuParticleComputeRootSignature_));
    if (FAILED(hr)) return FailHr("CreateRootSignature(GpuParticleCompute)", hr);

    // ------------------------------
    // Full-screen Composite RootSignature
    // t0: SceneColor, t1: VfxAccumulation, t2: PostColor
    // ------------------------------
    D3D12_DESCRIPTOR_RANGE compositeRanges[3] = {};
    for (uint32_t i = 0; i < _countof(compositeRanges); ++i) {
        compositeRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        compositeRanges[i].NumDescriptors = 1;
        compositeRanges[i].BaseShaderRegister = i;
        compositeRanges[i].OffsetInDescriptorsFromTableStart = 0;
    }

    D3D12_ROOT_PARAMETER compositeParams[4] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        compositeParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        compositeParams[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        compositeParams[i].DescriptorTable.NumDescriptorRanges = 1;
        compositeParams[i].DescriptorTable.pDescriptorRanges = &compositeRanges[i];
    }
    compositeParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    compositeParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    compositeParams[3].Constants.ShaderRegister = 0;
    compositeParams[3].Constants.Num32BitValues = 8;

    D3D12_ROOT_SIGNATURE_DESC compositeRootDesc{};
    compositeRootDesc.NumParameters = _countof(compositeParams);
    compositeRootDesc.pParameters = compositeParams;
    compositeRootDesc.NumStaticSamplers = _countof(staticSamplers);
    compositeRootDesc.pStaticSamplers = staticSamplers;
    compositeRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> compositeSigBlob;
    ComPtr<ID3DBlob> compositeErrBlob;
    hr = D3D12SerializeRootSignature(
        &compositeRootDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &compositeSigBlob,
        &compositeErrBlob);
    if (FAILED(hr)) {
        if (compositeErrBlob) {
            OutputDebugStringA(reinterpret_cast<const char*>(compositeErrBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = device->CreateRootSignature(
        0,
        compositeSigBlob->GetBufferPointer(),
        compositeSigBlob->GetBufferSize(),
        IID_PPV_ARGS(&compositeRootSignature_));
    if (FAILED(hr)) return FailHr("CreateRootSignature(Composite)", hr);

    // ------------------------------
    // Compile shaders
    // ------------------------------
    vs_ = Compile_(L"resources/Object3D.VS.hlsl", L"vs_6_0");
    ps_ = Compile_(L"resources/Object3D.PS.hlsl", L"ps_6_0");
    spriteVs_ = Compile_(L"resources/Sprite.VS.hlsl", L"vs_6_0");
    spritePs_ = Compile_(L"resources/Sprite.PS.hlsl", L"ps_6_0");
    cs_ = Compile_(L"resources/MotionDetect.CS.hlsl", L"cs_6_0");
    particleVs_ = Compile_(L"resources/Particle.VS.hlsl", L"vs_6_0");
    particlePs_ = Compile_(L"resources/Particle.PS.hlsl", L"ps_6_0");
    trailMeshVs_ = Compile_(L"resources/TrailMesh.VS.hlsl", L"vs_6_0");
    trailMeshPs_ = Compile_(L"resources/TrailMesh.PS.hlsl", L"ps_6_0");
    distortionSpriteVs_ = Compile_(L"resources/DistortionSprite.VS.hlsl", L"vs_6_0");
    distortionSpritePs_ = Compile_(L"resources/DistortionSprite.PS.hlsl", L"ps_6_0");
    gpuParticleCs_ = Compile_(L"resources/ParticleSim.CS.hlsl", L"cs_6_0");
    compositeVs_ = Compile_(L"resources/FullscreenComposite.VS.hlsl", L"vs_6_0");
    compositePs_ = Compile_(L"resources/FullscreenComposite.PS.hlsl", L"ps_6_0");
    bloomExtractPs_ = Compile_(L"resources/BloomExtract.PS.hlsl", L"ps_6_0");
    bloomDownsamplePs_ = Compile_(L"resources/BloomDownsample.PS.hlsl", L"ps_6_0");
    bloomUpsamplePs_ = Compile_(L"resources/BloomUpsample.PS.hlsl", L"ps_6_0");
    blurHorizontalPs_ = Compile_(L"resources/BlurHorizontal.PS.hlsl", L"ps_6_0");
    blurVerticalPs_ = Compile_(L"resources/BlurVertical.PS.hlsl", L"ps_6_0");
    distortionCompositePs_ = Compile_(L"resources/DistortionComposite.PS.hlsl", L"ps_6_0");
    toneMappingPs_ = Compile_(L"resources/ToneMapping.PS.hlsl", L"ps_6_0");
    glowCompositePs_ = Compile_(L"resources/GlowComposite.PS.hlsl", L"ps_6_0");
    debugDepthPreviewPs_ = Compile_(L"resources/DebugDepthPreview.PS.hlsl", L"ps_6_0");
    debugEmissivePreviewPs_ = Compile_(L"resources/DebugEmissivePreview.PS.hlsl", L"ps_6_0");

    if (!vs_ || !ps_ || !spriteVs_ || !spritePs_ || !cs_ || !particleVs_ || !particlePs_ ||
        !trailMeshVs_ || !trailMeshPs_ || !distortionSpriteVs_ || !distortionSpritePs_ ||
        !gpuParticleCs_ || !compositeVs_ || !compositePs_ || !bloomExtractPs_ ||
        !bloomDownsamplePs_ || !bloomUpsamplePs_ || !blurHorizontalPs_ || !blurVerticalPs_ || !distortionCompositePs_ ||
        !toneMappingPs_ || !glowCompositePs_ || !debugDepthPreviewPs_ || !debugEmissivePreviewPs_) {
        OutputDebugStringA("[AppPipelines] Shader compilation failed.\n");
        return false;
    }

    // ------------------------------
    // InputLayout
    // ------------------------------
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[3] = {};
    inputElementDescs[0].SemanticName = "POSITION";
    inputElementDescs[0].SemanticIndex = 0;
    inputElementDescs[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    inputElementDescs[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    inputElementDescs[1].SemanticName = "TEXCOORD";
    inputElementDescs[1].SemanticIndex = 0;
    inputElementDescs[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    inputElementDescs[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    inputElementDescs[2].SemanticName = "NORMAL";
    inputElementDescs[2].SemanticIndex = 0;
    inputElementDescs[2].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    inputElementDescs[2].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    D3D12_INPUT_LAYOUT_DESC inputLayoutDesc{};
    inputLayoutDesc.pInputElementDescs = inputElementDescs;
    inputLayoutDesc.NumElements = _countof(inputElementDescs);

    // BlendState
    D3D12_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Rasterizer
    D3D12_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;

    // Depth
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // ------------------------------
    // Main PSO (graphicsPipelineState)
    // ------------------------------
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphicsPipelineStateDesc{};
    graphicsPipelineStateDesc.pRootSignature = mainRootSignature_.Get();
    graphicsPipelineStateDesc.InputLayout = inputLayoutDesc;
    graphicsPipelineStateDesc.VS = { vs_->GetBufferPointer(), vs_->GetBufferSize() };
    graphicsPipelineStateDesc.PS = { ps_->GetBufferPointer(), ps_->GetBufferSize() };
    graphicsPipelineStateDesc.BlendState = blendDesc;
    graphicsPipelineStateDesc.RasterizerState = rasterizerDesc;
    graphicsPipelineStateDesc.DepthStencilState = depthStencilDesc;
    graphicsPipelineStateDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    graphicsPipelineStateDesc.NumRenderTargets = 1;
    graphicsPipelineStateDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    graphicsPipelineStateDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphicsPipelineStateDesc.SampleDesc.Count = 1;
    graphicsPipelineStateDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

    hr = device->CreateGraphicsPipelineState(&graphicsPipelineStateDesc, IID_PPV_ARGS(&mainPso_));
    if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(Main)", hr);

    // ------------------------------
    // Sprite PSO
    // ------------------------------
    D3D12_INPUT_ELEMENT_DESC spriteElements[2] = {};
    spriteElements[0].SemanticName = "POSITION";
    spriteElements[0].SemanticIndex = 0;
    spriteElements[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    spriteElements[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    spriteElements[1].SemanticName = "TEXCOORD";
    spriteElements[1].SemanticIndex = 0;
    spriteElements[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    spriteElements[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    D3D12_INPUT_LAYOUT_DESC spriteInputLayout{};
    spriteInputLayout.pInputElementDescs = spriteElements;
    spriteInputLayout.NumElements = _countof(spriteElements);

    D3D12_BLEND_DESC spriteBlend{};
    spriteBlend.RenderTarget[0].BlendEnable = TRUE;
    spriteBlend.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    spriteBlend.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    spriteBlend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    spriteBlend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    spriteBlend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    spriteBlend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    spriteBlend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_DEPTH_STENCIL_DESC spriteDepth{};
    spriteDepth.DepthEnable = FALSE;
    spriteDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    spriteDepth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC spriteDesc{};
    spriteDesc.pRootSignature = spriteRootSignature_.Get();
    spriteDesc.InputLayout = spriteInputLayout;
    spriteDesc.VS = { spriteVs_->GetBufferPointer(), spriteVs_->GetBufferSize() };
    spriteDesc.PS = { spritePs_->GetBufferPointer(), spritePs_->GetBufferSize() };
    spriteDesc.BlendState = spriteBlend;
    spriteDesc.RasterizerState = rasterizerDesc;
    spriteDesc.DepthStencilState = spriteDepth;
    spriteDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    spriteDesc.NumRenderTargets = 1;
    spriteDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    spriteDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    spriteDesc.SampleDesc.Count = 1;
    spriteDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

    hr = device->CreateGraphicsPipelineState(&spriteDesc, IID_PPV_ARGS(&spritePso_));
    if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(Sprite)", hr);

    // ------------------------------
    // Compute PSO
    // ------------------------------
    D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc{};
    computePsoDesc.pRootSignature = computeRootSignature_.Get();
    computePsoDesc.CS = { cs_->GetBufferPointer(), cs_->GetBufferSize() };
    hr = device->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&computePso_));
    if (FAILED(hr)) return FailHr("CreateComputePipelineState(MotionDetect)", hr);

    D3D12_COMPUTE_PIPELINE_STATE_DESC gpuParticleComputeDesc{};
    gpuParticleComputeDesc.pRootSignature = gpuParticleComputeRootSignature_.Get();
    gpuParticleComputeDesc.CS = { gpuParticleCs_->GetBufferPointer(), gpuParticleCs_->GetBufferSize() };
    hr = device->CreateComputePipelineState(&gpuParticleComputeDesc, IID_PPV_ARGS(&gpuParticleComputePso_));
    if (FAILED(hr)) return FailHr("CreateComputePipelineState(GpuParticle)", hr);

    D3D12_DEPTH_STENCIL_DESC compositeDepth{};
    compositeDepth.DepthEnable = FALSE;

    D3D12_RASTERIZER_DESC compositeRaster{};
    compositeRaster.CullMode = D3D12_CULL_MODE_NONE;
    compositeRaster.FillMode = D3D12_FILL_MODE_SOLID;

    D3D12_BLEND_DESC compositeBlend{};
    compositeBlend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC compositeDesc{};
    compositeDesc.pRootSignature = compositeRootSignature_.Get();
    compositeDesc.VS = { compositeVs_->GetBufferPointer(), compositeVs_->GetBufferSize() };
    compositeDesc.PS = { compositePs_->GetBufferPointer(), compositePs_->GetBufferSize() };
    compositeDesc.BlendState = compositeBlend;
    compositeDesc.RasterizerState = compositeRaster;
    compositeDesc.DepthStencilState = compositeDepth;
    compositeDesc.NumRenderTargets = 1;
    compositeDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    compositeDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    compositeDesc.SampleDesc.Count = 1;
    compositeDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    hr = device->CreateGraphicsPipelineState(&compositeDesc, IID_PPV_ARGS(&compositePso_));
    if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(Composite)", hr);

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = compositeDesc;
        d.PS = { bloomExtractPs_->GetBufferPointer(), bloomExtractPs_->GetBufferSize() };
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&bloomExtractPso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(BloomExtract)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = compositeDesc;
        d.PS = { bloomDownsamplePs_->GetBufferPointer(), bloomDownsamplePs_->GetBufferSize() };
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&bloomDownsamplePso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(BloomDownsample)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = compositeDesc;
        d.PS = { bloomUpsamplePs_->GetBufferPointer(), bloomUpsamplePs_->GetBufferSize() };
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&bloomUpsamplePso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(BloomUpsample)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = compositeDesc;
        d.PS = { blurHorizontalPs_->GetBufferPointer(), blurHorizontalPs_->GetBufferSize() };
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&blurHorizontalPso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(BlurHorizontal)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = compositeDesc;
        d.PS = { blurVerticalPs_->GetBufferPointer(), blurVerticalPs_->GetBufferSize() };
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&blurVerticalPso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(BlurVertical)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = compositeDesc;
        d.PS = { distortionCompositePs_->GetBufferPointer(), distortionCompositePs_->GetBufferSize() };
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&distortionCompositePso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(DistortionComposite)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = compositeDesc;
        d.PS = { toneMappingPs_->GetBufferPointer(), toneMappingPs_->GetBufferSize() };
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&toneMappingPso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(ToneMapping)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = compositeDesc;
        d.PS = { glowCompositePs_->GetBufferPointer(), glowCompositePs_->GetBufferSize() };
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&glowCompositePso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(GlowComposite)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = compositeDesc;
        d.PS = { debugDepthPreviewPs_->GetBufferPointer(), debugDepthPreviewPs_->GetBufferSize() };
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&debugDepthPreviewPso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(DebugDepthPreview)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = compositeDesc;
        d.PS = { debugEmissivePreviewPs_->GetBufferPointer(), debugEmissivePreviewPs_->GetBufferSize() };
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&debugEmissivePreviewPso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(DebugEmissivePreview)", hr);
    }

    // ------------------------------
    // Particle PSOs
    // ------------------------------
    auto MakeParticleOpaqueBlend = []() {
        D3D12_BLEND_DESC d{};
        d.AlphaToCoverageEnable = FALSE;
        d.IndependentBlendEnable = FALSE;
        auto& rt = d.RenderTarget[0];
        rt.BlendEnable = FALSE;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        return d;
    };

    auto MakeParticleAlphaBlend = []() {
        D3D12_BLEND_DESC d{};
        d.AlphaToCoverageEnable = FALSE;
        d.IndependentBlendEnable = FALSE;
        auto& rt = d.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        return d;
    };

    // Particle quad uses VertexData: position(float4), texcoord(float2), normal(float3).
    D3D12_INPUT_ELEMENT_DESC particleElements[3] = {};
    particleElements[0].SemanticName = "POSITION";
    particleElements[0].SemanticIndex = 0;
    particleElements[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    particleElements[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    particleElements[1].SemanticName = "TEXCOORD";
    particleElements[1].SemanticIndex = 0;
    particleElements[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    particleElements[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    particleElements[2].SemanticName = "NORMAL";
    particleElements[2].SemanticIndex = 0;
    particleElements[2].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    particleElements[2].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;

    D3D12_INPUT_LAYOUT_DESC particleInputLayout{};
    particleInputLayout.pInputElementDescs = particleElements;
    particleInputLayout.NumElements = _countof(particleElements);

    // Particle rasterizer: no cull is common for billboard
    D3D12_RASTERIZER_DESC particleRaster{};
    particleRaster.CullMode = D3D12_CULL_MODE_NONE;
    particleRaster.FillMode = D3D12_FILL_MODE_SOLID;

    // Particle depth: enable but no write for transparent
    D3D12_DEPTH_STENCIL_DESC particleDepth{};
    particleDepth.DepthEnable = TRUE;
    particleDepth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    particleDepth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // Base particle desc
    D3D12_GRAPHICS_PIPELINE_STATE_DESC particleDesc{};
    particleDesc.pRootSignature = particleRootSignature_.Get();
    particleDesc.InputLayout = particleInputLayout;
    particleDesc.VS = { particleVs_->GetBufferPointer(), particleVs_->GetBufferSize() };
    particleDesc.PS = { particlePs_->GetBufferPointer(), particlePs_->GetBufferSize() };
    particleDesc.RasterizerState = particleRaster;
    particleDesc.DepthStencilState = particleDepth;
    particleDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    particleDesc.NumRenderTargets = 1;
    particleDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    particleDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    particleDesc.SampleDesc.Count = 1;
    particleDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

    // Legacy: particlePso_ (equivalent to particlePipelineState)
    particleDesc.BlendState = MakeParticleAlphaBlend();
    hr = device->CreateGraphicsPipelineState(&particleDesc, IID_PPV_ARGS(&particlePso_));
    if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(Particle)", hr);

    // Opaque
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = particleDesc;
        d.BlendState = MakeParticleOpaqueBlend();
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&particleOpaquePso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(ParticleOpaque)", hr);
    }

    // Alpha
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = particleDesc;
        d.BlendState = MakeParticleAlphaBlend();
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&particleAlphaPso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(ParticleAlpha)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = particleDesc;
        d.VS = { trailMeshVs_->GetBufferPointer(), trailMeshVs_->GetBufferSize() };
        d.PS = { trailMeshPs_->GetBufferPointer(), trailMeshPs_->GetBufferSize() };
        d.BlendState = MakeParticleAlphaBlend();
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&trailMeshPso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(TrailMesh)", hr);
    }

    {
        D3D12_BLEND_DESC distortionBlend{};
        distortionBlend.AlphaToCoverageEnable = FALSE;
        distortionBlend.IndependentBlendEnable = FALSE;
        auto& rt = distortionBlend.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = particleDesc;
        d.VS = { distortionSpriteVs_->GetBufferPointer(), distortionSpriteVs_->GetBufferSize() };
        d.PS = { distortionSpritePs_->GetBufferPointer(), distortionSpritePs_->GetBufferSize() };
        d.BlendState = distortionBlend;
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&distortionSpritePso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(DistortionSprite)", hr);
    }

    // ------------------------------
    // Main Opaque/Alpha variants (for sprite or UI etc)
    // These were in AppMain as psoOpaque/psoAlpha using mainRootSignature.
    // We create them as variants of main PSO by only changing BlendState.
    // ------------------------------
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = graphicsPipelineStateDesc;
        auto MakeOpaqueBlend = []() {
            D3D12_BLEND_DESC bd{};
            bd.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            return bd;
        };
        d.BlendState = MakeOpaqueBlend();
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&mainOpaquePso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(MainOpaque)", hr);
    }

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = graphicsPipelineStateDesc;
        D3D12_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.BlendState = bd;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        hr = device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&mainAlphaPso_));
        if (FAILED(hr)) return FailHr("CreateGraphicsPipelineState(MainAlpha)", hr);
    }

    const std::wstring shaders[] = {
        L"resources/Object3D.VS.hlsl",
        L"resources/Object3D.PS.hlsl",
        L"resources/Sprite.VS.hlsl",
        L"resources/Sprite.PS.hlsl",
        L"resources/MotionDetect.CS.hlsl",
        L"resources/Particle.VS.hlsl",
        L"resources/Particle.PS.hlsl",
        L"resources/TrailMesh.VS.hlsl",
        L"resources/TrailMesh.PS.hlsl",
        L"resources/DistortionSprite.VS.hlsl",
        L"resources/DistortionSprite.PS.hlsl",
        L"resources/ParticleSim.CS.hlsl",
        L"resources/FullscreenComposite.VS.hlsl",
        L"resources/FullscreenComposite.PS.hlsl",
        L"resources/BloomExtract.PS.hlsl",
        L"resources/BloomDownsample.PS.hlsl",
        L"resources/BloomUpsample.PS.hlsl",
        L"resources/BlurHorizontal.PS.hlsl",
        L"resources/BlurVertical.PS.hlsl",
        L"resources/DistortionComposite.PS.hlsl",
        L"resources/ToneMapping.PS.hlsl",
        L"resources/GlowComposite.PS.hlsl",
        L"resources/DebugDepthPreview.PS.hlsl",
        L"resources/DebugEmissivePreview.PS.hlsl",
    };
    shaderWriteTimes_.clear();
    for (const std::wstring& shader : shaders) {
        TrackShader_(shader);
    }

    return true;
}
