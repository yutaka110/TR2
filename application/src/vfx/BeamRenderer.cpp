#include "vfx/BeamRenderer.h"

#include <vector>
#include <stdexcept>

#include "../../AppFrameState.h"
#include "../../EffectSystem.h"
#include "vfx/VfxRenderInputs.h"
#include "vfx/VfxRenderContext.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;
using ge3::core::ShaderCompiler;

static XMMATRIX ToXmmatrix(const Matrix4x4& matrix)
{
    return XMMatrixSet(
        matrix.m[0][0], matrix.m[0][1], matrix.m[0][2], matrix.m[0][3],
        matrix.m[1][0], matrix.m[1][1], matrix.m[1][2], matrix.m[1][3],
        matrix.m[2][0], matrix.m[2][1], matrix.m[2][2], matrix.m[2][3],
        matrix.m[3][0], matrix.m[3][1], matrix.m[3][2], matrix.m[3][3]);
}

void BeamRenderer::Draw(
    ID3D12GraphicsCommandList* cmdList,
    const BeamRenderInput& input,
    const VfxRenderContext& context)
{
    if (cmdList == nullptr ||
        context.srvDescriptorHeap == nullptr ||
        context.frameState == nullptr ||
        input.primary.componentCommon == nullptr ||
        input.primary.instance == nullptr ||
        input.settings == nullptr) {
        return;
    }

    ID3D12DescriptorHeap* descriptorHeaps[] = { context.srvDescriptorHeap };
    cmdList->SetDescriptorHeaps(1, descriptorHeaps);

    const EffectComponentCommon& component = *input.primary.componentCommon;
    const EffectInstance& instance = *input.primary.instance;
    XMMATRIX world = XMMatrixScaling(
        instance.transform.scale.x * component.size.x,
        instance.transform.scale.y * component.size.y,
        instance.transform.scale.z * component.size.z);
    world = XMMatrixMultiply(world, XMMatrixTranslation(
        instance.transform.translate.x,
        instance.transform.translate.y,
        instance.transform.translate.z));
    const XMMATRIX wvp =
        world *
        ToXmmatrix(context.frameState->viewMatrix) *
        ToXmmatrix(context.frameState->projMatrix);
    const XMFLOAT4 color = {
        instance.color.x * component.color.x,
        instance.color.y * component.color.y,
        instance.color.z * component.color.z,
        instance.color.w * component.color.w,
    };
    DrawTest(cmdList, wvp, color, input.settings->emissive);
}

// 邁｡譏薙・繝ｫ繝・
static void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr)) { throw std::runtime_error("D3D12 error"); }
}

// 鬆らせ繝輔か繝ｼ繝槭ャ繝・
struct BeamVertex
{
    XMFLOAT3 pos;
    XMFLOAT2 uv;
};

BeamRenderer::~BeamRenderer()
{
    Shutdown();
}

