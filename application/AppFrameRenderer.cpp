#include "AppFrameRenderer.h"

void AppFrameRenderer::BeginFrame(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* backBuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv,
    const float clearColor[4]) const {
    if (commandList == nullptr || backBuffer == nullptr || clearColor == nullptr) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);

    commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(
        dsv,
        D3D12_CLEAR_FLAG_DEPTH,
        1.0f,
        0,
        0,
        nullptr);
}

bool AppFrameRenderer::PrepareMainPass(
    ID3D12GraphicsCommandList* commandList,
    const D3D12_VIEWPORT& viewport,
    const D3D12_RECT& scissorRect,
    ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pipelineState) const {
    if (commandList == nullptr || rootSignature == nullptr || pipelineState == nullptr) {
        return false;
    }

    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissorRect);
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    return true;
}

void AppFrameRenderer::DrawMainModel(
    ID3D12GraphicsCommandList* commandList,
    const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView,
    D3D12_GPU_VIRTUAL_ADDRESS materialBufferAddress,
    D3D12_GPU_VIRTUAL_ADDRESS transformBufferAddress,
    D3D12_GPU_DESCRIPTOR_HANDLE textureHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE receivedTextureHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE motionMaskTextureHandle,
    D3D12_GPU_VIRTUAL_ADDRESS directionalLightBufferAddress,
    D3D12_GPU_VIRTUAL_ADDRESS cameraBufferAddress,
    D3D12_GPU_VIRTUAL_ADDRESS pointLightBufferAddress,
    D3D12_GPU_VIRTUAL_ADDRESS spotLightBufferAddress,
    uint32_t vertexCount) const {
    if (commandList == nullptr || vertexCount == 0 ||
        materialBufferAddress == 0 || transformBufferAddress == 0 ||
        textureHandle.ptr == 0 || directionalLightBufferAddress == 0 ||
        cameraBufferAddress == 0 || pointLightBufferAddress == 0 ||
        spotLightBufferAddress == 0) {
        return;
    }

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &vertexBufferView);

    ge3::graphics::MaterialInstance material{};
    material.name = "MainModel";
    material.parameters.cbv[0] = materialBufferAddress;
    material.parameters.cbv[1] = transformBufferAddress;
    material.parameters.cbv[3] = directionalLightBufferAddress;
    material.parameters.cbv[6] = cameraBufferAddress;
    material.parameters.cbv[7] = pointLightBufferAddress;
    material.parameters.cbv[8] = spotLightBufferAddress;
    material.textures.srv[2] = textureHandle;
    material.textures.srv[4] = receivedTextureHandle;
    material.textures.srv[5] = motionMaskTextureHandle;
    ApplyMaterialInstance(commandList, material);
    commandList->DrawInstanced(vertexCount, 1, 0, 0);
}

void AppFrameRenderer::ApplyMaterialInstance(
    ID3D12GraphicsCommandList* commandList,
    const ge3::graphics::MaterialInstance& material) const {
    if (commandList == nullptr) {
        return;
    }

    if (material.parameters.cbv[0] != 0) commandList->SetGraphicsRootConstantBufferView(0, material.parameters.cbv[0]);
    if (material.parameters.cbv[1] != 0) commandList->SetGraphicsRootConstantBufferView(1, material.parameters.cbv[1]);
    if (material.textures.srv[2].ptr != 0) commandList->SetGraphicsRootDescriptorTable(2, material.textures.srv[2]);
    if (material.parameters.cbv[3] != 0) commandList->SetGraphicsRootConstantBufferView(3, material.parameters.cbv[3]);
    if (material.textures.srv[4].ptr != 0) commandList->SetGraphicsRootDescriptorTable(4, material.textures.srv[4]);
    if (material.textures.srv[5].ptr != 0) commandList->SetGraphicsRootDescriptorTable(5, material.textures.srv[5]);
    if (material.parameters.cbv[6] != 0) commandList->SetGraphicsRootConstantBufferView(6, material.parameters.cbv[6]);
    if (material.parameters.cbv[7] != 0) commandList->SetGraphicsRootConstantBufferView(7, material.parameters.cbv[7]);
    if (material.parameters.cbv[8] != 0) commandList->SetGraphicsRootConstantBufferView(8, material.parameters.cbv[8]);
}

void AppFrameRenderer::DrawSprite(
    ID3D12GraphicsCommandList* commandList,
    ID3D12DescriptorHeap* descriptorHeap,
    const D3D12_INDEX_BUFFER_VIEW& indexBufferView,
    const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView,
    D3D12_GPU_VIRTUAL_ADDRESS materialBufferAddress,
    D3D12_GPU_VIRTUAL_ADDRESS transformBufferAddress,
    D3D12_GPU_DESCRIPTOR_HANDLE textureHandle) const {
    if (commandList == nullptr || descriptorHeap == nullptr ||
        materialBufferAddress == 0 || transformBufferAddress == 0 ||
        textureHandle.ptr == 0) {
        return;
    }

    ID3D12DescriptorHeap* descriptorHeaps[] = { descriptorHeap };
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
    commandList->SetGraphicsRootConstantBufferView(0, materialBufferAddress);
    commandList->SetGraphicsRootConstantBufferView(1, transformBufferAddress);
    commandList->SetGraphicsRootDescriptorTable(2, textureHandle);
    commandList->IASetIndexBuffer(&indexBufferView);
    commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawIndexedInstanced(6, 1, 0, 0, 0);
}

void AppFrameRenderer::PrepareSphere(
    ID3D12GraphicsCommandList* commandList,
    ID3D12RootSignature* rootSignature,
    ID3D12PipelineState* pipelineState,
    D3D12_GPU_VIRTUAL_ADDRESS cameraBufferAddress,
    const D3D12_VERTEX_BUFFER_VIEW& sphereVertexBufferView) const {
    if (commandList == nullptr || rootSignature == nullptr || pipelineState == nullptr) {
        return;
    }

    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetPipelineState(pipelineState);
    commandList->SetGraphicsRootConstantBufferView(6, cameraBufferAddress);
    commandList->IASetVertexBuffers(0, 1, &sphereVertexBufferView);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void AppFrameRenderer::EndFrame(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* backBuffer) const {
    if (commandList == nullptr || backBuffer == nullptr) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}
