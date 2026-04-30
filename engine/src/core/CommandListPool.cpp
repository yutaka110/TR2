#include "core/CommandListPool.h"
#include "core/Device.h"
#include "utils/Logger.h"
#include <cassert>
#include <sstream>   // 追加

using Microsoft::WRL::ComPtr;
using namespace core;

static std::string HexHR(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex << (unsigned)(hr & 0xFFFFFFFF);
    return oss.str();
}


// frameCount はスワップチェーンのバッファ数に合わせる（例: 2）
bool CommandListPool::Initialize(Device& dev, UINT frameCount)
{
    frameCount_ = frameCount;
    allocators_.resize(frameCount_);

    auto* d3d = dev.GetDevice();
    HRESULT hr = S_OK;

    for (UINT i = 0; i < frameCount_; ++i) {
        hr = d3d->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i]));
        if (FAILED(hr)) {
            std::ostringstream oss;
            oss << "[CommandListPool] CreateCommandAllocator[" << i << "] failed: " << HexHR(hr) << "\n";
            Logger::Error(oss.str());
            return false;
        }
    }

    hr = d3d->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocators_[0].Get(), nullptr,
        IID_PPV_ARGS(&list_));
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << "[CommandListPool] CreateCommandList failed: " << HexHR(hr) << "\n";
        Logger::Error(oss.str());
        return false;
    }

    hr = list_->Close();
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << "[CommandListPool] Initial Close() failed: " << HexHR(hr) << "\n";
        Logger::Error(oss.str());
        return false;
    }

    Logger::Info("[CommandListPool] Initialized successfully.\n");
    return true;
}

// Begin: 指定フレームの Allocator を Reset し、リストを Reset して返す
ID3D12GraphicsCommandList* CommandListPool::Begin(UINT frameIndex, ID3D12PipelineState* pso)
{
    assert(frameIndex < frameCount_);
    currentFrame_ = frameIndex;

    HRESULT hr = allocators_[currentFrame_]->Reset();
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << "[CommandListPool] CommandAllocator[" << currentFrame_ << "] Reset() failed: " << HexHR(hr) << "\n";
        Logger::Error(oss.str());
        return nullptr;
    }

    hr = list_->Reset(allocators_[currentFrame_].Get(), pso);
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << "[CommandListPool] CommandList Reset() failed on frame " << currentFrame_ << ": " << HexHR(hr) << "\n";
        Logger::Error(oss.str());
        return nullptr;
    }

    return list_.Get();
}

// EndAndExecute: Close→Execute（QueueはDeviceから取得）
void CommandListPool::EndAndExecute(Device& dev)
{
    HRESULT hr = list_->Close();
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << "[CommandListPool] CommandList Close() failed: " << HexHR(hr) << "\n";
        Logger::Error(oss.str());
        return;
    }

    ID3D12CommandList* lists[] = { list_.Get() };
    dev.GetCommandQueue()->ExecuteCommandLists(1, lists);

    // Trace が無いようなので Info に落とす or 削除
    Logger::Info("[CommandListPool] CommandList executed.\n");
}
