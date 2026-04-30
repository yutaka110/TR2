#include "graphics/PipelineBuilder.h"
#include <windows.h> // OutputDebugStringA

using namespace ge3::graphics;

static void DebugPrintA(const char* s) {
#if defined(_DEBUG)
    OutputDebugStringA(s);
#endif
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> PipelineBuilder::CreateRootSignature(
    ID3D12Device* device,
    const D3D12_ROOT_SIGNATURE_DESC& desc)
{
    ComPtr<ID3D12RootSignature> rootSig;
    if (!device) {
        DebugPrintA("[PipelineBuilder] CreateRootSignature: device is null\n");
        return rootSig;
    }

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;

    HRESULT hr = D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &errors);

    if (FAILED(hr)) {
        DebugPrintA("[PipelineBuilder] D3D12SerializeRootSignature failed\n");
        if (errors) {
            OutputDebugStringA((const char*)errors->GetBufferPointer());
        }
        return rootSig;
    }

    hr = device->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(&rootSig));

    if (FAILED(hr)) {
        DebugPrintA("[PipelineBuilder] CreateRootSignature failed\n");
        return ComPtr<ID3D12RootSignature>();
    }

    return rootSig;
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineBuilder::CreateGraphicsPipelineState(
    ID3D12Device* device,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    ComPtr<ID3D12PipelineState> pso;
    if (!device) {
        DebugPrintA("[PipelineBuilder] CreateGraphicsPipelineState: device is null\n");
        return pso;
    }

    HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        DebugPrintA("[PipelineBuilder] CreateGraphicsPipelineState failed\n");
        return ComPtr<ID3D12PipelineState>();
    }

    return pso;
}
