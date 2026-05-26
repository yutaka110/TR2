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
#include "../network/PacketProtocol.h"
#include <algorithm>
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

	std::vector<uint8_t> ResizeRgbaNearest(
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
			const uint32_t srcY =
				std::min<uint32_t>(
					srcHeight - 1u,
					static_cast<uint32_t>(
						(static_cast<uint64_t>(y) * srcHeight) / dstHeight
					)
				);

			for (uint32_t x = 0; x < dstWidth; ++x) {
				const uint32_t srcX =
					std::min<uint32_t>(
						srcWidth - 1u,
						static_cast<uint32_t>(
							(static_cast<uint64_t>(x) * srcWidth) / dstWidth
						)
					);

				const size_t srcIndex =
					(static_cast<size_t>(srcY) * srcWidth + srcX) * 4u;

				const size_t dstIndex =
					(static_cast<size_t>(y) * dstWidth + x) * 4u;

				std::memcpy(dst.data() + dstIndex, src.data() + srcIndex, 4u);
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
	if (GetEnvironmentVariableA("TR2_NETWORK_EXPERIMENT_AUTO", nullptr, 0) > 0) {
		runtimeState.networkExperimentMode = true;
	}
	AppParticleSystem particleSystem;
	runtimeState.transform.scale = { 6.0f, 6.0f, 6.0f };
	runtimeState.transform.rotate = { 0.0f, 0.0f, 0.0f };
	runtimeState.transform.translate = { 0.0f, 0.0f, 0.0f };

	runtimeState.transformSprite.scale = { 360.0f, 203.0f, 1.0f };
	runtimeState.transformSprite.rotate = { 0.0f, 0.0f, 0.0f };

	runtimeState.transformSprite.translate = { 640.0f, 560.0f, 0.0f };

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
	const UINT texWidth = 320;
	const UINT texHeight = 180;
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

	ComPtr<ID3D12Resource> receivedUploadBuffer;
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

		HRESULT hr = device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&bufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&receivedUploadBuffer)
		);

		if (FAILED(hr)) {
			OutputDebugStringA("[AppMain] Failed to create receivedUploadBuffer.\n");
		}
	}

	AppAudio audio;
	audio.Initialize();


	::audio::SoundData soundData1 = audio.LoadWave("Resources/Alarm01.wav");

	audio.PlayWave(soundData1);
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

	auto adaptiveController = std::make_unique<net::AdaptiveStreamingController>();

	if (udpReceiver->Start(kRnvpListenPort)) {
		std::cout << "[AppMain] UdpReceiver started. port="
			<< kRnvpListenPort << "\n";
	}
	else {
		std::cerr << "[AppMain] Failed to start UdpReceiver. port="
			<< kRnvpListenPort << "\n";
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
			adaptive = adaptiveController.get()
		]() {
		net::NetworkStatsSnapshot stats{};

		if (receiver) {
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

			stats.currentRttMs = sender->GetLastRttMs();
			stats.averageRttMs = sender->GetAverageRttMs();

			stats.maxRttMs = sender->GetLastRttMs();

			stats.rttSamples = sender->GetRttSampleCount();

			stats.networkCondition = sender->GetNetworkCondition();
			stats.networkSimulation = sender->GetNetworkSimulationStats();
		}

		if (adaptive) {
			const net::AdaptiveStreamingState adaptiveState =
				adaptive->GetState();

			stats.adaptiveEnabled = adaptive->IsEnabled();

			stats.adaptiveTargetJpegQuality =
				adaptiveState.targetJpegQuality;

			stats.adaptiveTargetFps =
				adaptiveState.targetFps;

			stats.adaptiveTargetBitrateKbps =
				adaptiveState.targetBitrateKbps;

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
	net::NetworkExperimentRunner networkExperimentRunner;
	net::NetworkExperimentReporter networkExperimentReporter;
	if (networkExperimentReporter.Start("logs")) {
		std::cout << "[AppMain] Network experiment summary started: "
			<< networkExperimentReporter.CsvFilePath()
			<< " report: "
			<< networkExperimentReporter.MarkdownFilePath()
			<< " before/after: "
			<< networkExperimentReporter.BeforeAfterFilePath()
			<< "\n";
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
	runLoop.SetReceivedFrameProvider(
		[
			receiver = udpReceiver.get()
		](net::CompletedFrame& outFrame) {
			if (!receiver) {
				return false;
			}

			return receiver->TryPopFrame(outFrame);
		}
				);

	// Report decode/display milestones back to the receiver-side network stats.
	runLoop.SetNetworkFrameDecodeNotifier(
		[
			receiver = udpReceiver.get()
		]() {
			if (receiver) {
				receiver->NotifyDecodeFrame();
			}
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
		receivedUploadBuffer,
		receivedSrvHandleGPU,
		texWidth,
		texHeight
	);

	// Prefer a live camera frame; fallback frames keep the network path testable without a camera.
	auto cameraCapture = std::make_unique<CameraCapture>();
	bool cameraCaptureEnabled = cameraCapture->Initialize(texWidth, texHeight);
	if (cameraCaptureEnabled) {
		OutputDebugStringA("[AppMain] Camera capture enabled for RNVP raw video.\n");
	}
	else {
		OutputDebugStringA("[AppMain] Camera capture unavailable; RNVP falls back to generated test video.\n");
	}

	while (msg.message != WM_QUIT) {
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else {
			static auto lastPingTime = std::chrono::steady_clock::now();
			const auto now = std::chrono::steady_clock::now();

			if (networkManager) {
				networkManager->FlushNetworkSimulator();
			}

			const double networkExperimentDeltaSec =
				std::chrono::duration<double>(
					now - lastNetworkExperimentUpdateTime
				).count();
			lastNetworkExperimentUpdateTime = now;

			const bool experimentScenarioChanged =
				networkExperimentRunner.Update(
					runtimeState.networkExperimentMode,
					networkExperimentDeltaSec);

			if (networkExperimentRunner.IsActive()) {
				if (networkManager) {
					networkManager->SetNetworkCondition(
						networkExperimentRunner.CurrentCondition());
				}

				networkCsvLogger.SetScenarioName(
					networkExperimentRunner.CurrentScenarioName());
			}
			else {
				networkCsvLogger.SetScenarioName("Auto");
				if (experimentScenarioChanged && networkManager) {
					networkManager->SetNetworkCondition(
						networkExperimentRunner.CurrentCondition());
				}
			}

			if (experimentScenarioChanged) {
				if (networkExperimentRunner.IsActive()) {
					if (adaptiveController) {
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
						<< " remainingSec="
						<< networkExperimentRunner.RemainingSec();
				}
				else {
					oss << "[AppMain] Network experiment stopped.";
				}

				OutputDebugStringA(oss.str().c_str());
				OutputDebugStringA("\n");
				std::cout << oss.str() << "\n";
			}

			if (networkManager &&
				std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPingTime).count() >= 1000) {
				networkManager->SendRNVPPing(1);
				lastPingTime = now;
			}

			// Capture, adapt, encode, and send one MJPEG/RGBA video frame at the target FPS.
			static auto nextDummyFrameTime = std::chrono::steady_clock::now();
			static uint32_t dummyFrameId = 1;

			if (networkManager && adaptiveController) {
				const net::AdaptiveStreamingState adaptiveState =
					adaptiveController->GetState();

				int targetFps = adaptiveState.targetFps;

				if (targetFps < 1) {
					targetFps = 1;
				}
				if (targetFps > 30) {
					targetFps = 30;
				}

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

				const auto sendInterval =
					std::chrono::duration<double>(1.0 / static_cast<double>(targetFps));

				if (now >= nextDummyFrameTime) {
					std::vector<uint8_t> videoFrame;
					const bool cameraFrameReady =
						cameraCaptureEnabled &&
						cameraCapture &&
						cameraCapture->TryGetRgbaFrame(videoFrame);

					if (!cameraFrameReady) {
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
									static_cast<uint8_t>((x + dummyFrameId * 3u) & 0xFF);
								const uint8_t g =
									static_cast<uint8_t>((y + dummyFrameId * 2u) & 0xFF);
								const uint8_t b =
									static_cast<uint8_t>(((x / 16u) ^ (y / 16u) ^ dummyFrameId) & 0xFF);

								videoFrame[index + 0] = r;
								videoFrame[index + 1] = g;
								videoFrame[index + 2] = b;
								videoFrame[index + 3] = 255;
							}
						}
					}

					std::vector<uint8_t> adaptiveVideoFrame =
						ResizeRgbaNearest(
							videoFrame,
							texWidth,
							texHeight,
							targetWidth,
							targetHeight
						);

					std::vector<uint8_t> encodedPayload =
						EncodeJpegFrame(
							adaptiveVideoFrame,
							targetWidth,
							targetHeight,
							adaptiveState.targetJpegQuality
						);

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

					if (adaptiveController) {
						adaptiveController->ReportEncodedFrame(
							adaptiveVideoFrame.size(),
							encodedPayload.size()
						);
					}

					networkManager->SendRNVPFragmented(
						encodedPayload,
						dummyFrameId,
						sendCodec,
						1,
						sendAsKeyFrame
					);

					dummyFrameId++;

					if ((dummyFrameId % 60) == 0) {
						std::ostringstream oss;
						oss << "[AppMain] RNVP video frame sent. frameId="
							<< dummyFrameId
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
							<< (cameraFrameReady ? "camera" : "fallback")
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

					nextDummyFrameTime +=
						std::chrono::duration_cast<std::chrono::steady_clock::duration>(sendInterval);

					if (nextDummyFrameTime < now - std::chrono::milliseconds(100)) {
						nextDummyFrameTime =
							now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(sendInterval);
					}
				}
			}

			// AIMD target bitrate updates consume ACK loss, packet loss, RTT, and latency.
			static auto lastAdaptiveUpdateTime = std::chrono::steady_clock::now();

			if (adaptiveController && networkManager && udpReceiver) {
				const auto adaptiveNow = std::chrono::steady_clock::now();

				const double deltaTimeSec =
					std::chrono::duration<double>(adaptiveNow - lastAdaptiveUpdateTime).count();

				lastAdaptiveUpdateTime = adaptiveNow;

				const net::NetworkStatsSnapshot receiverStats = udpReceiver->GetStats();

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
				adaptiveInput.lastOutputQueueDropReason =
					receiverStats.lastOutputQueueDropReason;

				adaptiveController->Update(adaptiveInput, deltaTimeSec);
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
						appTimeSec);
				}

				lastNetworkCsvSampleTime = now;
			}

			runLoop.RenderFrame();
		}

	}

	// Stop network components before releasing providers that may reference them.
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

	engineContext.Shutdown();



















	audio.Finalize();

	runLoop.Shutdown();
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
