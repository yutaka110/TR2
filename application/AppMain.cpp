#define _WINSOCKAPI_
#define _WIN32_WINNT 0x0602 // Windows 8
#define ANCHOR_TEST

#include "camera/debugCamera.h"
#include "utils/math/MathUtils.h"
#include"utils/math/Vector.h"
#include "../../externals/DirectXTex/DirectXTex.h"
#include "../../externals/DirectXTex/d3dx12.h"
#include "../../externals/imgui/imgui.h"
#include "../../externals/imgui/imgui_impl_dx12.h"
#include "../../externals/imgui/imgui_impl_win32.h"
#include "wrl.h"
#include <Windows.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <codecvt>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <d3d12.h>
#include <dbghelp.h>
#include <dxcapi.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <format>
#include <fstream>
#include <locale>
#include <mutex>
#include <numbers>
#include <queue>
#include <sstream>
#include <string>
#include <strsafe.h>
#include <thread>
#include <unordered_map>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <comdef.h>
#include <propidl.h>
#include <wincodec.h>
#include <map> 

#include"../media/TextureHelper.h"
#include "../media/CameraCapture.h"
#include "core/Device.h"
#include "graphics/SwapChain.h"
#include "core/CommandListPool.h"
#include "core/DescriptorHeap.h"
#include "graphics/RenderContext.h"
#include"utils/dx12/BufferHelper.h"
#include <random>
#include "ModelLoaderAssimp.h"
#include "AppAudio.h"
#include "AppBootstrap.h"
#include "AppImGuiLayer.h"
#include "AppFrameRenderer.h"
#include "AppPipelines.h"
#include "AppParticleSystem.h"
#include "AppRenderResources.h"
#include "AppRunLoop.h"
#include "AppRuntimeState.h"
#include "AppRuntimeUtils.h"
#include "AppSceneResources.h"
#include "EngineContext.h"
#include "../network/UdpReceiver.h"
#include "../network/NetworkManager.h"
#include "../network/AdaptiveStreamingController.h"
#include "../network/NetworkCsvLogger.h"
#include "../network/NetworkExperimentReporter.h"
#include "../network/NetworkExperimentRunner.h"
#include "../network/NetworkRuntimeMode.h"
#include "../network/NetworkVideoReceiver.h"
#include "../network/PacketProtocol.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <iostream>

using namespace DirectX;

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "dxcompiler.lib")
#pragma comment(lib, "Dbghelp.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

using namespace Microsoft::WRL;

namespace {
	constexpr uint16_t kRnvpListenPort = 50000;
	constexpr const char* kRnvpRemoteIp = "127.0.0.1";
	constexpr uint16_t kRnvpRemotePort = kRnvpListenPort;
	constexpr uint32_t kReceivedVideoUploadBufferCount = 3;

	std::string ToLowerAscii(std::string value) {
		std::transform(
			value.begin(),
			value.end(),
			value.begin(),
			[](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
		return value;
	}

	bool TryReadEnvString(const char* name, std::string& outValue) {
		char buffer[64]{};
		const DWORD length = GetEnvironmentVariableA(
			name,
			buffer,
			static_cast<DWORD>(sizeof(buffer)));
		if (length == 0 || length >= sizeof(buffer)) {
			return false;
		}

		outValue.assign(buffer, length);
		return true;
	}

	net::AdaptiveControlMode ParseAdaptiveControlModeEnv(
		const std::string& value,
		net::AdaptiveControlMode fallback
	) {
		const std::string mode = ToLowerAscii(value);
		if (mode == "0" || mode == "fixed" || mode == "fixedquality" ||
			mode == "fixed-quality") {
			return net::AdaptiveControlMode::FixedQuality;
		}
		if (mode == "1" || mode == "loss" || mode == "lossreactive" ||
			mode == "loss-reactive") {
			return net::AdaptiveControlMode::LossReactive;
		}
		if (mode == "2" || mode == "qoe" || mode == "deadline" ||
			mode == "qoe-deadline" || mode == "qoedeadlineadaptive") {
			return net::AdaptiveControlMode::QoeDeadlineAdaptive;
		}
		return fallback;
	}

	net::CongestionControlMode ParseCongestionControlModeEnv(
		const std::string& value,
		net::CongestionControlMode fallback
	) {
		const std::string mode = ToLowerAscii(value);
		if (mode == "0" || mode == "loss" || mode == "lossbased" ||
			mode == "loss-based") {
			return net::CongestionControlMode::LossBased;
		}
		if (mode == "1" || mode == "delay" || mode == "delaybased" ||
			mode == "delay-based") {
			return net::CongestionControlMode::DelayBased;
		}
		if (mode == "2" || mode == "hybrid") {
			return net::CongestionControlMode::Hybrid;
		}
		return fallback;
	}

	std::vector<uint8_t> ResizeRgbaBilinear(
		const std::vector<uint8_t>& src,
		uint32_t srcWidth,
		uint32_t srcHeight,
		uint32_t dstWidth,
		uint32_t dstHeight) {
		if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0) {
			return {};
		}

		const size_t requiredBytes =
			static_cast<size_t>(srcWidth) *
			static_cast<size_t>(srcHeight) *
			4u;

		if (src.size() < requiredBytes) {
			return {};
		}

		if (srcWidth == dstWidth && srcHeight == dstHeight) {
			return src;
		}

		std::vector<uint8_t> dst(
			static_cast<size_t>(dstWidth) *
			static_cast<size_t>(dstHeight) *
			4u
		);

		for (uint32_t y = 0; y < dstHeight; ++y) {
			const double srcYf =
				dstHeight <= 1
				? 0.0
				: (static_cast<double>(y) * static_cast<double>(srcHeight - 1u)) /
					static_cast<double>(dstHeight - 1u);
			const uint32_t y0 =
				(std::min<uint32_t>)(srcHeight - 1u, static_cast<uint32_t>(srcYf));
			const uint32_t y1 = (std::min<uint32_t>)(srcHeight - 1u, y0 + 1u);
			const double wy = srcYf - static_cast<double>(y0);

			for (uint32_t x = 0; x < dstWidth; ++x) {
				const size_t dstIndex =
					(static_cast<size_t>(y) * dstWidth + x) * 4u;

				const double srcXf =
					dstWidth <= 1
					? 0.0
					: (static_cast<double>(x) * static_cast<double>(srcWidth - 1u)) /
						static_cast<double>(dstWidth - 1u);
				const uint32_t x0 =
					(std::min<uint32_t>)(srcWidth - 1u, static_cast<uint32_t>(srcXf));
				const uint32_t x1 = (std::min<uint32_t>)(srcWidth - 1u, x0 + 1u);
				const double wx = srcXf - static_cast<double>(x0);

				const size_t i00 =
					(static_cast<size_t>(y0) * srcWidth + x0) * 4u;
				const size_t i10 =
					(static_cast<size_t>(y0) * srcWidth + x1) * 4u;
				const size_t i01 =
					(static_cast<size_t>(y1) * srcWidth + x0) * 4u;
				const size_t i11 =
					(static_cast<size_t>(y1) * srcWidth + x1) * 4u;

				for (uint32_t c = 0; c < 4u; ++c) {
					const double top =
						static_cast<double>(src[i00 + c]) * (1.0 - wx) +
						static_cast<double>(src[i10 + c]) * wx;
					const double bottom =
						static_cast<double>(src[i01 + c]) * (1.0 - wx) +
						static_cast<double>(src[i11 + c]) * wx;
					const double value = top * (1.0 - wy) + bottom * wy;
					dst[dstIndex + c] =
						static_cast<uint8_t>(std::clamp(
							static_cast<int>(std::lround(value)),
							0,
							255
						));
				}
			}
		}

		return dst;
	}

	std::vector<uint8_t> PackRawRgbaPayload(
		const std::vector<uint8_t>& rgba,
		uint32_t width,
		uint32_t height) {
		const size_t imageBytes =
			static_cast<size_t>(width) *
			static_cast<size_t>(height) *
			4u;

		if (width == 0 || height == 0 || rgba.size() < imageBytes) {
			return {};
		}

		std::vector<uint8_t> payload(net::kRawFramePayloadHeaderSize + imageBytes);

		net::RawFramePayloadHeader header{};
		header.width = static_cast<uint16_t>(width);
		header.height = static_cast<uint16_t>(height);
		header.format = static_cast<uint8_t>(net::RawFrameFormat::Rgba8);
		header.payloadBytes = static_cast<uint32_t>(imageBytes);

		net::EncodeRawFramePayloadHeader(payload.data(), header);
		std::memcpy(
			payload.data() + net::kRawFramePayloadHeaderSize,
			rgba.data(),
			imageBytes
		);

		return payload;
	}

	std::vector<uint8_t> EncodeJpegFrame(
		const std::vector<uint8_t>& rgba,
		uint32_t width,
		uint32_t height,
		int quality) {
		const size_t imageBytes =
			static_cast<size_t>(width) *
			static_cast<size_t>(height) *
			4u;

		if (width == 0 || height == 0 || rgba.size() < imageBytes) {
			return {};
		}

		DirectX::Image image{};
		image.width = width;
		image.height = height;
		image.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		image.rowPitch = static_cast<size_t>(width) * 4u;
		image.slicePitch = image.rowPitch * static_cast<size_t>(height);
		image.pixels = const_cast<uint8_t*>(rgba.data());

		DirectX::Blob blob;
		const float imageQuality =
			static_cast<float>(std::clamp(quality, 1, 100)) / 100.0f;

		auto setJpegQuality =
			[imageQuality](IPropertyBag2* propertyBag) {
				if (!propertyBag) {
					return;
				}

				PROPBAG2 option{};
				option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");

				VARIANT value{};
				VariantInit(&value);
				value.vt = VT_R4;
				value.fltVal = imageQuality;

				propertyBag->Write(1, &option, &value);
				VariantClear(&value);
			};

		const HRESULT hr = DirectX::SaveToWICMemory(
			image,
			DirectX::WIC_FLAGS_NONE,
			DirectX::GetWICCodec(DirectX::WIC_CODEC_JPEG),
			blob,
			&GUID_WICPixelFormat24bppBGR,
			setJpegQuality
		);

		if (FAILED(hr) ||
			blob.GetBufferPointer() == nullptr ||
			blob.GetBufferSize() == 0) {
			static uint32_t encodeFailLogCount = 0;
			if (encodeFailLogCount < 10) {
				OutputDebugStringA("[AppMain] JPEG encode failed; falling back to Raw.\n");
				encodeFailLogCount++;
			}
			return {};
		}

		const uint8_t* bytes =
			static_cast<const uint8_t*>(blob.GetBufferPointer());

		return std::vector<uint8_t>(
			bytes,
			bytes + blob.GetBufferSize()
		);
	}
}

MaterialData LoadMaterialTemplateFile(const std::string& directoryPath,
	const std::string& filename);

ModelData LoadObjFile(const std::string& directoryPath,
	const std::string& filename) {
	ModelData modelData;
	std::vector<Vector4> positions;
	std::vector<Vector3> normals;
	std::vector<Vector2> texcoords;
	std::string line;

	std::ifstream file(directoryPath + "/" + filename);
	assert(file.is_open());

	while (std::getline(file, line)) {
		std::string identifier;
		std::istringstream s(line);

		s >> identifier;


		if (identifier == "v") {
			Vector4 position;

			s >> position.x >> position.y >> position.z;
			position.w = 1.0f;
			position.x *= -1.0f;
			positions.push_back(position);

		}
		else if (identifier == "vt") {
			Vector2 texcoord;

			s >> texcoord.x >> texcoord.y;
			texcoord.y = 1.0f - texcoord.y;
			texcoords.push_back(texcoord);

		}
		else if (identifier == "vn") {
			Vector3 normal;

			s >> normal.x >> normal.y >> normal.z;
			normal.x *= -1.0f;
			normals.push_back(normal);

		}
		else if (identifier == "f") {

			VertexData triangle[3];

			for (int32_t faceVertex = 0; faceVertex < 3; ++faceVertex) {
				std::string vertexDefinition;
				s >> vertexDefinition;

				std::istringstream w(vertexDefinition);
				uint32_t elementIndices[3];

				for (int32_t element = 0; element < 3; ++element) {
					std::string index;

					std::getline(w, index, '/');
					elementIndices[element] = std::stoi(index);
				}

				Vector4 position = positions[elementIndices[0] - 1];
				Vector2 texcoord = texcoords[elementIndices[1] - 1];
				Vector3 normal = normals[elementIndices[2] - 1];

				triangle[faceVertex] = { position, texcoord, normal };
			}

			modelData.vertices.push_back(triangle[2]);
			modelData.vertices.push_back(triangle[1]);
			modelData.vertices.push_back(triangle[0]);

		}
		else if (identifier == "mtllib") {
			std::string materialFilename;
			s >> materialFilename;

			modelData.material =
				LoadMaterialTemplateFile(directoryPath, materialFilename);
		}
	}

	return modelData;
}









MaterialData LoadMaterialTemplateFile(const std::string& directoryPath,
	const std::string& filename) {

	MaterialData materialData;
	std::string line;

	std::ifstream file(directoryPath + "/" + filename);

	assert(file.is_open());

	while (std::getline(file, line)) {
		std::string identifier;
		std::istringstream s(line);
		s >> identifier;

		if (identifier == "map_Kd") {
			std::string textureFilename;
			s >> textureFilename;

			materialData.textureFilePath = directoryPath + "/" + textureFilename;
		}
	}

	return materialData;
}










struct CpuTransferCtx {
	ComPtr<ID3D11Texture2D> stagingTex;
	D3D11_TEXTURE2D_DESC    stagingDesc{};

