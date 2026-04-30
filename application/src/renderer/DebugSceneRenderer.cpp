#include "renderer/DebugSceneRenderer.h"

#include <cassert>
#include <cmath>

using Microsoft::WRL::ComPtr;

bool DebugSceneRenderer::Initialize(
    ID3D12Device* device,
    ID3D12DescriptorHeap* srvHeap,
    UINT srvDescriptorSize,
    IDxcUtils* dxcUtils,
    IDxcCompiler* dxcCompiler,
    IDxcIncludeHandler* includeHandler
) {
    device_ = device;
    srvHeap_ = srvHeap;
    srvDescSize_ = srvDescriptorSize;

    // main.cpp 側のクライアントサイズに合わせるなら外から渡すのがベストだが、
    // ここでは一旦固定値 or 後で Setter を用意してもOK。
    clientWidth_ = 1280;
    clientHeight_ = 720;

    assert(device_);
    assert(srvHeap_);

    if (!CreateRootSignatureAndPSO()) {
        return false;
    }
    if (!CreateSpriteGeometry()) {
        return false;
    }
    if (!CreateConstantBuffers()) {
        return false;
    }
    if (!LoadTextureUVChecker()) {
        return false;
    }

    // カメラ初期化（main.cpp の DebugCamera セットアップと同等に）
    debugCamera_.Initialize();

    // ワールド行列の初期値
    worldTransform_.scale = { 1.0f, 1.0f, 1.0f };
    worldTransform_.rotate = { 0.0f, 0.0f, 0.0f };
    worldTransform_.translate = { 0.0f, 0.0f, 0.0f };

    uvTransformSprite_.scale = { 1.0f, 1.0f, 1.0f };
    uvTransformSprite_.rotate = { 0.0f, 0.0f, 0.0f };
    uvTransformSprite_.translate = { 0.0f, 0.0f, 0.0f };

    // DirectionalLight 初期値（main.cpp で directionalLightData に入れていたものと同じ）
    directionalLightInit_.color = { 1.0f, 1.0f, 1.0f, 1.0f };
    Vector3 dir = { 0.0f, -1.0f, 0.0f };
    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    dir.x /= len; dir.y /= len; dir.z /= len;
    directionalLightInit_.direction = dir;
    directionalLightInit_.intensity = 1.0f;

    *directionalLightData_ = directionalLightInit_;

    // WVP 初期値（単位行列）
    *wvpData_ = MakeIdentity4x4();

    return true;
}