void BeamRenderer::Initialize(
    ID3D12Device* device,
    ID3D12DescriptorHeap* srvHeap,
    UINT srvDescriptorSize,
    D3D12_CPU_DESCRIPTOR_HANDLE rampCpuHandle,
    D3D12_CPU_DESCRIPTOR_HANDLE noiseCpuHandle,
    DXGI_FORMAT rtvFormat,
    DXGI_FORMAT dsvFormat
)
{
    //==============================
    // 1) RootSignature 菴懈・
    //==============================

    D3D12_ROOT_PARAMETER rootParams[3] = {};

    // [0] VS CBV (b0)
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootParams[0].Descriptor.ShaderRegister = 0; // b0

    // [1] PS CBV (b1)
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[1].Descriptor.ShaderRegister = 1; // b1

    // [2] SRV 繝・・繝悶Ν (t0縲徼3縺上ｉ縺・∪縺ｧ諠ｳ螳・
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 4;   // t0・柎3 縺ｾ縺ｧ遒ｺ菫晢ｼ井ｻ翫・ t0,t1 菴ｿ逕ｨ・・
    srvRange.BaseShaderRegister = 0;   // t0
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &srvRange;

    // Static Sampler (s0)
    D3D12_STATIC_SAMPLER_DESC staticSamp{};
    staticSamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamp.ShaderRegister = 0; // s0
    staticSamp.RegisterSpace = 0;
    staticSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = _countof(rootParams);
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &staticSamp;
    rsDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob;
    ComPtr<ID3DBlob> rsError;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &rsBlob, &rsError));

    ThrowIfFailed(device->CreateRootSignature(
        0,
        rsBlob->GetBufferPointer(),
        rsBlob->GetBufferSize(),
        IID_PPV_ARGS(&rootSig_)));

    //==============================
    // 2) 繧ｷ繧ｧ繝ｼ繝隱ｭ縺ｿ霎ｼ縺ｿ / 繧ｳ繝ｳ繝代う繝ｫ
    //==============================

    ComPtr<IDxcBlob> vsBlob;
    ComPtr<IDxcBlob> psBlob;

    if (!shaderCompiler_.Initialize()) {
        throw std::runtime_error("ShaderCompiler initialization failed");
    }

    // 萓具ｼ售haderCompiler 縺・DXC 繧剃ｽｿ縺｣縺ｦ縺・ｋ蜑肴署
     vsBlob = shaderCompiler_.CompileFromFile(
        L"Resources/Beam3D.VS.hlsl",
        L"VSMain",          // 笘・繧ｨ繝ｳ繝医Μ繝昴う繝ｳ繝・
        L"vs_6_0");         // or "vs_5_1"

    psBlob = shaderCompiler_.CompileFromFile(
        L"Resources/Beam3D.PS.hlsl",
        L"PSMain",          // 笘・繧ｨ繝ｳ繝医Μ繝昴う繝ｳ繝・
        L"ps_6_0");         // or "ps_5_1"


    //==============================
    // 3) 蜈･蜉帙Ξ繧､繧｢繧ｦ繝・
    //==============================
    D3D12_INPUT_ELEMENT_DESC inputElements[] = {
        // POSITION : float3
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
          D3D12_APPEND_ALIGNED_ELEMENT,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

          // TEXCOORD : float2
          { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,
            D3D12_APPEND_ALIGNED_ELEMENT,
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    //==============================
    // 4) 繝悶Ξ繝ｳ繝・繝ｩ繧ｹ繧ｿ/豺ｱ蠎ｦ繧ｹ繝・Φ繧ｷ繝ｫ
    //==============================

    // 蜉邂励ヶ繝ｬ繝ｳ繝・(SrcAlpha, One)
    D3D12_BLEND_DESC blendDesc{};
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    auto& rt0 = blendDesc.RenderTarget[0];
    rt0.BlendEnable = TRUE;
    rt0.SrcBlend = D3D12_BLEND_ONE;
    rt0.DestBlend = D3D12_BLEND_ONE;
    rt0.BlendOp = D3D12_BLEND_OP_ADD;
    rt0.SrcBlendAlpha = D3D12_BLEND_ONE;
    rt0.DestBlendAlpha = D3D12_BLEND_ZERO;
    rt0.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt0.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // 繝ｩ繧ｹ繧ｿ繝ｩ繧､繧ｶ
    D3D12_RASTERIZER_DESC rastDesc{};
    rastDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rastDesc.CullMode = D3D12_CULL_MODE_NONE;
    rastDesc.FrontCounterClockwise = FALSE;
    rastDesc.DepthClipEnable = TRUE;

    // 豺ｱ蠎ｦ繧ｹ繝・Φ繧ｷ繝ｫ
    D3D12_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // 豺ｱ蠎ｦ縺ｯ隱ｭ繧縺縺・
    depthDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    depthDesc.StencilEnable = FALSE;

    //==============================
    // 5) PSO 菴懈・
    //==============================

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSig_.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.BlendState = blendDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.RasterizerState = rastDesc;
    psoDesc.DepthStencilState = depthDesc;
    psoDesc.InputLayout = { inputElements, _countof(inputElements) };
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = rtvFormat;
    psoDesc.DSVFormat = dsvFormat;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(device->CreateGraphicsPipelineState(
        &psoDesc, IID_PPV_ARGS(&pso_)));

    //==============================
    // 6) 譚ｿ繝昴Μ (VB/IB) 菴懈・
    //==============================
    // 繝ｭ繝ｼ繧ｫ繝ｫ遨ｺ髢薙〒 Y+ 譁ｹ蜷代↓莨ｸ縺ｳ繧・1x1 縺ｮ譚ｿ
    // y: 0竊・ 縺後ン繝ｼ繝縺ｮ髟ｷ縺墓婿蜷代』: -0.5竊・0.5 縺悟､ｪ縺墓婿蜷・
    BeamVertex vertices[] = {
        { XMFLOAT3(-0.5f, 0.0f, 0.0f), XMFLOAT2(0.0f, 0.0f) }, // 0: 蟾ｦ荳・
        { XMFLOAT3(0.5f, 0.0f, 0.0f), XMFLOAT2(1.0f, 0.0f) }, // 1: 蜿ｳ荳・
        { XMFLOAT3(0.5f, 1.0f, 0.0f), XMFLOAT2(1.0f, 1.0f) }, // 2: 蜿ｳ荳・
        { XMFLOAT3(-0.5f, 1.0f, 0.0f), XMFLOAT2(0.0f, 1.0f) }, // 3: 蟾ｦ荳・
    };
    uint16_t indices[] = {
        0, 1, 2,
        0, 2, 3
    };
    indexCount_ = _countof(indices);

    // ---- VB ----
    {
        const UINT vbSize = sizeof(vertices);

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Alignment = 0;
        bufDesc.Width = vbSize;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.SampleDesc.Quality = 0;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&vb_)));

        void* mapped = nullptr;
        D3D12_RANGE range{};
        range.Begin = 0;
        range.End = 0;
        ThrowIfFailed(vb_->Map(0, &range, &mapped));
        memcpy(mapped, vertices, vbSize);
        vb_->Unmap(0, nullptr);

        vbView_.BufferLocation = vb_->GetGPUVirtualAddress();
        vbView_.SizeInBytes = vbSize;
        vbView_.StrideInBytes = sizeof(BeamVertex);
    }

    // ---- IB ----
    {
        const UINT ibSize = sizeof(indices);

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Alignment = 0;
        bufDesc.Width = ibSize;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.SampleDesc.Quality = 0;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&ib_)));

        void* mapped = nullptr;
        D3D12_RANGE range{};
        range.Begin = 0;
        range.End = 0;
        ThrowIfFailed(ib_->Map(0, &range, &mapped));
        memcpy(mapped, indices, ibSize);
        ib_->Unmap(0, nullptr);

        ibView_.BufferLocation = ib_->GetGPUVirtualAddress();
        ibView_.SizeInBytes = ibSize;
        ibView_.Format = DXGI_FORMAT_R16_UINT;
    }

    ////==============================
    //// 7) 螳壽焚繝舌ャ繝輔ぃ (VS/PS) 菴懈・ & Map
    ////==============================

   // ---- VS CBV ----
    {
        const UINT cbSize = (sizeof(BeamVSConstants) + 255) & ~255u; // 256繝舌う繝医い繝ｩ繧､繝ｳ

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Alignment = 0;
        bufDesc.Width = cbSize;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.SampleDesc.Quality = 0;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&vsCb_)));

        D3D12_RANGE range{};
        range.Begin = 0;
        range.End = 0;
        ThrowIfFailed(vsCb_->Map(0, &range, reinterpret_cast<void**>(&vsCbMapped_)));
    }

    // ---- PS CBV ----
    {
        const UINT cbSize = (sizeof(BeamPSConstants) + 255) & ~255u;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufDesc{};
        bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufDesc.Alignment = 0;
        bufDesc.Width = cbSize;
        bufDesc.Height = 1;
        bufDesc.DepthOrArraySize = 1;
        bufDesc.MipLevels = 1;
        bufDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufDesc.SampleDesc.Count = 1;
        bufDesc.SampleDesc.Quality = 0;
        bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&psCb_)));

        D3D12_RANGE range{};
        range.Begin = 0;
        range.End = 0;
        ThrowIfFailed(psCb_->Map(0, &range, reinterpret_cast<void**>(&psCbMapped_)));
    }

    //==============================
    // 8) SRV繝・・繝悶Ν (t0=ramp, t1=noise) 繧堤｢ｺ菫・& 繧ｳ繝斐・
    //==============================

    // 笘・繝偵・繝励・ 3逡ｪ繝ｻ4逡ｪ繧偵ン繝ｼ繝蟆ら畑縺ｫ菴ｿ縺・ｼ・:ImGui, 1:繝・け繧ｹ繝√Ε1, 2:繝・け繧ｹ繝√Ε2・・
    const UINT baseIndex = 1;

    D3D12_GPU_DESCRIPTOR_HANDLE heapGpuStart = srvHeap->GetGPUDescriptorHandleForHeapStart();

    // t0 逕ｨ (baseIndex)

    // t1 逕ｨ (baseIndex+1)

    // 繧ｳ繝斐・
    (void)device;
    (void)rampCpuHandle;
    (void)noiseCpuHandle;

    // GPU 蛛ｴ繧ょ酔縺倥う繝ｳ繝・ャ繧ｯ繧ｹ縺ｫ蜷医ｏ縺帙ｋ
    srvTableGpuStart_ = heapGpuStart;
    srvTableGpuStart_.ptr += static_cast<SIZE_T>(srvDescriptorSize) * baseIndex;
}

