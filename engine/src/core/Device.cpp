#include "core/Device.h"
#include <wrl/client.h>
#include <cassert>
#include <cstdio>

using Microsoft::WRL::ComPtr;
using namespace core;

namespace {
#if defined(_DEBUG)
    void EnableD3D12DebugLayerIfAvailable(bool enable)
    {
        if (!enable) return;
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
        }
    }
#else
    void EnableD3D12DebugLayerIfAvailable(bool) {}
#endif

    // 簡易ログ（必要なら utils::Logger に置き換えてOK）
    void Log(const char* fmt, ...)
    {
#if defined(_DEBUG)
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        OutputDebugStringA(buf);
#endif
    }
} // namespace

bool Device::Initialize(bool enableDebugLayer)
{
    Shutdown();

    EnableD3D12DebugLayerIfAvailable(enableDebugLayer);

    if (!CreateFactory(enableDebugLayer)) {
        Log("[Device] CreateFactory failed.\n");
        return false;
    }

    ComPtr<IDXGIAdapter4> adapter;
    if (!PickAdapter(adapter)) {
        Log("[Device] No suitable adapter found. Trying WARP...\n");
        // WARP fallback
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
            Log("[Device] EnumWarpAdapter failed.\n");
            return false;
        }
        if (FAILED(warp.As(&adapter))) {
            Log("[Device] Adapter cast to IDXGIAdapter4 failed.\n");
            return false;
        }
    }

    if (!CreateDeviceAndQueue(adapter.Get(), enableDebugLayer)) {
        Log("[Device] CreateDeviceAndQueue failed.\n");
        return false;
    }

    //#ifdef _DEBUG
//	ComPtr<ID3D12InfoQueue> infoQueue = nullptr;
//	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&infoQueue)))) {
//		// やばいエラー時に止まる
//		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
//
//		// エラー時に止まる
//		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
//
//		// 警告時に止まる
//		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
//
//		// 抑制するメッセージのID
//		D3D12_MESSAGE_ID denyIds[] = {
//			D3D12_MESSAGE_ID_RESOURCE_BARRIER_INVALID_COMMAND_LIST_TYPE };
//
//		////抑制するレベル
//		D3D12_MESSAGE_SEVERITY severities[] = { D3D12_MESSAGE_SEVERITY_INFO };
//		D3D12_INFO_QUEUE_FILTER filter{};
//		filter.DenyList.NumIDs = _countof(denyIds);
//		filter.DenyList.pIDList = denyIds;
//		filter.DenyList.NumSeverities = _countof(severities);
//		filter.DenyList.pSeverityList = severities;
//
//		// 指定したメッセージの表示を抑制する
//		infoQueue->PushStorageFilter(&filter);
//
//		// 解放
//		infoQueue->Release();
//	}
//#endif

    if (!CreateFence()) {
        Log("[Device] CreateFence failed.\n");
        return false;
    }

    CacheDescriptorIncrements();
    CheckTearingSupport();

    Log("[Device] Initialize OK. FL=%#x, RTVInc=%u, CBVSRVUAVInc=%u, Tearing=%d\n",
        (unsigned)featureLevel_, rtvIncSize_, cbvSrvUavIncSize_, allowTearing_);
    return true;
}

void Device::Shutdown()
{
    fence_.Reset();
    queue_.Reset();
    device_.Reset();
    factory_.Reset();
    rtvIncSize_ = cbvSrvUavIncSize_ = 0;
    featureLevel_ = D3D_FEATURE_LEVEL_12_0;
    allowTearing_ = false;
}

bool Device::CreateFactory(bool enableDebugLayer)
{
    UINT flags = 0;
#if defined(_DEBUG)
    if (enableDebugLayer) flags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    return SUCCEEDED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory_)));
}

bool Device::PickAdapter(ComPtr<IDXGIAdapter4>& outAdapter)
{
    // ハイパフォーマンス順で列挙、ソフトウェアは除外
    ComPtr<IDXGIFactory6> fac = factory_;
    ComPtr<IDXGIAdapter1> ad1;
    for (UINT i = 0;
        fac->EnumAdapterByGpuPreference(i,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&ad1)) != DXGI_ERROR_NOT_FOUND;
        ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        ad1->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue; // ソフトウェアはスキップ（必要なら条件変更）
        }
        // D3D12 が作れるか軽く確認
        if (SUCCEEDED(D3D12CreateDevice(ad1.Get(), D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device), nullptr)) ||
            SUCCEEDED(D3D12CreateDevice(ad1.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr)))
        {
            ad1.As(&outAdapter);
            return true;
        }
    }
    return false; // 見つからなかった（WARPへフォールバックさせる）
}

bool Device::CreateDeviceAndQueue(IDXGIAdapter4* adapter, bool /*enableDebugLayer*/)
{
    // まず 12_1 を試し、だめなら 12_0
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device_));
    featureLevel_ = SUCCEEDED(hr) ? D3D_FEATURE_LEVEL_12_1 : D3D_FEATURE_LEVEL_12_0;
    if (FAILED(hr)) {
        hr = D3D12CreateDevice(adapter, featureLevel_, IID_PPV_ARGS(&device_));
        if (FAILED(hr)) return false;
    }

    // Directキュー作成
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(device_->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue_)))) {
        return false;
    }

    return true;
}

bool Device::CreateFence()
{
    return SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
}

void Device::CacheDescriptorIncrements()
{
    rtvIncSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    cbvSrvUavIncSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void Device::CheckTearingSupport()
{
    allowTearing_ = false;
    ComPtr<IDXGIFactory5> fac5;
    if (SUCCEEDED(factory_.As(&fac5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(fac5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
            allowTearing_ = (allow == TRUE);
        }
    }
}