bool DebugSceneRenderer::CreateRootSignatureAndPSO(
) {
    HRESULT hr = S_OK;

    // ============================
    // 1. ShaderCompiler 初期化
    // ============================
    if (!shaderCompiler_.Initialize()) {
        return false;
    }

    // ============================
    // 2. RootSignature 設定
    // ============================
    D3D12_ROOT_SIGNATURE_DESC descriptionRootSignature{};

    // ---- Static Sampler (s0) ----
    D3D12_STATIC_SAMPLER_DESC staticSamplers[1] = {};
    staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister = 0; // s0
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    descriptionRootSignature.pStaticSamplers = staticSamplers;
    descriptionRootSignature.NumStaticSamplers = _countof(staticSamplers);

    // IA からの頂点レイアウトを許可
    descriptionRootSignature.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    // ---- SRV Descriptor Range(t0) : 通常オブジェクト用テクスチャ ----
    D3D12_DESCRIPTOR_RANGE descriptorRange[1] = {};
    descriptorRange[0].BaseShaderRegister = 0; // t0
    descriptorRange[0].NumDescriptors = 1;
    descriptorRange[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptorRange[0].OffsetInDescriptorsFromTableStart = 0;

    // ---- SRV Range(t4) : 受信用テクスチャ ----
    D3D12_DESCRIPTOR_RANGE receivedRange = {};
    receivedRange.BaseShaderRegister = 4; // t4
    receivedRange.NumDescriptors = 1;
    receivedRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    receivedRange.OffsetInDescriptorsFromTableStart = 0;

    // ---- SRV Range(t2) : motionMaskTex 用 ----
    D3D12_DESCRIPTOR_RANGE motionMaskRange = {};
    motionMaskRange.BaseShaderRegister = 2; // t2
    motionMaskRange.NumDescriptors = 1;
    motionMaskRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    motionMaskRange.OffsetInDescriptorsFromTableStart = 0;

    // ---- Root Parameters ----
    D3D12_ROOT_PARAMETER rootParameters[6] = {};

    // [0] PS: Material 用 CBV (b0)
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[0].Descriptor.ShaderRegister = 0; // b0

    // [1] VS: Transform(WVP) 用 CBV (b0)
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootParameters[1].Descriptor.ShaderRegister = 0; // b0

    // [2] PS: 通常テクスチャ用 SRV テーブル (t0)
    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[2].DescriptorTable.pDescriptorRanges = descriptorRange;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = _countof(descriptorRange);

    // [3] PS: 平行光源用 CBV (b1)
    rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[3].Descriptor.ShaderRegister = 1; // b1

    // [4] PS: 受信用テクスチャ SRV テーブル (t4)
    rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[4].DescriptorTable.pDescriptorRanges = &receivedRange;

    // [5] PS: motionMaskTex SRV テーブル (t2)
    rootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[5].DescriptorTable.pDescriptorRanges = &motionMaskRange;

    descriptionRootSignature.pParameters = rootParameters;
    descriptionRootSignature.NumParameters = _countof(rootParameters);

    // ============================
    // 3. 各種定数バッファを作成
    // ============================
    // Material (b0, PS)
    materialResourceSprite_ = CreateBufferResource(device_, sizeof(Material));
    {
        Material* materialData = nullptr;
        materialResourceSprite_->Map(0, nullptr, reinterpret_cast<void**>(&materialData));
        // デフォルト値
        materialData->color = { 1.0f, 1.0f, 1.0f, 1.0f };
        materialData->enableLighting = 1;
        materialData->uvTransform = MakeIdentity4x4();
        // メンバに保持
        // （ヘッダ側の型が Material* なら）
        // materialDataSprite_ = materialData;
    }

    // WVP (b0, VS)
    wvpResource_ = CreateBufferResource(device_, sizeof(Matrix4x4));
    wvpResource_->Map(0, nullptr, reinterpret_cast<void**>(&wvpData_));
    *wvpData_ = MakeIdentity4x4();

    // DirectionalLight (b1, PS)
    directionalLightResource_ = CreateBufferResource(device_, sizeof(DirectionalLight));
    directionalLightResource_->Map(0, nullptr, reinterpret_cast<void**>(&directionalLightData_));
    directionalLightData_->color = { 1.0f, 1.0f, 1.0f, 1.0f };
    directionalLightData_->direction = { 0.0f, -1.0f, 0.0f };
    directionalLightData_->intensity = 1.0f;

    // ============================
    // 4. RootSignature 生成
    // ============================
    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

    hr = D3D12SerializeRootSignature(
        &descriptionRootSignature,
        D3D_ROOT_SIGNATURE_VERSION_1,
        signatureBlob.GetAddressOf(),
        errorBlob.GetAddressOf());
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        }
        return false;
    }

    hr = device_->CreateRootSignature(
        0,
        signatureBlob->GetBufferPointer(),
        signatureBlob->GetBufferSize(),
        IID_PPV_ARGS(rootSignature_.GetAddressOf()));
    if (FAILED(hr)) {
        return false;
    }

    // ============================
    // 5. シェーダコンパイル (Object3D)
    // ============================
    auto vsBlob = shaderCompiler_.CompileFromFile(
        L"Object3D.VS.hlsl",
        L"main",
        L"vs_6_0"
    );
    if (!vsBlob) {
        return false;
    }

    auto psBlob = shaderCompiler_.CompileFromFile(
        L"Object3D.PS.hlsl",
        L"main",
        L"ps_6_0"
    );
    if (!psBlob) {
        return false;
    }

    // ============================
    // 6. InputLayout / Blend / Raster / Depth
    // ============================
    // 頂点レイアウト（POSITION/ TEXCOORD / NORMAL）
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[3] = {};

    // POSITION (float4)
    inputElementDescs[0].SemanticName = "POSITION";
    inputElementDescs[0].SemanticIndex = 0;
    inputElementDescs[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    inputElementDescs[0].InputSlot = 0;
    inputElementDescs[0].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
    inputElementDescs[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    inputElementDescs[0].InstanceDataStepRate = 0;

    // TEXCOORD (float2)
    inputElementDescs[1].SemanticName = "TEXCOORD";
    inputElementDescs[1].SemanticIndex = 0;
    inputElementDescs[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    inputElementDescs[1].InputSlot = 0;
    inputElementDescs[1].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
    inputElementDescs[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    inputElementDescs[1].InstanceDataStepRate = 0;

    // NORMAL (float3)
    inputElementDescs[2].SemanticName = "NORMAL";
    inputElementDescs[2].SemanticIndex = 0;
    inputElementDescs[2].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    inputElementDescs[2].InputSlot = 0;
    inputElementDescs[2].AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
    inputElementDescs[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    inputElementDescs[2].InstanceDataStepRate = 0;

    D3D12_INPUT_LAYOUT_DESC inputLayoutDesc{};
    inputLayoutDesc.pInputElementDescs = inputElementDescs;
    inputLayoutDesc.NumElements = _countof(inputElementDescs);

    // Blend
    D3D12_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // （必要ならここで AlphaBlend を有効化）

    // Rasterizer
    D3D12_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.DepthClipEnable = TRUE;

    // DepthStencil
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc{};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    depthStencilDesc.StencilEnable = FALSE;

    // ============================
    // 7. PSO 生成
    // ============================
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = rootSignature_.Get();
    psoDesc.InputLayout = inputLayoutDesc;

    psoDesc.VS = {
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize()
    };
    psoDesc.PS = {
        psBlob->GetBufferPointer(),
        psBlob->GetBufferSize()
    };

    psoDesc.BlendState = blendDesc;
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.DepthStencilState = depthStencilDesc;

    psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    hr = device_->CreateGraphicsPipelineState(
        &psoDesc,
        IID_PPV_ARGS(graphicsPipelineState_.GetAddressOf()));
    if (FAILED(hr)) {
        return false;
    }

    return true;
}


//----------------------------------------
// スプライト用 VB/IB の作成
//----------------------------------------
bool DebugSceneRenderer::CreateSpriteGeometry() {
    // main.cpp の「Sprite用頂点リソース作成」周りをほぼそのまま移植します。
    //
    // 元のコード：
    //   ComPtr<ID3D12Resource> vertexResourceSprite =
    //       CreateBufferResource(device, sizeof(VertexData) * 6);
    //   ComPtr<ID3D12Resource> indexResourceSprite =
    //       CreateBufferResource(device, sizeof(uint32_t) * 6);
    //   D3D12_VERTEX_BUFFER_VIEW vertexBufferViewSprite{};
    //   ...
    //   D3D12_INDEX_BUFFER_VIEW indexBufferViewSprite{};
    //   ...
    //   vertexResourceSprite->Map(..., (void**)&vertexDataSprite);
    //   indexResourceSprite->Map(..., (void**)&indexDataSprite);
    //
    // これをメンバに置き換え：
    //
    //   vertexResourceSprite_ = CreateBufferResource(device_, sizeof(VertexData) * 6);
    //   indexResourceSprite_  = CreateBufferResource(device_, sizeof(uint32_t) * 6);
    //   vertexBufferViewSprite_... = ...
    //   indexBufferViewSprite_ ... = ...
    //   vertexResourceSprite_->Map(...);
    //   indexResourceSprite_->Map(...);

    vertexResourceSprite_ = CreateBufferResource(device_, sizeof(VertexData) * 6);
    indexResourceSprite_ = CreateBufferResource(device_, sizeof(uint32_t) * 6);

    vertexBufferViewSprite_.BufferLocation = vertexResourceSprite_->GetGPUVirtualAddress();
    vertexBufferViewSprite_.SizeInBytes = sizeof(VertexData) * 6;
    vertexBufferViewSprite_.StrideInBytes = sizeof(VertexData);

    indexBufferViewSprite_.BufferLocation = indexResourceSprite_->GetGPUVirtualAddress();
    indexBufferViewSprite_.SizeInBytes = sizeof(uint32_t) * 6;
    indexBufferViewSprite_.Format = DXGI_FORMAT_R32_UINT;

    // 頂点 / インデックスの内容は main.cpp と同じでOK
    VertexData* vtx = nullptr;
    vertexResourceSprite_->Map(0, nullptr, reinterpret_cast<void**>(&vtx));

    uint32_t* idx = nullptr;
    indexResourceSprite_->Map(0, nullptr, reinterpret_cast<void**>(&idx));
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 1; idx[4] = 3; idx[5] = 2;

    // vtx[0..3] の position / normal / texcoord は
    // main.cpp に書いてあるパターンをそのままここに移植してね。

    return true;
}

//----------------------------------------
// CBuffer 作成（Material / WVP / Light）
//----------------------------------------
bool DebugSceneRenderer::CreateConstantBuffers() {
    // Material CBuffer
    materialResourceSprite_ = CreateBufferResource(device_, sizeof(Material));
    materialResourceSprite_->Map(
        0, nullptr, reinterpret_cast<void**>(&materialDataSprite_));

    // WVP（CreateRootSignatureAndPSO で作っても OK。どちらか片方で）
    if (!wvpResource_) {
        wvpResource_ = CreateBufferResource(device_, sizeof(Matrix4x4));
        wvpResource_->Map(0, nullptr, reinterpret_cast<void**>(&wvpData_));
        *wvpData_ = MakeIdentity4x4();
    }

    // Light CBuffer
    directionalLightResource_ = CreateBufferResource(device_, sizeof(DirectionalLight));
    directionalLightResource_->Map(
        0, nullptr, reinterpret_cast<void**>(&directionalLightData_));

    return true;
}

//----------------------------------------
// テクスチャ読み込み（uvChecker）
//----------------------------------------
bool DebugSceneRenderer::LoadTextureUVChecker() {
    // ここは main.cpp で uvChecker.png を読み込んでいたコードを
    // そのまま移植してOK。
    //
    // すでに TextureHelper.cpp/h があるので、それに合わせて
    //   - CreateTextureResourceResolution
    //   - CreateUploadHeap
    //   - CreateTextureSRV
    // などを使う。

    // ここでは SRV ハンドルだけ確保してメンバに保持する例だけ書いておく。

    D3D12_CPU_DESCRIPTOR_HANDLE srvStart =
        srvHeap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srvStartGPU =
        srvHeap_->GetGPUDescriptorHandleForHeapStart();

    // ひとまず「0番目」を使う例（実際は TextureManager 等で取得した index を使う）
    textureSrvCPU_ = srvStart;
    textureSrvGPU_ = srvStartGPU;

    return true;
}

//----------------------------------------
// Update
//----------------------------------------
void DebugSceneRenderer::Update(float deltaTime) {
    // カメラ更新
    debugCamera_.Update();

    // ライトの正規化
    directionalLightData_->color = directionalLightInit_.color;
    directionalLightData_->direction = directionalLightInit_.direction;
    directionalLightData_->intensity = directionalLightInit_.intensity;

    // WVP 行列を更新（world * view * proj）
    Matrix4x4 world =
        MakeAffineMatrix(worldTransform_.scale, worldTransform_.rotate, worldTransform_.translate);
    Matrix4x4 view = debugCamera_.GetViewMatrix();
    Matrix4x4 proj = debugCamera_.GetProjectionMatrix();
    *wvpData_ = Multiply(world, Multiply(view, proj));

    // UV Transform などが必要なら materialDataSprite_ に書き込む
}

//----------------------------------------
// Render
//----------------------------------------
void DebugSceneRenderer::Render(
    ge3::graphics::RenderContext& ctx,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* backBuffer,
    UINT backBufferIndex
) {
    assert(cmdList);
    assert(backBuffer);

    // ここでは「BeginFrame は main 側で呼ばれている」前提。
    // RootSignature / PSO / VBV,IBV / CBV / SRV 設定をまとめる。

    // RootSignature / PSO
    cmdList->SetGraphicsRootSignature(rootSignature_.Get());
    cmdList->SetPipelineState(graphicsPipelineState_.Get());

    // VB / IB
    cmdList->IASetVertexBuffers(0, 1, &vertexBufferViewSprite_);
    cmdList->IASetIndexBuffer(&indexBufferViewSprite_);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // CBV
    cmdList->SetGraphicsRootConstantBufferView(0, materialResourceSprite_->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootConstantBufferView(1, wvpResource_->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootConstantBufferView(3, directionalLightResource_->GetGPUVirtualAddress());

    // SRV ヒープ
    ID3D12DescriptorHeap* heaps[] = { srvHeap_ };
    cmdList->SetDescriptorHeaps(1, heaps);

    // t0 用 SRV
    cmdList->SetGraphicsRootDescriptorTable(2, textureSrvGPU_);

    // 描画
    cmdList->DrawIndexedInstanced(6, 1, 0, 0, 0);
}
