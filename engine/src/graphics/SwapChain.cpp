#include "graphics/SwapChain.h"
#include "core/Device.h"
#include <cassert>

using Microsoft::WRL::ComPtr;
using namespace graphics;

bool SwapChain::Create(core::Device& dev, HWND hwnd, UINT w, UINT h, UINT bufferCount)
{
    bufferCount_ = bufferCount;
    allowTearing_ = dev.IsTearingSupported();

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = w;
    scDesc.Height = h;
    scDesc.Format = format_;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = bufferCount_;
    scDesc.SampleDesc = { 1, 0 };
    scDesc.Scaling = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scDesc.Flags = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> sc1;
    auto* factory = dev.GetFactory();
    auto* queue = dev.GetCommandQueue();

    HRESULT hr = factory->CreateSwapChainForHwnd(queue, hwnd, &scDesc, nullptr, nullptr, &sc1);
    if (FAILED(hr)) return false;

    hr = sc1.As(&swapChain_);
    if (FAILED(hr)) return false;

    // Alt+Enter を無効化（推奨）
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = bufferCount_;
    hr = dev.GetDevice()->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_));
    if (FAILED(hr)) return false;

    CreateRTVs(dev);
    return true;
}

void SwapChain::CreateRTVs(core::Device& dev)
{
    ReleaseBuffers();
    backBuffers_.resize(bufferCount_);
    rtvHandles_.resize(bufferCount_);

    auto inc = dev.GetRTVDescriptorSize();
    auto h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < bufferCount_; ++i) {
        swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
        dev.GetDevice()->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, h);
        rtvHandles_[i] = h;
        h.ptr += inc;
    }
}

void SwapChain::ReleaseBuffers()
{
    for (auto& bb : backBuffers_) bb.Reset();
    backBuffers_.clear();
    rtvHandles_.clear();
}

void SwapChain::Resize(core::Device& dev, UINT w, UINT h)
{
    if (!swapChain_) return;

    // --- GPUの完了待ち（Flushの代替：Fenceで同期） ---
    ID3D12CommandQueue* queue = dev.GetCommandQueue();
    ID3D12Fence* fence = dev.GetFence();

    static UINT64 s_fenceValue = 0;              // この関数内で使うカウンタ（メンバにしてもOK）
    const UINT64 signalValue = ++s_fenceValue;

    // 1) シグナル
    HRESULT hr = queue->Signal(fence, signalValue);
    if (SUCCEEDED(hr)) {
        // 2) 完了待ち
        if (fence->GetCompletedValue() < signalValue) {
            HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (hEvent) {
                fence->SetEventOnCompletion(signalValue, hEvent);
                WaitForSingleObject(hEvent, INFINITE);
                CloseHandle(hEvent);
            }
        }
    }
    // --- ここまででGPUが前フレームの使用を終えている ---

    // 既存処理
    ReleaseBuffers();
    swapChain_->ResizeBuffers(
        bufferCount_, w, h, format_,
        allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

    CreateRTVs(dev);
}


void SwapChain::Present(core::Device& dev, UINT syncInterval)
{
    UINT flags = (allowTearing_ && syncInterval == 0) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    swapChain_->Present(syncInterval, flags);
}
