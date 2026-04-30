#pragma once
#include "wrl.h"
#include <d3d11.h>
#include <d3d12.h>

using Microsoft::WRL::ComPtr;

namespace media {

struct CpuTransferCtx {
	// A) D3D11 Staging 再利用用
	ComPtr<ID3D11Texture2D> stagingTex;
	D3D11_TEXTURE2D_DESC    stagingDesc{}; // 現状を保持

	// D) STREAM_CHANGE 対応（出力側の変化に追従）
	D3D12_RESOURCE_DESC     lastDstDesc{}; // 直近のdstRGBTex12のDesc

	// B/C) D3D12 Upload リング & フェンス
	static constexpr UINT   kRing = 3;
	ComPtr<ID3D12Resource>  upload[kRing];
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; // CopyableFootprints
	UINT                    numRows = 0;
	UINT64                  rowSize = 0;
	UINT64                  totalBytes = 0;
	UINT64                  fenceValue[kRing]{};
	ComPtr<ID3D12Fence>     fence;
	HANDLE                  fenceEvent = nullptr;
	UINT64                  nextFence = 1;
	UINT                    ringIndex = 0;

	// 内部専用：即席コピー用の D3D12 コマンド（他と干渉しない）
	ComPtr<ID3D12CommandAllocator> cmdAlloc;
	ComPtr<ID3D12GraphicsCommandList> cmdList;
};


struct D3DResourceLeakChecker {
	~D3DResourceLeakChecker() {

		// リソースリークチェック
		Microsoft::WRL::ComPtr<IDXGIDebug1> debug;
		if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug)))) {
			debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
			debug->ReportLiveObjects(DXGI_DEBUG_APP, DXGI_DEBUG_RLO_ALL);
			debug->ReportLiveObjects(DXGI_DEBUG_D3D12, DXGI_DEBUG_RLO_ALL);
		}
	}
};



} // namespace media