	D3D12_RESOURCE_DESC     lastDstDesc{};

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

	ComPtr<ID3D12CommandAllocator> cmdAlloc;
	ComPtr<ID3D12GraphicsCommandList> cmdList;
};


struct D3DResourceLeakChecker {
	~D3DResourceLeakChecker() {

		Microsoft::WRL::ComPtr<IDXGIDebug1> debug;
		if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debug)))) {
			debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
			debug->ReportLiveObjects(DXGI_DEBUG_APP, DXGI_DEBUG_RLO_ALL);
			debug->ReportLiveObjects(DXGI_DEBUG_D3D12, DXGI_DEBUG_RLO_ALL);
		}
	}
};




#include "AppMain.h"


bool AppMain::Initialize(HINSTANCE hInstance) {
	hInstance_ = hInstance;
	return true;
}

void AppMain::Finalize() {
}

int AppMain::Run() {
	D3DResourceLeakChecker leakCheck;

	AppBootstrap bootstrap;
	if (!bootstrap.Initialize(hInstance_)) {
		return -1;
	}

	const HWND hwnd = bootstrap.Handle();
	const uint32_t windowWidth = bootstrap.Width();
	const uint32_t windowHeight = bootstrap.Height();

	HRESULT hr = S_OK;


	EngineContext engineContext;
	if (!engineContext.Initialize(hwnd, windowWidth, windowHeight, /*enableDebugLayer=*/true)) {
		return -1;
	}

	core::Device& dev = engineContext.GetDevice();


	Microsoft::WRL::ComPtr < ID3D12Device> device = dev.GetDevice();
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> commandQueue = dev.GetCommandQueue();
	Microsoft::WRL::ComPtr<ID3D12Fence>        fence = dev.GetFence();



	auto& swapChain = engineContext.GetSwapChain();
	auto& clPool = engineContext.GetCommandListPool();







	auto& heaps = engineContext.GetHeaps();
	ComPtr<ID3D12DescriptorHeap> srvDescriptorHeap = heaps.srv.GetHeap();
	heaps.srv.Reserve(32);





	uint32_t descriptorSizeSRV = device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	assert(SUCCEEDED(hr));


	constexpr DXGI_FORMAT kRtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	uint64_t fenceValue = engineContext.GetFenceValue();
	HANDLE fenceEvent = engineContext.GetFenceEvent();






	ComPtr<ID3D12Resource> wvpResource =
		CreateBufferResource(device, sizeof(Matrix4x4));

	Matrix4x4* wvpData = nullptr;

	wvpResource->Map(0, nullptr, reinterpret_cast<void**>(&wvpData));

	*wvpData = MakeIdentity4x4();




















	D3D12_HEAP_PROPERTIES uploadHeapProperties{};
	uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC vertexResourceDesc{};

	vertexResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	vertexResourceDesc.Width =
		sizeof(VertexData) * 3;

	vertexResourceDesc.Height = 1;
	vertexResourceDesc.DepthOrArraySize = 1;
	vertexResourceDesc.MipLevels = 1;
	vertexResourceDesc.SampleDesc.Count = 1;
	vertexResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> vertexResource =
		CreateBufferResource(device, sizeof(VertexData) * 6);
	hr = device->CreateCommittedResource(
		&uploadHeapProperties, D3D12_HEAP_FLAG_NONE, &vertexResourceDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&vertexResource));
	assert(SUCCEEDED(hr));

	ComPtr<ID3D12Resource> indexResourceSprite =
		CreateBufferResource(device, sizeof(uint32_t) * 6);

	const UINT stackCount = 16;
	const UINT sliceCount = 32;

	ComPtr<ID3D12Resource> vertexResourceSprite =
		CreateBufferResource(device, sizeof(VertexData) * 6);

	AppSceneResources scene;
	AppPipelines appPipelines;
	if (!appPipelines.Initialize(device.Get())) {
		OutputDebugStringA("[AppMain] AppPipelines initialization failed.\n");
		return 1;
	}
	if (!scene.Initialize(device, srvDescriptorHeap, descriptorSizeSRV)) {
		OutputDebugStringA("[AppMain] AppSceneResources initialization failed.\n");
		return 1;
	}

	AppRuntimeState runtimeState{};
	bool autoNetworkExperiment = false;
	if (char networkModeBuffer[64]{};
		GetEnvironmentVariableA(
			"TR2_NETWORK_MODE",
			networkModeBuffer,
			static_cast<DWORD>(sizeof(networkModeBuffer))) > 0) {
		runtimeState.networkRuntimeMode =
			net::ParseNetworkRuntimeMode(networkModeBuffer);
	}
	if (GetEnvironmentVariableA("TR2_NETWORK_EXPERIMENT_AUTO", nullptr, 0) > 0) {
		runtimeState.networkExperimentMode = true;
		autoNetworkExperiment = true;
	}
	if (char presentSyncBuffer[16]{};
		GetEnvironmentVariableA(
			"RNVP_PRESENT_SYNC_INTERVAL",
			presentSyncBuffer,
			static_cast<DWORD>(sizeof(presentSyncBuffer))) > 0) {
		runtimeState.lowLatencyPresentMode =
			std::atoi(presentSyncBuffer) == 0;
	}
	if (GetEnvironmentVariableA("RNVP_LOW_LATENCY_PRESENT", nullptr, 0) > 0) {
		runtimeState.lowLatencyPresentMode = true;
	}
	if (GetEnvironmentVariableA("RNVP_WAITABLE_SWAPCHAIN", nullptr, 0) > 0) {
		runtimeState.waitableSwapChainPacingEnabled = true;
	}
	if (!net::NetworkModeCanReceiveVideo(runtimeState.networkRuntimeMode)) {
		runtimeState.showReceivedVideoInGame = false;
		runtimeState.showReceivedVideoPreviewWindow = false;
	}
	AppParticleSystem particleSystem;
	runtimeState.transform.scale = { 6.0f, 6.0f, 6.0f };
	runtimeState.transform.rotate = { 0.0f, 0.0f, 0.0f };
	runtimeState.transform.translate = { 0.0f, 0.0f, 0.0f };

	runtimeState.transformSprite.scale = { 360.0f, 203.0f, 1.0f };
	runtimeState.transformSprite.rotate = { 0.0f, 0.0f, 0.0f };

	runtimeState.transformSprite.translate = { 640.0f, 256.0f, 0.0f };

	runtimeState.uvTransformSprite.scale = { 1.0f, 1.0f, 1.0f };
	runtimeState.uvTransformSprite.rotate = { 0.0f, 0.0f, 0.0f };
	runtimeState.uvTransformSprite.translate = { 0.0f, 0.0f, 0.0f };

	runtimeState.viewport.Width = 1280.0f;
	runtimeState.viewport.Height = 720.0f;
	runtimeState.viewport.TopLeftX = 0.0f;
	runtimeState.viewport.TopLeftY = 0.0f;
	runtimeState.viewport.MinDepth = 0.0f;
	runtimeState.viewport.MaxDepth = 1.0f;

	runtimeState.scissorRect.left = 0;
	runtimeState.scissorRect.top = 0;
	runtimeState.scissorRect.right = 1280;
	runtimeState.scissorRect.bottom = 720;

	runtimeState.directionalLightData = scene.directionalLightData;
	runtimeState.pointLightData = scene.pointLightData;
	runtimeState.spotLight = scene.spotLight;
	runtimeState.cameraWorldPosition = scene.mappedCamera->worldPosition;
	runtimeState.materialData = *scene.materialData;

	AppImGuiLayer imguiLayer;
	imguiLayer.Initialize(
		hwnd,
		device.Get(),
		static_cast<int>(swapChain.BufferCount()),
		kRtvFormat,
		srvDescriptorHeap.Get());


	AppRenderResources renderResources;
	assert(renderResources.InitializeParticleQuad(device));
	AppFrameRenderer frameRenderer;






	constexpr uint32_t kInstancingSrvIndex = 10;
	ComPtr<ID3D12Resource> instancingResource =
		particleSystem.CreateInstancingResource(device.Get());
	particleSystem.InitializeInstancingBuffer(
		instancingResource.Get(),
		particleSystem.MaxInstances());

	D3D12_CPU_DESCRIPTOR_HANDLE instancingSrvCPU =

		AppRenderResources::GetCPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, kInstancingSrvIndex);

	D3D12_GPU_DESCRIPTOR_HANDLE instancingSrvGPU =
		AppRenderResources::GetGPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, kInstancingSrvIndex);

	particleSystem.CreateInstancingSrv(
		device.Get(),
		instancingResource.Get(),
		instancingSrvCPU,
		instancingSrvGPU);
	const UINT texWidth = 640;
	const UINT texHeight = 360;
	DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
	ComPtr<ID3D12Resource> texture =
		CreateTextureResourceResolution(device, texWidth, texHeight, format);



	D3D12_CPU_DESCRIPTOR_HANDLE receivedSrvHandleCPU =
		AppRenderResources::GetCPUDescriptorHandle(srvDescriptorHeap, descriptorSizeSRV, 4);
	CreateTextureSRV(device.Get(), texture.Get(),
		receivedSrvHandleCPU);

	D3D12_GPU_DESCRIPTOR_HANDLE receivedSrvHandleGPU =
		AppRenderResources::GetGPUDescriptorHandle(
			srvDescriptorHeap,
			descriptorSizeSRV,
			4
		);

	UINT64 receivedUploadBufferSize = 0;
	{
		D3D12_RESOURCE_DESC textureDesc = texture->GetDesc();

		device->GetCopyableFootprints(
			&textureDesc,
			0,
			1,
			0,
			nullptr,
			nullptr,
			nullptr,
			&receivedUploadBufferSize
		);
	}

	std::vector<ComPtr<ID3D12Resource>> receivedUploadBuffers;
	receivedUploadBuffers.reserve(kReceivedVideoUploadBufferCount);
	{
		D3D12_HEAP_PROPERTIES heapProps{};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC bufferDesc{};
		bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufferDesc.Alignment = 0;
		bufferDesc.Width = receivedUploadBufferSize;
		bufferDesc.Height = 1;
		bufferDesc.DepthOrArraySize = 1;
		bufferDesc.MipLevels = 1;
		bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufferDesc.SampleDesc.Count = 1;
		bufferDesc.SampleDesc.Quality = 0;
		bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

		for (uint32_t i = 0; i < kReceivedVideoUploadBufferCount; ++i) {
			ComPtr<ID3D12Resource> uploadBuffer;
			HRESULT hr = device->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&bufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&uploadBuffer)
			);

			if (FAILED(hr)) {
				OutputDebugStringA("[AppMain] Failed to create receivedUploadBuffer.\n");
				continue;
			}
			receivedUploadBuffers.push_back(std::move(uploadBuffer));
		}
	}

	AppAudio audio;
	audio.Initialize();


	::audio::SoundData soundData1 = audio.LoadWave("Resources/Alarm01.wav");

	//audio.PlayWave(soundData1);
	DebugCamera debugCamera;
	debugCamera.Initialize();




	CpuTransferCtx xferCtx;

	MSG msg{};

	FrameLoopState frameState{};
	AppRunLoop runLoop(
		debugCamera,
		runtimeState,
		scene,
		particleSystem,
		imguiLayer,
		frameRenderer,
		appPipelines,
		renderResources,
		swapChain,
		clPool,
		engineContext,
		heaps,
		dev,
		srvDescriptorHeap,
		wvpData,
		windowWidth,
		windowHeight,
		frameState,
		commandQueue.Get(),
		fence.Get(),
		fenceEvent);
	runLoop.InitializeBeam(
		device.Get(),
		srvDescriptorHeap.Get(),
		descriptorSizeSRV,
		kRtvFormat,
		DXGI_FORMAT_D24_UNORM_S8_UINT);

	// RNVP local loopback pipeline for realtime streaming experiments.
	auto udpReceiver = std::make_unique<net::UdpReceiver>();

	auto networkManager = std::make_unique<NetworkManager>(
		kRnvpRemoteIp,
		kRnvpRemotePort
	);
	{
		auto readEnvDouble = [](const char* name, double fallback) {
			char buffer[64]{};
			const DWORD length = GetEnvironmentVariableA(
				name,
				buffer,
				static_cast<DWORD>(sizeof(buffer)));
			if (length == 0 || length >= sizeof(buffer)) {
				return fallback;
			}
			char* end = nullptr;
			const double value = std::strtod(buffer, &end);
			return end != buffer ? value : fallback;
			};
		auto readEnvUint = [](const char* name, uint32_t fallback) {
			char buffer[64]{};
			const DWORD length = GetEnvironmentVariableA(
				name,
				buffer,
				static_cast<DWORD>(sizeof(buffer)));
			if (length == 0 || length >= sizeof(buffer)) {
				return fallback;
			}
			char* end = nullptr;
			const unsigned long value = std::strtoul(buffer, &end, 10);
			return end != buffer ? static_cast<uint32_t>(value) : fallback;
			};
		const bool simEnvEnabled =
			GetEnvironmentVariableA("RNVP_SIM_ENABLED", nullptr, 0) > 0;
		if (simEnvEnabled && networkManager) {
			net::NetworkCondition condition{};
			condition.enabled = true;
			condition.lossRate =
				readEnvDouble("RNVP_SIM_LOSS_PERCENT", 0.0) / 100.0;
			condition.duplicateRate =
				readEnvDouble("RNVP_SIM_DUPLICATE_PERCENT", 0.0) / 100.0;
			condition.reorderRate =
				readEnvDouble("RNVP_SIM_REORDER_PERCENT", 0.0) / 100.0;
			condition.minDelayMs =
				readEnvUint("RNVP_SIM_MIN_DELAY_MS", 0);
			condition.maxDelayMs =
				readEnvUint("RNVP_SIM_MAX_DELAY_MS", condition.minDelayMs);
			condition.burstLossLength =
				readEnvUint("RNVP_SIM_BURST_LOSS_LENGTH", 0);
			networkManager->SetNetworkCondition(condition);
			OutputDebugStringA("[AppMain] RNVP network simulation enabled from environment.\n");
		}
	}

	auto adaptiveController = std::make_unique<net::AdaptiveStreamingController>();
	adaptiveController->SetControlMode(net::AdaptiveControlMode::FixedQuality);
	{
		std::string adaptiveModeEnv;
		if (TryReadEnvString("RNVP_ADAPTIVE_CONTROL_MODE", adaptiveModeEnv)) {
			adaptiveController->SetControlMode(
				ParseAdaptiveControlModeEnv(
					adaptiveModeEnv,
					net::AdaptiveControlMode::FixedQuality));
		}

		std::string congestionModeEnv;
		if (TryReadEnvString("RNVP_CONGESTION_CONTROL_MODE", congestionModeEnv)) {
			adaptiveController->SetCongestionControlMode(
				ParseCongestionControlModeEnv(
					congestionModeEnv,
					net::CongestionControlMode::Hybrid));
		}
	}
	std::mutex adaptiveControllerMutex;
	net::NetworkExperimentRunner networkExperimentRunner;
	networkExperimentRunner.SetStopAfterOnePass(autoNetworkExperiment);
	struct NetworkSendTelemetry {
		std::mutex mutex;
		double captureFps = 0.0;
		double encodeMs = 0.0;
		double sendFrameIntervalMs = 0.0;
		bool cameraFrameReady = false;
		uint64_t cameraReadyFrames = 0;
		uint64_t cameraReadyFramesAtLastFpsUpdate = 0;
		std::chrono::steady_clock::time_point lastCaptureFpsUpdate =
			std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point lastSendFrameTime{};
		bool hasLastSendFrameTime = false;
	};
	NetworkSendTelemetry sendTelemetry;
	net::NetworkVideoReceiver networkVideoReceiver;

	if (net::NetworkModeCanReceiveVideo(runtimeState.networkRuntimeMode)) {
		if (udpReceiver->Start(kRnvpListenPort)) {
			std::cout << "[AppMain] UdpReceiver started. port="
				<< kRnvpListenPort << "\n";
		}
		else {
			std::cerr << "[AppMain] Failed to start UdpReceiver. port="
				<< kRnvpListenPort << "\n";
		}
	}
	else {
		std::cout << "[AppMain] UdpReceiver disabled for network mode="
			<< net::ToString(runtimeState.networkRuntimeMode) << "\n";
	}

	if (networkManager) {
		if (networkManager->StartRNVPControlReceiver()) {
			std::cout << "[AppMain] NetworkManager RNVP control receiver started.\n";
		}
		else {
			std::cerr << "[AppMain] Failed to start NetworkManager RNVP control receiver.\n";
		}
	}

	// Merge receiver, sender, simulator, and adaptive streaming state for monitoring and CSV logging.
	auto collectNetworkStats =
		[
			receiver = udpReceiver.get(),
			sender = networkManager.get(),
			adaptive = adaptiveController.get(),
			adaptiveMutex = &adaptiveControllerMutex,
			experiment = &networkExperimentRunner,
			runtimeState = &runtimeState,
			sendTelemetry = &sendTelemetry,
			videoReceiver = &networkVideoReceiver,
			runLoop = &runLoop
		]() {
		net::NetworkStatsSnapshot stats{};

		if (receiver && receiver->IsRunning()) {
			stats = receiver->GetStats();
		}

		if (sender) {
			stats.ackCount = sender->GetAckCount();
			stats.lastAckFrameId = sender->GetLastAckFrameId();
			stats.lastAckReceivedChunks = sender->GetLastAckReceivedChunks();
			stats.lastAckMissingChunks = sender->GetLastAckMissingChunks();
			stats.lastAckMissingRate = sender->GetLastAckMissingRate();
			stats.ackRetransmittedFrames = sender->GetAckRetransmittedFrameCount();
			stats.ackRetransmittedChunks = sender->GetAckRetransmittedChunkCount();
			stats.ackStaleDroppedFrames = sender->GetAckStaleDroppedFrameCount();
			stats.ackKeyFrameRequests = sender->GetAckKeyFrameRequestCount();
			stats.ackKeyFramePending = sender->IsKeyFrameRequestPending();
			stats.fecEnabled = sender->IsFecEnabled();
			stats.adaptiveFecEnabled = sender->IsAdaptiveFecEnabled();
			stats.fecGroupChunkCount = sender->GetFecGroupChunkCount();

			const net::PacketPacerStats pacingStats =
				sender->GetPacingStats();
			stats.pacingEnabled = pacingStats.enabled;
			stats.pacingTargetBitrateBps = pacingStats.targetBitrateBps;
			stats.pacingQueuedPackets = pacingStats.queuedPackets;
			stats.pacingHighPriorityQueuedPackets =
				pacingStats.highPriorityQueuedPackets;
			stats.pacingNormalQueuedPackets =
				pacingStats.normalQueuedPackets;
			stats.pacingEnqueuedPackets = pacingStats.enqueuedPackets;
			stats.pacingSentPackets = pacingStats.sentPackets;
			stats.pacingSentBytes = pacingStats.sentBytes;
			stats.pacingDroppedPackets = pacingStats.droppedPackets;
			stats.pacingDeadlineDroppedPackets =
				pacingStats.deadlineDroppedPackets;
			stats.pacingOverflowDroppedPackets =
				pacingStats.overflowDroppedPackets;
			stats.pacingCurrentQueueDelayMs =
				pacingStats.currentQueueDelayMs;
			stats.pacingMaxQueueDelayMs = pacingStats.maxQueueDelayMs;

			const NetworkManager::TransportFeedbackStats feedbackStats =
				sender->GetTransportFeedbackStats();
			stats.transportFeedbackPackets =
				feedbackStats.feedbackPackets;
			stats.transportFeedbackPacketStatuses =
				feedbackStats.feedbackPacketStatuses;
			stats.transportFeedbackReceivedPackets =
				feedbackStats.feedbackReceivedPackets;
			stats.transportFeedbackMissingPackets =
				feedbackStats.feedbackMissingPackets;
			stats.transportFeedbackLossRate =
				feedbackStats.feedbackLossRate;
			stats.transportFeedbackArrivalJitterMs =
				feedbackStats.feedbackArrivalJitterMs;
			stats.transportFeedbackQueueDelayTrendMs =
				feedbackStats.feedbackQueueDelayTrendMs;
			stats.transportFeedbackLastSequence =
				feedbackStats.lastFeedbackSequence;

			const net::BandwidthEstimatorStats bandwidthStats =
				sender->GetBandwidthEstimatorStats();
			stats.estimatedBandwidthBps =
				bandwidthStats.estimatedBandwidthBps;
			stats.deliveryRateBps =
				bandwidthStats.deliveryRateBps;
			stats.bandwidthQueueDelayMs =
				bandwidthStats.queueDelayMs;
			stats.bandwidthRttTrendMs =
				bandwidthStats.rttTrendMs;
			stats.bandwidthLossTrend =
				bandwidthStats.lossTrend;
			stats.bandwidthJitterTrendMs =
				bandwidthStats.jitterTrendMs;
			stats.bandwidthFeedbackSamples =
				bandwidthStats.feedbackSamples;

			stats.currentRttMs = sender->GetLastRttMs();
			stats.averageRttMs = sender->GetAverageRttMs();

			stats.maxRttMs = sender->GetMaxRttMs();

			stats.rttSamples = sender->GetRttSampleCount();

			stats.networkCondition = sender->GetNetworkCondition();
			stats.networkSimulation = sender->GetNetworkSimulationStats();
		}

		if (adaptive) {
			std::lock_guard<std::mutex> adaptiveLock(*adaptiveMutex);
			const net::AdaptiveStreamingState adaptiveState =
				adaptive->GetState();

			stats.adaptiveEnabled = adaptive->IsEnabled();
			stats.adaptiveControlMode =
				net::ToString(adaptiveState.controlMode);
			stats.adaptiveCongestionControlMode =
				net::ToString(adaptiveState.congestionControlMode);

			stats.adaptiveTargetJpegQuality =
				adaptiveState.targetJpegQuality;

			stats.adaptiveTargetFps =
				adaptiveState.targetFps;

			stats.adaptiveTargetBitrateKbps =
				adaptiveState.targetBitrateKbps;

			stats.adaptiveBandwidthCeilingKbps =
				adaptiveState.bandwidthCeilingKbps;

			stats.adaptiveTargetWidth =
				adaptiveState.targetWidth;

			stats.adaptiveTargetHeight =
				adaptiveState.targetHeight;

			stats.adaptiveRawFrameBytes =
				static_cast<uint64_t>(adaptiveState.lastRawFrameBytes);

			stats.adaptiveEncodedFrameBytes =
				static_cast<uint64_t>(adaptiveState.lastEncodedFrameBytes);

			stats.adaptiveCompressionRatio =
				adaptiveState.lastCompressionRatio;

			if (sendTelemetry) {
				std::lock_guard<std::mutex> sendLock(
					sendTelemetry->mutex);
				stats.captureFps = sendTelemetry->captureFps;
				stats.encodeMs = sendTelemetry->encodeMs;
				stats.sendFrameIntervalMs =
					sendTelemetry->sendFrameIntervalMs;
				stats.cameraFrameReady =
					sendTelemetry->cameraFrameReady;
			}

			stats.adaptiveQualityChanged =
				adaptiveState.qualityChanged;

			stats.adaptiveFpsChanged =
				adaptiveState.fpsChanged;

			stats.adaptiveBitrateChanged =
				adaptiveState.bitrateChanged;

			stats.adaptiveResolutionChanged =
				adaptiveState.resolutionChanged;

			stats.adaptiveLastAckMissingRate =
				adaptiveState.lastAckMissingRate;

			stats.adaptiveLastPacketLossRate =
				adaptiveState.lastPacketLossRate;

			stats.adaptiveLastRttMs =
				adaptiveState.lastRttMs;

			stats.adaptiveLastLatencyMs =
				adaptiveState.lastLatencyMs;

			stats.adaptiveLastJitterMs =
				adaptiveState.lastJitterMs;

			stats.adaptiveLastReceiveFps =
				adaptiveState.lastReceiveFps;

			stats.adaptiveLastDecodeFps =
				adaptiveState.lastDecodeFps;

			stats.adaptiveLastDisplayFps =
				adaptiveState.lastDisplayFps;

			stats.adaptiveLastQoeScore =
				adaptiveState.lastQoeScore;

			stats.adaptiveDegradationCause =
				net::ToString(adaptiveState.lastDegradationCause);

			stats.adaptiveFecRecoveryWorking =
				adaptiveState.lastFecRecoveryWorking;
			stats.adaptiveFecGuardActive =
				adaptiveState.lastFecRecoveryGuardActive &&
				adaptive->IsEnabled() &&
				adaptiveState.controlMode ==
				net::AdaptiveControlMode::QoeDeadlineAdaptive;
			stats.adaptiveFecRecoveryEfficiency =
				adaptiveState.lastFecRecoveryEfficiency;
			stats.adaptiveFecParityPacketDelta =
				adaptiveState.lastFecParityPacketDelta;
			stats.adaptiveFecRecoveredFrameDelta =
				adaptiveState.lastFecRecoveredFrameDelta;
			stats.adaptiveFecRecoveredChunkDelta =
				adaptiveState.lastFecRecoveredChunkDelta;
		}

		if (experiment) {
			stats.networkExperimentActive = experiment->IsActive();
			stats.networkExperimentScenarioName =
				experiment->IsActive()
				? experiment->CurrentScenarioName()
				: "";
			stats.networkExperimentAdaptiveMode =
				experiment->IsActive()
				? std::string(net::ToString(
					experiment->CurrentAdaptiveControlMode())) +
					" / " +
					net::ToString(experiment->CurrentCongestionControlMode())
				: "";
			stats.networkExperimentRemainingSec =
				experiment->IsActive()
				? experiment->RemainingSec()
				: 0.0;
			stats.networkExperimentStepIndex =
				static_cast<uint32_t>(
					experiment->IsActive()
					? experiment->CurrentIndex() + 1
					: 0);
			stats.networkExperimentStepCount =
				static_cast<uint32_t>(experiment->ScenarioCount());
		}

		if (runtimeState) {
			stats.networkRuntimeMode = runtimeState->networkRuntimeMode;
			stats.networkRuntimeModeName =
				net::ToString(runtimeState->networkRuntimeMode);
			stats.networkModeSendingEnabled =
				net::NetworkModeCanSendVideo(runtimeState->networkRuntimeMode);
			stats.networkModeReceivingEnabled =
				net::NetworkModeCanReceiveVideo(runtimeState->networkRuntimeMode);
		}

		if (runLoop) {
			runLoop->PopulateNetworkRenderTimings(stats);
		}

		if (videoReceiver) {
			const net::NetworkVideoReceiverStats videoReceiverStats =
				videoReceiver->GetStats();
			stats.receiveJpegDecodeMs =
				videoReceiverStats.jpegDecodeMs;
			stats.receiveDecodeWorkerFps =
				videoReceiverStats.decodeWorkerFps;
			stats.receiveDecodeWorkerFrames =
				videoReceiverStats.decodedFrames;
			stats.receiveDecodeOverwrittenFrames =
				videoReceiverStats.overwrittenFrames;
			stats.receiveDecodeQueueDroppedFrames =
				videoReceiverStats.decodeQueueDroppedFrames;
			stats.receiveDecodeRenderOverwriteFrames =
				videoReceiverStats.decodeRenderOverwriteFrames;
			stats.receiveDecodeFailures =
				videoReceiverStats.decodeFailures;
			stats.receiveFreshnessDroppedFrames =
				videoReceiverStats.freshnessDroppedFrames;
			stats.receiveDecodeInputFrameAgeMs =
				videoReceiverStats.decodeInputFrameAgeMs;
			stats.receiveLatestDecodedFrameAgeMs =
				videoReceiverStats.latestDecodedFrameAgeMs;
			stats.receiveFreshnessDropThresholdMs =
				videoReceiverStats.freshnessDropThresholdMs;
			stats.receiveDecodeLastDropReason =
				videoReceiverStats.lastDropReason;
		}

		return stats;
		};

	runLoop.SetNetworkStatsProvider(collectNetworkStats);

	net::NetworkCsvLogger networkCsvLogger;
	if (networkCsvLogger.Start("logs")) {
		std::cout << "[AppMain] Network CSV logging started: "
			<< networkCsvLogger.FilePath()
			<< "\n";
	}
	else {
		std::cerr << "[AppMain] Network CSV logging failed to start.\n";
	}
	const auto networkCsvStartTime = std::chrono::steady_clock::now();
	auto lastNetworkCsvSampleTime = networkCsvStartTime - std::chrono::seconds(1);
	net::NetworkExperimentReporter networkExperimentReporter;
	if (networkExperimentReporter.Start("logs")) {
		std::cout << "[AppMain] Network experiment summary started: "
			<< networkExperimentReporter.CsvFilePath()
			<< " report: "
			<< networkExperimentReporter.MarkdownFilePath()
			<< " before/after: "
			<< networkExperimentReporter.BeforeAfterFilePath()
			<< " repeat: "
			<< networkExperimentReporter.RepeatReportFilePath()
			<< " manifest: "
			<< networkExperimentReporter.ManifestFilePath()
			<< "\n";
		networkExperimentReporter.WriteManifest(
			networkExperimentRunner.Scenarios(),
			networkCsvLogger.FilePath(),
			runtimeState.networkRuntimeMode,
			autoNetworkExperiment,
			autoNetworkExperiment,
			networkExperimentRunner.WarmupSec());
	}
	else {
		std::cerr << "[AppMain] Network experiment summary failed to start.\n";
	}
	auto lastNetworkExperimentUpdateTime = networkCsvStartTime;

	runLoop.SetJitterBufferTargetDelaySetter(
		[
			receiver = udpReceiver.get()
		](uint32_t delayMs) {
			if (receiver) {
				receiver->SetJitterBufferTargetDelayMs(delayMs);
			}
		}
				);

	runLoop.SetJitterBufferAutoModeSetter(
		[
			receiver = udpReceiver.get()
		](bool enabled) {
			if (receiver) {
				receiver->SetJitterBufferAutoModeEnabled(enabled);
			}
		}
				);

	runLoop.SetNetworkConditionSetter(
		[
			sender = networkManager.get()
		](const net::NetworkCondition& condition) {
			if (sender) {
				sender->SetNetworkCondition(condition);
			}
		}
				);

	runLoop.SetAdaptiveControlModeSetter(
		[
			adaptive = adaptiveController.get(),
			adaptiveMutex = &adaptiveControllerMutex
		](int modeIndex) {
		if (!adaptive) {
			return;
		}

		const int clampedMode =
			std::clamp(modeIndex, 0, 2);
		std::lock_guard<std::mutex> lock(*adaptiveMutex);
		adaptive->SetControlMode(
			static_cast<net::AdaptiveControlMode>(clampedMode));
	}
				);
	runLoop.SetCongestionControlModeSetter(
		[
			adaptive = adaptiveController.get(),
			adaptiveMutex = &adaptiveControllerMutex
		](int modeIndex) {
		if (!adaptive) {
			return;
		}

		const int clampedMode =
			std::clamp(modeIndex, 0, 2);
		std::lock_guard<std::mutex> lock(*adaptiveMutex);
		adaptive->SetCongestionControlMode(
			static_cast<net::CongestionControlMode>(clampedMode));
	}
				);
	runLoop.SetReceivedFrameProvider(
		[
			videoReceiver = &networkVideoReceiver,
			runtimeState = &runtimeState
		](net::DecodedVideoFrame& outFrame) {
			if (!runtimeState ||
				!net::NetworkModeCanReceiveVideo(
					runtimeState->networkRuntimeMode)) {
				return false;
			}

			if (!videoReceiver) {
				return false;
			}

			return videoReceiver->TryGetLatestFrame(outFrame);
		}
				);

	runLoop.SetNetworkFrameDisplayNotifier(
		[
			receiver = udpReceiver.get()
		]() {
			if (receiver) {
				receiver->NotifyDisplayFrame();
			}
		}
				);

	runLoop.SetReceivedVideoTexture(
		texture,
		std::move(receivedUploadBuffers),
		receivedSrvHandleGPU,
		texWidth,
		texHeight
	);

	networkVideoReceiver.Start(
		udpReceiver.get(),
		[&runtimeState]() {
			return net::NetworkModeCanReceiveVideo(
				runtimeState.networkRuntimeMode);
		});

	// Prefer a live camera frame; fallback frames keep the network path testable without a camera.
	auto cameraCapture = std::make_unique<CameraCapture>();
	const bool forceDisableCamera =
		GetEnvironmentVariableA("RNVP_DISABLE_CAMERA", nullptr, 0) > 0;
	bool cameraCaptureEnabled =
		!forceDisableCamera && cameraCapture->Initialize(texWidth, texHeight);
	if (cameraCaptureEnabled) {
		OutputDebugStringA("[AppMain] Camera capture enabled for RNVP raw video.\n");
		cameraCapture->StartAsyncCapture();
	}
	else if (forceDisableCamera) {
		OutputDebugStringA("[AppMain] Camera capture disabled by environment; RNVP uses generated test video.\n");
	}
	else {
		OutputDebugStringA("[AppMain] Camera capture unavailable; RNVP falls back to generated test video.\n");
	}

	std::atomic<bool> videoSenderRunning{ true };
	std::thread videoSenderThread(
		[
			&videoSenderRunning,
			networkManager = networkManager.get(),
			adaptiveController = adaptiveController.get(),
			adaptiveMutex = &adaptiveControllerMutex,
			cameraCapture = cameraCapture.get(),
			cameraCaptureEnabled,
			&sendTelemetry,
			&runtimeState,
			texWidth,
			texHeight
		]() {
		auto nextSendTime = std::chrono::steady_clock::now();
		uint32_t frameId = 1;
		uint64_t lastCameraFrameId = 0;
		std::vector<uint8_t> lastCameraFrameCache;

		while (videoSenderRunning.load()) {
			const auto now = std::chrono::steady_clock::now();
			if (now < nextSendTime) {
				std::this_thread::sleep_until(nextSendTime);
				continue;
			}

			if (!networkManager ||
				!adaptiveController ||
				!net::NetworkModeCanSendVideo(
					runtimeState.networkRuntimeMode)) {
				nextSendTime =
					std::chrono::steady_clock::now() +
					std::chrono::milliseconds(10);
				continue;
			}

			net::AdaptiveStreamingState adaptiveState{};
			{
				std::lock_guard<std::mutex> lock(*adaptiveMutex);
				adaptiveState = adaptiveController->GetState();
			}

			int targetFps = adaptiveState.targetFps;
			targetFps = std::clamp(targetFps, 1, 30);

			uint32_t pacingTargetBitrateKbps =
				static_cast<uint32_t>(
					(std::max)(1, adaptiveState.targetBitrateKbps)
				);
			const bool stableBaselineMode =
				runtimeState.networkRuntimeMode ==
				net::NetworkRuntimeMode::Loopback &&
				net::NetworkModeCanReceiveVideo(
					runtimeState.networkRuntimeMode) &&
				!runtimeState.networkExperimentMode &&
				!networkManager->GetNetworkCondition().enabled;
			if (stableBaselineMode) {
				pacingTargetBitrateKbps =
					(std::max)(
						pacingTargetBitrateKbps,
						uint32_t{ 50000 }
					);
			}
			networkManager->SetPacingTargetBitrateKbps(
				pacingTargetBitrateKbps
			);

			const uint32_t targetWidth =
				static_cast<uint32_t>(
					std::clamp(
						adaptiveState.targetWidth,
						160,
						static_cast<int>(texWidth)
					)
				);
			const uint32_t targetHeight =
				static_cast<uint32_t>(
					std::clamp(
						adaptiveState.targetHeight,
						90,
						static_cast<int>(texHeight)
					)
				);

			const auto sendFrameStartTime =
				std::chrono::steady_clock::now();
			{
				std::lock_guard<std::mutex> lock(sendTelemetry.mutex);
				if (sendTelemetry.hasLastSendFrameTime) {
					sendTelemetry.sendFrameIntervalMs =
						std::chrono::duration<double, std::milli>(
							sendFrameStartTime -
							sendTelemetry.lastSendFrameTime
						).count();
				}
				sendTelemetry.lastSendFrameTime = sendFrameStartTime;
				sendTelemetry.hasLastSendFrameTime = true;
			}

			std::vector<uint8_t> videoFrame;
			uint64_t cameraFrameId = 0;
			const bool hasCameraFrame =
				cameraCaptureEnabled &&
				cameraCapture &&
				cameraCapture->TryGetLatestRgbaFrame(
					videoFrame,
					cameraFrameId
				);
			const bool freshCameraFrame =
				hasCameraFrame &&
				cameraFrameId != lastCameraFrameId;
			if (hasCameraFrame) {
				lastCameraFrameId = cameraFrameId;
				lastCameraFrameCache = videoFrame;
			}

			{
				std::lock_guard<std::mutex> lock(sendTelemetry.mutex);
				sendTelemetry.cameraFrameReady = freshCameraFrame;
				sendTelemetry.captureFps =
					cameraCaptureEnabled && cameraCapture
					? cameraCapture->GetAsyncCaptureFps()
					: 0.0;
			}

			bool usedCameraCache = false;
			if (!hasCameraFrame &&
				cameraCaptureEnabled &&
				!lastCameraFrameCache.empty()) {
				videoFrame = lastCameraFrameCache;
				usedCameraCache = true;
			}
			else if (!hasCameraFrame && cameraCaptureEnabled) {
				const auto sendInterval =
					std::chrono::duration_cast<
						std::chrono::steady_clock::duration>(
							std::chrono::duration<double>(
								1.0 / static_cast<double>(targetFps)
							));
				nextSendTime += sendInterval;
				continue;
			}

			if (!hasCameraFrame && !usedCameraCache) {
				videoFrame.resize(
					static_cast<size_t>(texWidth) *
					static_cast<size_t>(texHeight) *
					4u
				);

				for (UINT y = 0; y < texHeight; ++y) {
					for (UINT x = 0; x < texWidth; ++x) {
						const size_t index =
							(static_cast<size_t>(y) *
								static_cast<size_t>(texWidth) +
								static_cast<size_t>(x)) * 4u;

						const uint8_t r =
							static_cast<uint8_t>((x + frameId * 3u) & 0xFF);
						const uint8_t g =
							static_cast<uint8_t>((y + frameId * 2u) & 0xFF);
						const uint8_t b =
							static_cast<uint8_t>(
								((x / 16u) ^ (y / 16u) ^ frameId) & 0xFF);

						videoFrame[index + 0] = r;
						videoFrame[index + 1] = g;
						videoFrame[index + 2] = b;
						videoFrame[index + 3] = 255;
					}
				}
			}

			std::vector<uint8_t> adaptiveVideoFrame =
				ResizeRgbaBilinear(
					videoFrame,
					texWidth,
					texHeight,
					targetWidth,
					targetHeight
				);

			const auto encodeStartTime =
				std::chrono::steady_clock::now();
			std::vector<uint8_t> encodedPayload =
				EncodeJpegFrame(
					adaptiveVideoFrame,
					targetWidth,
					targetHeight,
					adaptiveState.targetJpegQuality
				);
			const double encodeMs =
				std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() -
					encodeStartTime
				).count();

			{
				std::lock_guard<std::mutex> lock(sendTelemetry.mutex);
				sendTelemetry.encodeMs = encodeMs;
			}

			net::CodecType sendCodec = net::CodecType::MJPEG;
			if (encodedPayload.empty()) {
				encodedPayload =
					PackRawRgbaPayload(videoFrame, texWidth, texHeight);
				sendCodec = net::CodecType::Raw;
			}

			const bool requestedKeyFrame =
				networkManager->ConsumeKeyFrameRequest();
			const bool sendAsKeyFrame =
				requestedKeyFrame ||
				sendCodec == net::CodecType::MJPEG;

			{
				std::lock_guard<std::mutex> lock(*adaptiveMutex);
				adaptiveController->ReportEncodedFrame(
					adaptiveVideoFrame.size(),
					encodedPayload.size()
				);
			}

			networkManager->SendRNVPFragmented(
				encodedPayload,
				frameId,
				sendCodec,
				1,
				sendAsKeyFrame
			);

			frameId++;

			if ((frameId % 60) == 0) {
				std::ostringstream oss;
				oss << "[AppMain] RNVP video frame sent. frameId="
					<< frameId
					<< " targetFps="
					<< targetFps
					<< " targetBitrateKbps="
					<< adaptiveState.targetBitrateKbps
					<< " targetQuality="
					<< adaptiveState.targetJpegQuality
					<< " targetResolution="
					<< targetWidth
					<< "x"
					<< targetHeight
					<< " codec="
					<< (sendCodec == net::CodecType::MJPEG ? "mjpeg" : "raw")
					<< " source="
					<< (hasCameraFrame
						? (freshCameraFrame ? "camera" : "camera-cache")
						: (usedCameraCache ? "camera-hold" : "fallback"))
					<< " size="
					<< encodedPayload.size()
					<< " rawSize="
					<< adaptiveVideoFrame.size()
					<< " keyFrame="
					<< (sendAsKeyFrame ? "true" : "false")
					<< " requestedKeyFrame="
					<< (requestedKeyFrame ? "true" : "false");

				OutputDebugStringA(oss.str().c_str());
				OutputDebugStringA("\n");
			}

			const auto sendInterval =
				std::chrono::duration<double>(
					1.0 / static_cast<double>(targetFps)
				);
			nextSendTime +=
				std::chrono::duration_cast<
					std::chrono::steady_clock::duration
				>(sendInterval);

			const auto scheduleNow = std::chrono::steady_clock::now();
			if (nextSendTime < scheduleNow - std::chrono::milliseconds(100)) {
				nextSendTime =
					scheduleNow +
					std::chrono::duration_cast<
						std::chrono::steady_clock::duration
					>(sendInterval);
			}
		}
	});

	bool receiverRuntimeActive =
		udpReceiver &&
		udpReceiver->IsRunning();
	bool autoNetworkExperimentShutdownRequested = false;

	while (msg.message != WM_QUIT) {
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else {
			static auto lastPingTime = std::chrono::steady_clock::now();
			const auto now = std::chrono::steady_clock::now();
			const net::NetworkRuntimeMode networkRuntimeMode =
				runtimeState.networkRuntimeMode;
			const bool networkSendEnabled =
				net::NetworkModeCanSendVideo(networkRuntimeMode);
			const bool networkReceiveEnabled =
				net::NetworkModeCanReceiveVideo(networkRuntimeMode);
			const bool networkExperimentEnabled =
				runtimeState.networkExperimentMode &&
				net::NetworkModeCanRunExperiment(networkRuntimeMode);
			const bool receiverShouldRun = networkReceiveEnabled;

			if (udpReceiver &&
				receiverShouldRun != receiverRuntimeActive) {
				if (receiverShouldRun) {
					if (udpReceiver->Start(kRnvpListenPort)) {
						receiverRuntimeActive = true;
						udpReceiver->ResetStats();
						OutputDebugStringA("[AppMain] UdpReceiver enabled by network mode.\n");
					}
					else {
						OutputDebugStringA("[AppMain] Failed to enable UdpReceiver by network mode.\n");
					}
				}
				else {
					udpReceiver->Stop();
					udpReceiver->ResetStats();
					receiverRuntimeActive = false;
					OutputDebugStringA("[AppMain] UdpReceiver disabled by network mode.\n");
				}
			}

			if (networkManager && networkSendEnabled) {
				networkManager->FlushNetworkSimulator();
			}

			const double networkExperimentDeltaSec =
				std::chrono::duration<double>(
					now - lastNetworkExperimentUpdateTime
				).count();
			lastNetworkExperimentUpdateTime = now;

			const bool experimentScenarioChanged =
				networkExperimentRunner.Update(
					networkExperimentEnabled,
					networkExperimentDeltaSec);

			if (networkExperimentRunner.IsActive() && networkSendEnabled) {
				if (networkManager) {
					networkManager->SetNetworkCondition(
						networkExperimentRunner.CurrentCondition());
					networkManager->SetAdaptiveFecEnabled(
						networkExperimentRunner.CurrentAdaptiveFecEnabled());
					if (!networkExperimentRunner.CurrentAdaptiveFecEnabled()) {
						networkManager->SetFecEnabled(
							networkExperimentRunner.CurrentFecEnabled());
						networkManager->SetFecGroupChunkCount(
							networkExperimentRunner.CurrentFecGroupChunkCount());
					}
					else if (experimentScenarioChanged) {
						networkManager->SetFecEnabled(
							networkExperimentRunner.CurrentFecEnabled());
						networkManager->SetFecGroupChunkCount(
							networkExperimentRunner.CurrentFecGroupChunkCount());
					}
				}
				if (adaptiveController) {
					std::lock_guard<std::mutex> lock(
						adaptiveControllerMutex);
					adaptiveController->SetControlMode(
						networkExperimentRunner.CurrentAdaptiveControlMode());
					adaptiveController->SetCongestionControlMode(
						networkExperimentRunner.CurrentCongestionControlMode());
				}

				networkCsvLogger.SetScenarioName(
					networkExperimentRunner.CurrentScenarioName());
			}
			else {
				networkCsvLogger.SetScenarioName("Auto");
				if (experimentScenarioChanged &&
					networkManager &&
					networkSendEnabled) {
					networkManager->SetNetworkCondition(net::NetworkCondition{});
					networkManager->SetAdaptiveFecEnabled(false);
					networkManager->SetFecEnabled(true);
					networkManager->SetFecGroupChunkCount(4);
				}
				if (experimentScenarioChanged &&
					adaptiveController) {
					std::lock_guard<std::mutex> lock(
						adaptiveControllerMutex);
					adaptiveController->SetControlMode(
						net::AdaptiveControlMode::FixedQuality);
					adaptiveController->SetCongestionControlMode(
						net::CongestionControlMode::Hybrid);
					adaptiveController->Reset();
				}
			}

			if (experimentScenarioChanged) {
				if (networkExperimentRunner.IsActive()) {
					if (adaptiveController) {
						std::lock_guard<std::mutex> lock(
							adaptiveControllerMutex);
						adaptiveController->Reset();
					}
					if (networkManager) {
						networkManager->ResetStats();
					}
					if (udpReceiver) {
						udpReceiver->ResetStats();
					}
				}

				std::ostringstream oss;
				if (networkExperimentRunner.IsActive()) {
					oss << "[AppMain] Network experiment scenario: "
						<< networkExperimentRunner.CurrentScenarioName()
						<< " step="
						<< (networkExperimentRunner.CurrentIndex() + 1)
						<< "/"
						<< networkExperimentRunner.ScenarioCount()
						<< " mode="
						<< net::ToString(
							networkExperimentRunner.CurrentAdaptiveControlMode())
						<< " congestionMode="
						<< net::ToString(
							networkExperimentRunner.CurrentCongestionControlMode())
						<< " fec="
						<< (networkExperimentRunner.CurrentAdaptiveFecEnabled()
							? "adaptive"
							: (networkExperimentRunner.CurrentFecEnabled()
								? "on"
								: "off"))
						<< " fecGroup="
						<< networkExperimentRunner.CurrentFecGroupChunkCount()
						<< " remainingSec="
						<< networkExperimentRunner.RemainingSec();
				}
				else {
					oss << "[AppMain] Network experiment stopped.";
				}

				OutputDebugStringA(oss.str().c_str());
				OutputDebugStringA("\n");
				std::cout << oss.str() << "\n";

				if (!networkExperimentRunner.IsActive() &&
					networkExperimentReporter.IsRunning()) {
					networkExperimentReporter.Stop();
				}

				if (autoNetworkExperiment &&
					networkExperimentRunner.IsCompleted() &&
					!autoNetworkExperimentShutdownRequested) {
					autoNetworkExperimentShutdownRequested = true;
					if (networkExperimentReporter.IsRunning()) {
						networkExperimentReporter.Stop();
					}
					if (networkCsvLogger.IsRunning()) {
						networkCsvLogger.Stop();
					}

					const char* shutdownMessage =
						"[AppMain] Auto network experiment completed; "
						"flushed logs and requested application shutdown.";
					OutputDebugStringA(shutdownMessage);
					OutputDebugStringA("\n");
					std::cout << shutdownMessage << "\n";
					PostQuitMessage(0);
					continue;
				}
			}

			if (networkManager &&
				networkSendEnabled &&
				std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPingTime).count() >= 1000) {
				networkManager->SendRNVPPing(1);
				lastPingTime = now;
			}

			// The network video sender runs on its own worker thread so camera
			// capture and JPEG encode cannot stall the render loop.

			// AIMD target bitrate updates consume ACK loss, packet loss, RTT, and latency.
			static auto lastAdaptiveUpdateTime = std::chrono::steady_clock::now();

			if (adaptiveController && networkManager && udpReceiver) {
				const auto adaptiveNow = std::chrono::steady_clock::now();

				const double deltaTimeSec =
					std::chrono::duration<double>(adaptiveNow - lastAdaptiveUpdateTime).count();

				lastAdaptiveUpdateTime = adaptiveNow;

				const net::NetworkStatsSnapshot receiverStats =
					networkReceiveEnabled
					? udpReceiver->GetStats()
					: net::NetworkStatsSnapshot{};
				const net::NetworkVideoReceiverStats videoReceiverStats =
					networkReceiveEnabled
					? networkVideoReceiver.GetStats()
					: net::NetworkVideoReceiverStats{};

				net::AdaptiveStreamingInput adaptiveInput{};
				adaptiveInput.ackMissingRate = networkManager->GetLastAckMissingRate();
				adaptiveInput.packetLossRate = receiverStats.packetLossRate;
				adaptiveInput.rttMs = networkManager->GetLastRttMs();
				adaptiveInput.latencyMs = receiverStats.currentLatencyMs;
				adaptiveInput.jitterMs = receiverStats.currentJitterMs;
				adaptiveInput.receiveFps = receiverStats.receiveFps;
				adaptiveInput.decodeFps = receiverStats.decodeFps;
				adaptiveInput.displayFps = receiverStats.displayFps;
				adaptiveInput.displayedFrames = receiverStats.displayedFrames;
				adaptiveInput.deadlineDroppedFrames =
					receiverStats.deadlineDroppedFrames;
				adaptiveInput.outputQueueDroppedFrames =
					receiverStats.outputQueueDroppedFrames;
				adaptiveInput.deadlineNackSentFrames =
					receiverStats.deadlineNackSentFrames;
				adaptiveInput.deadlineNackMissingChunks =
					receiverStats.deadlineNackMissingChunks;
				adaptiveInput.deadlineNackExpiredDroppedFrames =
					receiverStats.deadlineNackExpiredDroppedFrames;
				adaptiveInput.deadlineNackExpiredAfterNackFrames =
					receiverStats.deadlineNackExpiredAfterNackFrames;
				adaptiveInput.ackStaleDroppedFrames =
					networkManager->GetAckStaleDroppedFrameCount();
				adaptiveInput.ackKeyFrameRequests =
					networkManager->GetAckKeyFrameRequestCount();
				adaptiveInput.receiveFreshnessDroppedFrames =
					videoReceiverStats.freshnessDroppedFrames;
				adaptiveInput.receiveDecodeInputFrameAgeMs =
					videoReceiverStats.decodeInputFrameAgeMs;
				adaptiveInput.receiveLatestDecodedFrameAgeMs =
					videoReceiverStats.latestDecodedFrameAgeMs;
				adaptiveInput.receiveFreshnessDropThresholdMs =
					videoReceiverStats.freshnessDropThresholdMs;
				adaptiveInput.lastOutputQueueDropReason =
					receiverStats.lastOutputQueueDropReason;
				const net::PacketPacerStats pacingStats =
					networkManager->GetPacingStats();
				adaptiveInput.pacingEnabled = pacingStats.enabled;
				adaptiveInput.pacingDeadlineDroppedPackets =
					pacingStats.deadlineDroppedPackets;
				adaptiveInput.pacingCurrentQueueDelayMs =
					pacingStats.currentQueueDelayMs;
				adaptiveInput.pacingMaxQueueDelayMs =
					pacingStats.maxQueueDelayMs;
				adaptiveInput.networkConditionEnabled =
					networkManager->GetNetworkCondition().enabled;
				adaptiveInput.networkExperimentActive =
					networkExperimentRunner.IsActive();
				const net::BandwidthEstimatorStats bandwidthStats =
					networkManager->GetBandwidthEstimatorStats();
				adaptiveInput.estimatedBandwidthBps =
					bandwidthStats.estimatedBandwidthBps;
				adaptiveInput.deliveryRateBps =
					bandwidthStats.deliveryRateBps;
				adaptiveInput.bandwidthQueueDelayMs =
					bandwidthStats.queueDelayMs;
				adaptiveInput.bandwidthRttTrendMs =
					bandwidthStats.rttTrendMs;
				adaptiveInput.bandwidthLossTrend =
					bandwidthStats.lossTrend;
				adaptiveInput.bandwidthJitterTrendMs =
					bandwidthStats.jitterTrendMs;
				adaptiveInput.bandwidthFeedbackSamples =
					bandwidthStats.feedbackSamples;
				adaptiveInput.fecEnabled = networkManager->IsFecEnabled();
				adaptiveInput.adaptiveFecEnabled =
					networkManager->IsAdaptiveFecEnabled();
				adaptiveInput.fecGroupChunkCount =
					networkManager->GetFecGroupChunkCount();
				adaptiveInput.fecParityPackets =
					receiverStats.fecParityPackets;
				adaptiveInput.fecRecoveredFrames =
					receiverStats.fecRecoveredFrames;
				adaptiveInput.fecRecoveredChunks =
					receiverStats.fecRecoveredChunks;

				net::AdaptiveStreamingState adaptiveState{};
				{
					std::lock_guard<std::mutex> lock(
						adaptiveControllerMutex);
					adaptiveController->Update(adaptiveInput, deltaTimeSec);
					adaptiveState = adaptiveController->GetState();
				}

				networkManager->UpdateAdaptiveFec(
					(std::max)(
						adaptiveInput.packetLossRate,
						adaptiveInput.bandwidthLossTrend),
					adaptiveInput.ackMissingRate,
					adaptiveInput.deadlineNackSentFrames,
					adaptiveInput.deadlineNackExpiredDroppedFrames,
					receiverStats.fecParityPackets,
					receiverStats.fecRecoveredFrames,
					adaptiveInput.estimatedBandwidthBps,
					static_cast<uint32_t>(
						(std::max)(0, adaptiveState.targetBitrateKbps)),
					adaptiveInput.bandwidthQueueDelayMs);
			}

			if ((networkCsvLogger.IsRunning() ||
				networkExperimentReporter.IsRunning()) &&
				std::chrono::duration_cast<std::chrono::milliseconds>(
					now - lastNetworkCsvSampleTime).count() >= 1000) {
				const double appTimeSec =
					std::chrono::duration<double>(
						now - networkCsvStartTime
					).count();

				const net::NetworkStatsSnapshot networkStats =
					collectNetworkStats();

				if (networkCsvLogger.IsRunning()) {
					networkCsvLogger.WriteSample(
						networkStats,
						appTimeSec);
				}

				if (networkExperimentRunner.IsActive() &&
					networkExperimentReporter.IsRunning()) {
					networkExperimentReporter.RecordSample(
						networkExperimentRunner.CurrentScenarioName(),
						networkStats,
						appTimeSec,
						networkExperimentRunner.ElapsedSec(),
						networkExperimentRunner.WarmupSec());
				}

				lastNetworkCsvSampleTime = now;
			}

			runLoop.RenderFrame();
		}

	}

	// Stop network components before releasing providers that may reference them.
	videoSenderRunning.store(false);
	if (videoSenderThread.joinable()) {
		videoSenderThread.join();
	}
	networkVideoReceiver.Stop();

	runLoop.SetNetworkStatsProvider({});
	networkExperimentReporter.Stop();
	networkCsvLogger.Stop();

	if (adaptiveController) {
		adaptiveController.reset();
	}

	if (networkManager) {
		networkManager->StopRNVPControlReceiver();
		networkManager.reset();
	}

	if (udpReceiver) {
		udpReceiver->Stop();
		udpReceiver.reset();
	}

	if (cameraCapture) {
		cameraCapture->Shutdown();
		cameraCapture.reset();
	}

	MFShutdown();

	std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	std::chrono::time_point<std::chrono::system_clock, std::chrono::seconds>
		nowSeconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
	std::chrono::zoned_time localTime{ std::chrono::current_zone(), nowSeconds };
	std::string dateString = std::format("{:%Y%m%d_%H%M%S}", localTime);
	std::string logFilePath = std::string("logs/") + dateString + "log";
	std::ofstream logStream(logFilePath);
#if defined(_DEBUG)||DEVELOP
	imguiLayer.Shutdown();
#endif

	runLoop.Shutdown();
	engineContext.Shutdown();



















	audio.Finalize();

	audio.Unload(&soundData1);


#if defined(_DEBUG) || defined(DEVELOP)
	ComPtr<ID3D12Debug1> debugController = nullptr;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
		debugController->EnableDebugLayer();

		debugController->SetEnableGPUBasedValidation(TRUE);
	}
	debugController.Reset();
#endif
	bootstrap.Shutdown();
	return 0;

}