void BeamRenderer::SetTime(float t)
{
    if (psCbMapped_) {
        psCbMapped_->time = t;
    }
}

void BeamRenderer::Shutdown()
{
    if (vsCb_ && vsCbMapped_) {
        vsCb_->Unmap(0, nullptr);
    }
    if (psCb_ && psCbMapped_) {
        psCb_->Unmap(0, nullptr);
    }

    vsCbMapped_ = nullptr;
    psCbMapped_ = nullptr;
    vsCb_.Reset();
    psCb_.Reset();
    vb_.Reset();
    ib_.Reset();
    pso_.Reset();
    rootSig_.Reset();

    vbView_ = {};
    ibView_ = {};
    indexCount_ = 0;
    srvTableGpuStart_ = {};
}

//------------------------------------------------------
// 繝・せ繝育畑 1譛ｬ謠冗判
//------------------------------------------------------
void BeamRenderer::DrawTest(
    ID3D12GraphicsCommandList* cmdList,
    const XMMATRIX& worldViewProj,
    const XMFLOAT4& baseColor,
    float intensity
)
{
    // RootSignature / PSO
    cmdList->SetGraphicsRootSignature(rootSig_.Get());
    cmdList->SetPipelineState(pso_.Get());

    // IA 險ｭ螳・
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &vbView_);
    cmdList->IASetIndexBuffer(&ibView_);

    // CB 縺ｮ荳ｭ霄ｫ繧呈嶌縺肴鋤縺・
    XMStoreFloat4x4(&vsCbMapped_->worldViewProj, XMMatrixTranspose(worldViewProj));
    psCbMapped_->baseColor = baseColor;
    psCbMapped_->intensity = intensity;
    //psCbMapped_->time = 0.0f; // STEP2 縺ｧ邨碁℃譎る俣繧貞・繧後ｋ

    // CBV 繧ｻ繝・ヨ (b0, b1)
    cmdList->SetGraphicsRootConstantBufferView(
        0, vsCb_->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootConstantBufferView(
        1, psCb_->GetGPUVirtualAddress());

    // SRV繝・・繝悶Ν (t0,t1)
    cmdList->SetGraphicsRootDescriptorTable(
        2, srvTableGpuStart_);

    // 謠冗判
    cmdList->DrawIndexedInstanced(indexCount_, 1, 0, 0, 0);
}
