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
#include <emmintrin.h>
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
#include "../network/NetworkExperimentReplay.h"
#include "../network/NetworkExperimentRunner.h"
#include "../network/NetworkRuntimeMode.h"
#include "../network/NetworkVideoReceiver.h"
#include "../network/PacketProtocol.h"
#include "../network/H264Encoder.h"
#include "../network/NalUtils.h"
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
	constexpr uint32_t kReceivedVideoSrvIndex = 4;
	constexpr uint32_t kReceivedVideoNv12YSrvIndex = 6;
	constexpr uint32_t kReceivedVideoNv12UVSrvIndex = 7;
	constexpr uint32_t kReceivedVideoUavIndex = 8;
	constexpr uint32_t kDefaultH264LowLatencyMaxWidth = 480;
	constexpr uint32_t kDefaultH264LowLatencyMaxHeight = 270;

	void TraceStartup(const char* stage) {
		if (GetEnvironmentVariableA("RNVP_STARTUP_TRACE", nullptr, 0) == 0) {
			return;
		}

		static std::mutex mutex;
		std::lock_guard<std::mutex> lock(mutex);
		std::ofstream file("startup_trace.log", std::ios::out | std::ios::app);
		if (!file.is_open()) {
			return;
		}

		file << stage << '\n';
		file.flush();
	}

	enum class H264ScaleFilter {
		Fast,
		Bilinear,
	};

	ComPtr<ID3D12Resource> CreateReceivedVideoTexture(
		ComPtr<ID3D12Device> device,
		UINT width,
		UINT height,
		DXGI_FORMAT format,
		D3D12_RESOURCE_FLAGS flags,
		D3D12_RESOURCE_STATES initialState) {
		if (!device) {
			return nullptr;
		}

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = flags;

		D3D12_HEAP_PROPERTIES heapProps{};
		heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

		ComPtr<ID3D12Resource> texture;
		const HRESULT hr = device->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			initialState,
			nullptr,
			IID_PPV_ARGS(&texture));
		return SUCCEEDED(hr) ? texture : nullptr;
	}

	std::vector<ComPtr<ID3D12Resource>> CreateUploadBuffersForTexture(
		ComPtr<ID3D12Device> device,
		ID3D12Resource* texture,
		uint32_t count) {
		std::vector<ComPtr<ID3D12Resource>> buffers;
		if (!device || texture == nullptr || count == 0) {
			return buffers;
		}

		UINT64 uploadBufferSize = 0;
		const D3D12_RESOURCE_DESC textureDesc = texture->GetDesc();
		device->GetCopyableFootprints(
			&textureDesc,
			0,
			1,
			0,
			nullptr,
			nullptr,
			nullptr,
			&uploadBufferSize);

		D3D12_HEAP_PROPERTIES heapProps{};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC bufferDesc{};
		bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufferDesc.Width = uploadBufferSize;
		bufferDesc.Height = 1;
		bufferDesc.DepthOrArraySize = 1;
		bufferDesc.MipLevels = 1;
		bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufferDesc.SampleDesc.Count = 1;
		bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		buffers.reserve(count);
		for (uint32_t i = 0; i < count; ++i) {
			ComPtr<ID3D12Resource> uploadBuffer;
			const HRESULT hr = device->CreateCommittedResource(
				&heapProps,
				D3D12_HEAP_FLAG_NONE,
				&bufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&uploadBuffer));
			if (SUCCEEDED(hr)) {
				buffers.push_back(uploadBuffer);
			}
		}
		return buffers;
	}

	void CreateTextureSrv(
		ID3D12Device* device,
		ID3D12Resource* texture,
		DXGI_FORMAT format,
		D3D12_CPU_DESCRIPTOR_HANDLE handle) {
		if (device == nullptr || texture == nullptr) {
			return;
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
		desc.Format = format;
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		desc.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(texture, &desc, handle);
	}

	void CreateTextureUav(
		ID3D12Device* device,
		ID3D12Resource* texture,
		DXGI_FORMAT format,
		D3D12_CPU_DESCRIPTOR_HANDLE handle) {
		if (device == nullptr || texture == nullptr) {
			return;
		}

		D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
		desc.Format = format;
		desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device->CreateUnorderedAccessView(texture, nullptr, &desc, handle);
	}

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

	bool ReadEnvBool(const char* name, bool fallback) {
		std::string text;
		if (!TryReadEnvString(name, text)) {
			return fallback;
		}

		const std::string value = ToLowerAscii(text);
		if (value == "0" || value == "false" || value == "off" ||
			value == "no") {
			return false;
		}
		if (value == "1" || value == "true" || value == "on" ||
			value == "yes") {
			return true;
		}

		return fallback;
	}

	uint32_t ReadEnvUInt32Clamped(
		const char* name,
		uint32_t fallback,
		uint32_t minValue,
		uint32_t maxValue) {
		std::string text;
		if (!TryReadEnvString(name, text)) {
			return fallback;
		}

		char* end = nullptr;
		const unsigned long value = strtoul(text.c_str(), &end, 10);
		if (end == text.c_str()) {
			return fallback;
		}

		return static_cast<uint32_t>(
			std::clamp<unsigned long>(value, minValue, maxValue));
	}

	double ReadEnvDoubleClamped(
		const char* name,
		double fallback,
		double minValue,
		double maxValue) {
		std::string text;
		if (!TryReadEnvString(name, text)) {
			return fallback;
		}

		char* end = nullptr;
		const double value = std::strtod(text.c_str(), &end);
		if (end == text.c_str()) {
			return fallback;
		}

		return std::clamp(value, minValue, maxValue);
	}

	uint32_t ResolveH264EncoderTargetBitrateKbps(
		uint32_t pacingTargetBitrateKbps,
		bool fecEnabled,
		uint16_t fecGroupChunkCount,
		double videoBudgetScale) {
		if (pacingTargetBitrateKbps == 0) {
			return 500;
		}

		double budgetRatio =
			ReadEnvDoubleClamped(
				"RNVP_H264_VIDEO_BUDGET_RATIO",
				0.95,
				0.35,
				0.95);
		if (fecEnabled && fecGroupChunkCount > 0) {
			budgetRatio *=
				static_cast<double>(fecGroupChunkCount) /
				static_cast<double>(fecGroupChunkCount + 1u);
		}
		double effectiveVideoBudgetScale =
			std::clamp(videoBudgetScale, 0.50, 1.0);
		if (pacingTargetBitrateKbps <= 2200u) {
			effectiveVideoBudgetScale = 1.0;
		}
		else if (pacingTargetBitrateKbps <= 3200u) {
			effectiveVideoBudgetScale =
				(std::max)(effectiveVideoBudgetScale, 0.92);
		}
		budgetRatio *= effectiveVideoBudgetScale;

		const uint32_t bitrateKbps =
			static_cast<uint32_t>(
				std::lround(
					static_cast<double>(pacingTargetBitrateKbps) *
					budgetRatio));
		const uint32_t roundedKbps =
			((bitrateKbps + 125u) / 250u) * 250u;
		const uint32_t clampedKbps = std::clamp<uint32_t>(
			roundedKbps,
			500u,
			pacingTargetBitrateKbps);
		const uint32_t encoderFloorKbps =
			(std::min)(
				pacingTargetBitrateKbps,
				ReadEnvUInt32Clamped(
					"RNVP_H264_ENCODER_MIN_KBPS",
					1400u,
					500u,
					12000u));
		return (std::max)(clampedKbps, encoderFloorKbps);
	}

	struct H264EncoderBitrateHysteresisState {
		uint32_t appliedKbps = 0;
		std::chrono::steady_clock::time_point lastChangeTime{};
		bool hasLastChangeTime = false;
	};

	uint32_t ApplyH264EncoderBitrateHysteresis(
		uint32_t desiredKbps,
		H264EncoderBitrateHysteresisState& state,
		std::chrono::steady_clock::time_point now,
		bool force) {
		desiredKbps = (std::max)(desiredKbps, 500u);
		if (state.appliedKbps == 0 || force) {
			state.appliedKbps = desiredKbps;
			state.lastChangeTime = now;
			state.hasLastChangeTime = true;
			return state.appliedKbps;
		}

		const uint32_t currentKbps = state.appliedKbps;
		if (desiredKbps == currentKbps) {
			return currentKbps;
		}

		const double elapsedSec =
			state.hasLastChangeTime
			? std::chrono::duration<double>(now - state.lastChangeTime).count()
			: 999.0;
		const uint32_t downHysteresisKbps =
			ReadEnvUInt32Clamped(
				"RNVP_H264_BITRATE_DOWN_HYSTERESIS_KBPS",
				500u,
				0u,
				4000u);
		const uint32_t upHysteresisKbps =
			ReadEnvUInt32Clamped(
				"RNVP_H264_BITRATE_UP_HYSTERESIS_KBPS",
				750u,
				0u,
				4000u);
		const uint32_t downStepKbps =
			ReadEnvUInt32Clamped(
				"RNVP_H264_BITRATE_DOWN_STEP_KBPS",
				500u,
				250u,
				4000u);
		const uint32_t upStepKbps =
			ReadEnvUInt32Clamped(
				"RNVP_H264_BITRATE_UP_STEP_KBPS",
				250u,
				250u,
				4000u);

		uint32_t nextKbps = currentKbps;
		if (desiredKbps < currentKbps) {
			if (currentKbps - desiredKbps < downHysteresisKbps ||
				elapsedSec <
					ReadEnvDoubleClamped(
						"RNVP_H264_BITRATE_DOWN_INTERVAL_SEC",
						0.35,
						0.0,
						5.0)) {
				return currentKbps;
			}
			nextKbps =
				(std::max)(
					desiredKbps,
					currentKbps > downStepKbps
					? currentKbps - downStepKbps
					: desiredKbps);
		}
		else {
			if (desiredKbps - currentKbps < upHysteresisKbps ||
				elapsedSec <
					ReadEnvDoubleClamped(
						"RNVP_H264_BITRATE_UP_INTERVAL_SEC",
						1.0,
						0.0,
						10.0)) {
				return currentKbps;
			}
			nextKbps =
				(std::min)(desiredKbps, currentKbps + upStepKbps);
		}

		if (nextKbps != currentKbps) {
			state.appliedKbps = nextKbps;
			state.lastChangeTime = now;
			state.hasLastChangeTime = true;
		}
		return state.appliedKbps;
	}

	uint32_t ResolveH264PeriodicIdrFrames(uint32_t fps) {
		const uint32_t safeFps = (std::max<uint32_t>)(1u, fps);
		const double seconds =
			ReadEnvDoubleClamped(
				"RNVP_H264_PERIODIC_IDR_SEC",
				2.0,
				0.0,
				10.0);
		if (seconds <= 0.0) {
			return 0;
		}

		return (std::max<uint32_t>)(
			1u,
			static_cast<uint32_t>(
				std::lround(seconds * static_cast<double>(safeFps))));
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

	net::CodecType ParseNetworkVideoCodecEnv(
		const std::string& value,
		net::CodecType fallback
	) {
		const std::string mode = ToLowerAscii(value);
		if (mode == "h264" || mode == "h.264" || mode == "avc" ||
			mode == "1") {
			return net::CodecType::H264;
		}
		if (mode == "mjpeg" || mode == "jpeg" || mode == "0") {
			return net::CodecType::MJPEG;
		}
		return fallback;
	}

	H264ScaleFilter ParseH264ScaleFilterEnv(
		const std::string& value,
		H264ScaleFilter fallback) {
		const std::string mode = ToLowerAscii(value);
		if (mode == "bilinear" || mode == "quality") {
			return H264ScaleFilter::Bilinear;
		}
		if (mode == "fast" || mode == "nearest" || mode == "low_latency" ||
			mode == "low-latency") {
			return H264ScaleFilter::Fast;
		}
		return fallback;
	}

	const char* ToString(H264ScaleFilter filter) {
		switch (filter) {
		case H264ScaleFilter::Bilinear:
			return "bilinear";
		case H264ScaleFilter::Fast:
		default:
			return "fast";
		}
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

	std::vector<uint8_t> ConvertRgbaToNv12(
		const std::vector<uint8_t>& rgba,
		uint32_t width,
		uint32_t height) {
		const size_t imageBytes =
			static_cast<size_t>(width) *
			static_cast<size_t>(height) *
			4u;

		if (width == 0 || height == 0 ||
			(width % 2u) != 0 || (height % 2u) != 0 ||
			rgba.size() < imageBytes) {
			return {};
		}

		const size_t yPlaneBytes =
			static_cast<size_t>(width) * static_cast<size_t>(height);
		std::vector<uint8_t> nv12(yPlaneBytes + yPlaneBytes / 2u);
		uint8_t* yPlane = nv12.data();
		uint8_t* uvPlane = nv12.data() + yPlaneBytes;

		auto clampByte = [](int value) {
			return static_cast<uint8_t>(std::clamp(value, 0, 255));
		};

		for (uint32_t y = 0; y < height; y += 2u) {
			for (uint32_t x = 0; x < width; x += 2u) {
				int uSum = 0;
				int vSum = 0;
				for (uint32_t yy = 0; yy < 2u; ++yy) {
					for (uint32_t xx = 0; xx < 2u; ++xx) {
						const uint32_t px = x + xx;
						const uint32_t py = y + yy;
						const size_t index =
							(static_cast<size_t>(py) * width + px) * 4u;
						const int r = rgba[index + 0];
						const int g = rgba[index + 1];
						const int b = rgba[index + 2];
						const int luma =
							((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
						yPlane[static_cast<size_t>(py) * width + px] =
							clampByte(luma);
						uSum += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
						vSum += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
					}
				}

				const size_t uvIndex =
					(static_cast<size_t>(y / 2u) * width) + x;
				uvPlane[uvIndex + 0] = clampByte(uSum / 4);
				uvPlane[uvIndex + 1] = clampByte(vSum / 4);
			}
		}

		return nv12;
	}

	void ConvertFourRgbToYuvSse2(
		const uint8_t* p00,
		const uint8_t* p10,
		const uint8_t* p01,
		const uint8_t* p11,
		uint8_t yOut[4],
		int& uSum,
		int& vSum) {
		const __m128i r =
			_mm_set_epi16(0, 0, 0, 0, p11[0], p01[0], p10[0], p00[0]);
		const __m128i g =
			_mm_set_epi16(0, 0, 0, 0, p11[1], p01[1], p10[1], p00[1]);
		const __m128i b =
			_mm_set_epi16(0, 0, 0, 0, p11[2], p01[2], p10[2], p00[2]);

		const __m128i round = _mm_set1_epi16(128);
		const __m128i y =
			_mm_add_epi16(
				_mm_srli_epi16(
					_mm_add_epi16(
						_mm_add_epi16(
							_mm_add_epi16(
								_mm_mullo_epi16(r, _mm_set1_epi16(66)),
								_mm_mullo_epi16(g, _mm_set1_epi16(129))),
							_mm_mullo_epi16(b, _mm_set1_epi16(25))),
						round),
					8),
				_mm_set1_epi16(16));

		const __m128i chromaBias = _mm_set1_epi16(-32640);
		const __m128i u =
			_mm_srli_epi16(
				_mm_add_epi16(
					_mm_add_epi16(
						_mm_add_epi16(
							_mm_mullo_epi16(r, _mm_set1_epi16(-38)),
							_mm_mullo_epi16(g, _mm_set1_epi16(-74))),
						_mm_mullo_epi16(b, _mm_set1_epi16(112))),
					chromaBias),
				8);
		const __m128i v =
			_mm_srli_epi16(
				_mm_add_epi16(
					_mm_add_epi16(
						_mm_add_epi16(
							_mm_mullo_epi16(r, _mm_set1_epi16(112)),
							_mm_mullo_epi16(g, _mm_set1_epi16(-94))),
						_mm_mullo_epi16(b, _mm_set1_epi16(-18))),
					chromaBias),
				8);

		uint16_t yValues[8]{};
		uint16_t uValues[8]{};
		uint16_t vValues[8]{};
		_mm_storeu_si128(
			reinterpret_cast<__m128i*>(yValues),
			y);
		_mm_storeu_si128(
			reinterpret_cast<__m128i*>(uValues),
			u);
		_mm_storeu_si128(
			reinterpret_cast<__m128i*>(vValues),
			v);

		yOut[0] = static_cast<uint8_t>(yValues[0]);
		yOut[1] = static_cast<uint8_t>(yValues[1]);
		yOut[2] = static_cast<uint8_t>(yValues[2]);
		yOut[3] = static_cast<uint8_t>(yValues[3]);
		uSum =
			static_cast<int>(uValues[0]) +
			static_cast<int>(uValues[1]) +
			static_cast<int>(uValues[2]) +
			static_cast<int>(uValues[3]);
		vSum =
			static_cast<int>(vValues[0]) +
			static_cast<int>(vValues[1]) +
			static_cast<int>(vValues[2]) +
			static_cast<int>(vValues[3]);
	}

	bool ConvertScaledRgbaToNv12Fast(
		const std::vector<uint8_t>& rgba,
		uint32_t srcWidth,
		uint32_t srcHeight,
		uint32_t dstWidth,
		uint32_t dstHeight,
		std::vector<uint8_t>& nv12) {
		const size_t srcBytes =
			static_cast<size_t>(srcWidth) *
			static_cast<size_t>(srcHeight) *
			4u;
		if (srcWidth == 0 || srcHeight == 0 ||
			dstWidth == 0 || dstHeight == 0 ||
			(dstWidth % 2u) != 0 || (dstHeight % 2u) != 0 ||
			rgba.size() < srcBytes) {
			nv12.clear();
			return false;
		}

		if (srcWidth == dstWidth && srcHeight == dstHeight) {
			nv12 = ConvertRgbaToNv12(rgba, dstWidth, dstHeight);
			return !nv12.empty();
		}

		std::vector<uint32_t> xMap(dstWidth);
		std::vector<uint32_t> yMap(dstHeight);
		for (uint32_t x = 0; x < dstWidth; ++x) {
			xMap[x] =
				(std::min<uint32_t>)(
					srcWidth - 1u,
					static_cast<uint32_t>(
						(static_cast<uint64_t>(x) * srcWidth) / dstWidth));
		}
		for (uint32_t y = 0; y < dstHeight; ++y) {
			yMap[y] =
				(std::min<uint32_t>)(
					srcHeight - 1u,
					static_cast<uint32_t>(
						(static_cast<uint64_t>(y) * srcHeight) / dstHeight));
		}

		const size_t yPlaneBytes =
			static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight);
		nv12.resize(yPlaneBytes + yPlaneBytes / 2u);
		uint8_t* yPlane = nv12.data();
		uint8_t* uvPlane = nv12.data() + yPlaneBytes;
		const uint8_t* src = rgba.data();

		auto clampByte = [](int value) {
			return static_cast<uint8_t>(std::clamp(value, 0, 255));
		};

		for (uint32_t y = 0; y < dstHeight; y += 2u) {
			const uint32_t sy0 = yMap[y];
			const uint32_t sy1 = yMap[y + 1u];
			for (uint32_t x = 0; x < dstWidth; x += 2u) {
				const uint32_t sx0 = xMap[x];
				const uint32_t sx1 = xMap[x + 1u];

				const uint8_t* p00 =
					src + (static_cast<size_t>(sy0) * srcWidth + sx0) * 4u;
				const uint8_t* p10 =
					src + (static_cast<size_t>(sy0) * srcWidth + sx1) * 4u;
				const uint8_t* p01 =
					src + (static_cast<size_t>(sy1) * srcWidth + sx0) * 4u;
				const uint8_t* p11 =
					src + (static_cast<size_t>(sy1) * srcWidth + sx1) * 4u;

				uint8_t yValues[4]{};
				int uSum = 0;
				int vSum = 0;
				ConvertFourRgbToYuvSse2(
					p00,
					p10,
					p01,
					p11,
					yValues,
					uSum,
					vSum);

				const size_t yIndex0 =
					static_cast<size_t>(y) * dstWidth + x;
				const size_t yIndex1 =
					static_cast<size_t>(y + 1u) * dstWidth + x;
				yPlane[yIndex0 + 0u] = yValues[0];
				yPlane[yIndex0 + 1u] = yValues[1];
				yPlane[yIndex1 + 0u] = yValues[2];
				yPlane[yIndex1 + 1u] = yValues[3];

				const size_t uvIndex =
					(static_cast<size_t>(y / 2u) * dstWidth) + x;
				uvPlane[uvIndex + 0u] = clampByte((uSum + 2) / 4);
				uvPlane[uvIndex + 1u] = clampByte((vSum + 2) / 4);
			}
		}

		return true;
	}

	bool ConvertScaledNv12ToNv12Fast(
		const uint8_t* srcY,
		const uint8_t* srcUV,
		uint32_t srcWidth,
		uint32_t srcHeight,
		uint32_t srcYPitch,
		uint32_t srcUVPitch,
		uint32_t dstWidth,
		uint32_t dstHeight,
		std::vector<uint8_t>& nv12) {
		if (srcY == nullptr ||
			srcUV == nullptr ||
			srcWidth == 0 ||
			srcHeight == 0 ||
			(srcWidth % 2u) != 0 ||
			(srcHeight % 2u) != 0 ||
			srcYPitch < srcWidth ||
			srcUVPitch < srcWidth ||
			dstWidth == 0 ||
			dstHeight == 0 ||
			(dstWidth % 2u) != 0 ||
			(dstHeight % 2u) != 0) {
			nv12.clear();
			return false;
		}

		const size_t yPlaneBytes =
			static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight);
		nv12.resize(yPlaneBytes + yPlaneBytes / 2u);
		uint8_t* dstYPlane = nv12.data();
		uint8_t* dstUVPlane = nv12.data() + yPlaneBytes;

		if (srcWidth == dstWidth &&
			srcHeight == dstHeight &&
			srcYPitch == dstWidth &&
			srcUVPitch == dstWidth) {
			std::memcpy(dstYPlane, srcY, yPlaneBytes);
			std::memcpy(dstUVPlane, srcUV, yPlaneBytes / 2u);
			return true;
		}

		std::vector<uint32_t> xMap(dstWidth);
		std::vector<uint32_t> yMap(dstHeight);
		for (uint32_t x = 0; x < dstWidth; ++x) {
			xMap[x] =
				(std::min<uint32_t>)(
					srcWidth - 1u,
					static_cast<uint32_t>(
						(static_cast<uint64_t>(x) * srcWidth) / dstWidth));
		}
		for (uint32_t y = 0; y < dstHeight; ++y) {
			yMap[y] =
				(std::min<uint32_t>)(
					srcHeight - 1u,
					static_cast<uint32_t>(
						(static_cast<uint64_t>(y) * srcHeight) / dstHeight));
		}

		for (uint32_t y = 0; y < dstHeight; ++y) {
			const uint32_t sy = yMap[y];
			const uint8_t* srcRow =
				srcY + static_cast<size_t>(sy) * srcYPitch;
			uint8_t* dstRow =
				dstYPlane + static_cast<size_t>(y) * dstWidth;
			for (uint32_t x = 0; x < dstWidth; ++x) {
				dstRow[x] = srcRow[xMap[x]];
			}
		}

		for (uint32_t y = 0; y < dstHeight / 2u; ++y) {
			const uint32_t srcLumaY =
				((std::min<uint32_t>)(
					srcHeight - 1u,
					static_cast<uint32_t>(
						(static_cast<uint64_t>(y * 2u) * srcHeight) /
						dstHeight))) &
				~1u;
			const uint32_t srcChromaY =
				(std::min<uint32_t>)(srcHeight / 2u - 1u, srcLumaY / 2u);
			const uint8_t* srcRow =
				srcUV + static_cast<size_t>(srcChromaY) * srcUVPitch;
			uint8_t* dstRow =
				dstUVPlane + static_cast<size_t>(y) * dstWidth;

			for (uint32_t x = 0; x < dstWidth; x += 2u) {
				const uint32_t srcLumaX =
					((std::min<uint32_t>)(srcWidth - 1u, xMap[x])) & ~1u;
				dstRow[x + 0u] = srcRow[srcLumaX + 0u];
				dstRow[x + 1u] = srcRow[srcLumaX + 1u];
			}
		}

		return true;
	}

	struct ResizeAxisSample {
		uint32_t i0 = 0;
		uint32_t i1 = 0;
		uint32_t weight256 = 0;
	};

	std::vector<ResizeAxisSample> BuildResizeAxisSamples(
		uint32_t srcSize,
		uint32_t dstSize) {
		std::vector<ResizeAxisSample> samples(dstSize);
		if (srcSize == 0 || dstSize == 0) {
			return samples;
		}

		for (uint32_t i = 0; i < dstSize; ++i) {
			const double srcF =
				dstSize <= 1
				? 0.0
				: (static_cast<double>(i) * static_cast<double>(srcSize - 1u)) /
					static_cast<double>(dstSize - 1u);
			const uint32_t i0 =
				(std::min<uint32_t>)(srcSize - 1u, static_cast<uint32_t>(srcF));
			const uint32_t i1 = (std::min<uint32_t>)(srcSize - 1u, i0 + 1u);
			const uint32_t weight =
				static_cast<uint32_t>(
					std::clamp<int>(
						static_cast<int>(
							std::lround(
								(srcF - static_cast<double>(i0)) * 256.0)),
						0,
						256));

			samples[i] = ResizeAxisSample{ i0, i1, weight };
		}

		return samples;
	}

	std::vector<uint8_t> ConvertScaledRgbaToNv12Bilinear(
		const std::vector<uint8_t>& rgba,
		uint32_t srcWidth,
		uint32_t srcHeight,
		uint32_t dstWidth,
		uint32_t dstHeight) {
		const size_t srcBytes =
			static_cast<size_t>(srcWidth) *
			static_cast<size_t>(srcHeight) *
			4u;
		if (srcWidth == 0 || srcHeight == 0 ||
			dstWidth == 0 || dstHeight == 0 ||
			(dstWidth % 2u) != 0 || (dstHeight % 2u) != 0 ||
			rgba.size() < srcBytes) {
			return {};
		}

		if (srcWidth == dstWidth && srcHeight == dstHeight) {
			return ConvertRgbaToNv12(rgba, dstWidth, dstHeight);
		}

		const std::vector<ResizeAxisSample> xSamples =
			BuildResizeAxisSamples(srcWidth, dstWidth);
		const std::vector<ResizeAxisSample> ySamples =
			BuildResizeAxisSamples(srcHeight, dstHeight);

		const size_t yPlaneBytes =
			static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight);
		std::vector<uint8_t> nv12(yPlaneBytes + yPlaneBytes / 2u);
		uint8_t* yPlane = nv12.data();
		uint8_t* uvPlane = nv12.data() + yPlaneBytes;

		auto clampByte = [](int value) {
			return static_cast<uint8_t>(std::clamp(value, 0, 255));
		};

		auto sampleRgb = [&](uint32_t dstX, uint32_t dstY, int& r, int& g, int& b) {
			const ResizeAxisSample& xs = xSamples[dstX];
			const ResizeAxisSample& ys = ySamples[dstY];
			const uint32_t invWx = 256u - xs.weight256;
			const uint32_t invWy = 256u - ys.weight256;

			const size_t i00 =
				(static_cast<size_t>(ys.i0) * srcWidth + xs.i0) * 4u;
			const size_t i10 =
				(static_cast<size_t>(ys.i0) * srcWidth + xs.i1) * 4u;
			const size_t i01 =
				(static_cast<size_t>(ys.i1) * srcWidth + xs.i0) * 4u;
			const size_t i11 =
				(static_cast<size_t>(ys.i1) * srcWidth + xs.i1) * 4u;

			auto channel = [&](uint32_t c) {
				const uint32_t top =
					static_cast<uint32_t>(rgba[i00 + c]) * invWx +
					static_cast<uint32_t>(rgba[i10 + c]) * xs.weight256;
				const uint32_t bottom =
					static_cast<uint32_t>(rgba[i01 + c]) * invWx +
					static_cast<uint32_t>(rgba[i11 + c]) * xs.weight256;
				return static_cast<int>(
					(top * invWy + bottom * ys.weight256 + 32768u) >> 16u);
			};

			r = channel(0);
			g = channel(1);
			b = channel(2);
		};

		for (uint32_t y = 0; y < dstHeight; y += 2u) {
			for (uint32_t x = 0; x < dstWidth; x += 2u) {
				int uSum = 0;
				int vSum = 0;
				for (uint32_t yy = 0; yy < 2u; ++yy) {
					for (uint32_t xx = 0; xx < 2u; ++xx) {
						const uint32_t px = x + xx;
						const uint32_t py = y + yy;
						int r = 0;
						int g = 0;
						int b = 0;
						sampleRgb(px, py, r, g, b);

						const int luma =
							((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
						yPlane[static_cast<size_t>(py) * dstWidth + px] =
							clampByte(luma);
						uSum += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
						vSum += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
					}
				}

				const size_t uvIndex =
					(static_cast<size_t>(y / 2u) * dstWidth) + x;
				uvPlane[uvIndex + 0] = clampByte(uSum / 4);
				uvPlane[uvIndex + 1] = clampByte(vSum / 4);
			}
		}

		return nv12;
	}

	const char* H264NalTypeName(uint8_t type) {
		switch (type) {
		case 1:
			return "non-idr";
		case 5:
			return "idr";
		case 6:
			return "sei";
		case 7:
			return "sps";
		case 8:
			return "pps";
		case 9:
			return "aud";
		default:
			return "other";
		}
	}

	std::string FormatH264NalTypes(const std::vector<NalInfo>& nals) {
		std::ostringstream oss;
		for (size_t i = 0; i < nals.size(); ++i) {
			if (i > 0) {
				oss << '|';
			}
			oss << static_cast<int>(nals[i].type)
				<< ':'
				<< H264NalTypeName(nals[i].type);
		}
		return oss.str();
	}

	void TraceH264AccessUnit(
		uint32_t frameId,
		const char* inputFormat,
		size_t originalBytes,
		size_t normalizedBytes,
		const std::vector<NalInfo>& nals,
		bool hasSps,
		bool hasPps,
		bool hasIdr,
		uint8_t flags) {
		const bool decoderSync =
			(flags & net::H264AccessUnitFlag_DecoderSync) != 0;
		const bool shouldLog =
			frameId <= 180 ||
			hasSps ||
			hasPps ||
			hasIdr ||
			(frameId % 60u) == 0u;
		if (!shouldLog) {
			return;
		}

		const std::string nalTypes = FormatH264NalTypes(nals);

		{
			std::ostringstream debug;
			debug
				<< "[H264NAL] frameId=" << frameId
				<< " format=" << inputFormat
				<< " originalBytes=" << originalBytes
				<< " normalizedBytes=" << normalizedBytes
				<< " nalCount=" << nals.size()
				<< " nalTypes=" << nalTypes
				<< " hasSps=" << (hasSps ? "true" : "false")
				<< " hasPps=" << (hasPps ? "true" : "false")
				<< " hasIdr=" << (hasIdr ? "true" : "false")
				<< " decoderSync=" << (decoderSync ? "true" : "false")
				<< "\n";
			OutputDebugStringA(debug.str().c_str());
		}

		static std::mutex traceMutex;
		static std::ofstream traceFile;
		static bool traceFileInitAttempted = false;

		std::lock_guard<std::mutex> lock(traceMutex);
		if (!traceFileInitAttempted) {
			traceFileInitAttempted = true;
			CreateDirectoryA("logs", nullptr);

			SYSTEMTIME localTime{};
			GetLocalTime(&localTime);
			char path[MAX_PATH]{};
			sprintf_s(
				path,
				"logs\\h264_nal_trace_%04u%02u%02u_%02u%02u%02u.csv",
				static_cast<unsigned>(localTime.wYear),
				static_cast<unsigned>(localTime.wMonth),
				static_cast<unsigned>(localTime.wDay),
				static_cast<unsigned>(localTime.wHour),
				static_cast<unsigned>(localTime.wMinute),
				static_cast<unsigned>(localTime.wSecond));

			traceFile.open(path, std::ios::out | std::ios::trunc);
			if (traceFile.is_open()) {
				traceFile
					<< "frameId,inputFormat,originalBytes,normalizedBytes,"
					<< "nalCount,nalTypes,hasSps,hasPps,hasIdr,"
					<< "containsSpsPps,decoderSync,flags\n";
			}
		}

		if (!traceFile.is_open()) {
			return;
		}

		traceFile
			<< frameId << ','
			<< inputFormat << ','
			<< originalBytes << ','
			<< normalizedBytes << ','
			<< nals.size() << ','
			<< '"' << nalTypes << '"' << ','
			<< (hasSps ? 1 : 0) << ','
			<< (hasPps ? 1 : 0) << ','
			<< (hasIdr ? 1 : 0) << ','
			<< (((flags & net::H264AccessUnitFlag_ContainsSpsPps) != 0) ? 1 : 0) << ','
			<< (decoderSync ? 1 : 0) << ','
			<< static_cast<unsigned>(flags)
			<< '\n';
		traceFile.flush();
	}

	std::vector<uint8_t> PackH264AccessUnitPayload(
		const std::vector<uint8_t>& accessUnit,
		uint32_t frameId,
		uint32_t width,
		uint32_t height,
		uint64_t ptsUs,
		uint64_t cameraCaptureCompletedTimeUs,
		uint64_t encoderOutputTimeUs) {
		if (accessUnit.empty() || width == 0 || height == 0) {
			return {};
		}

		std::vector<uint8_t> normalizedAccessUnit = accessUnit;
		auto convertAvcLengthPrefixedToAnnexB =
			[](const std::vector<uint8_t>& bytes,
				size_t lengthBytes,
				std::vector<uint8_t>& out) {
				static constexpr uint8_t kStartCode[] = { 0, 0, 0, 1 };
				out.clear();
				size_t offset = 0;
				while (offset + lengthBytes <= bytes.size()) {
					uint32_t nalBytes = 0;
					for (size_t i = 0; i < lengthBytes; ++i) {
						nalBytes = (nalBytes << 8u) | bytes[offset + i];
					}
					offset += lengthBytes;

					if (nalBytes == 0 ||
						nalBytes > bytes.size() - offset) {
						out.clear();
						return false;
					}

					out.insert(out.end(), kStartCode, kStartCode + sizeof(kStartCode));
					out.insert(out.end(), bytes.begin() + offset, bytes.begin() + offset + nalBytes);
					offset += nalBytes;
				}

				if (offset != bytes.size()) {
					out.clear();
					return false;
				}
				return !out.empty();
			};

		const char* inputFormat = "annexb";
		std::vector<NalInfo> nals = ScanAnnexB(normalizedAccessUnit);
		if (nals.empty()) {
			std::vector<uint8_t> annexB;
			if (convertAvcLengthPrefixedToAnnexB(accessUnit, 4u, annexB)) {
				normalizedAccessUnit = std::move(annexB);
				nals = ScanAnnexB(normalizedAccessUnit);
				inputFormat = "avc-length-4";
			}
		}
		if (nals.empty()) {
			std::vector<uint8_t> annexB;
			if (convertAvcLengthPrefixedToAnnexB(accessUnit, 2u, annexB)) {
				normalizedAccessUnit = std::move(annexB);
				nals = ScanAnnexB(normalizedAccessUnit);
				inputFormat = "avc-length-2";
			}
		}
		if (nals.empty()) {
			inputFormat = "unknown";
		}

		bool hasIdr = false;
		bool hasSps = false;
		bool hasPps = false;
		for (const NalInfo& nal : nals) {
			hasIdr = hasIdr || nal.type == 5;
			hasSps = hasSps || nal.type == 7;
			hasPps = hasPps || nal.type == 8;
		}
		const bool hasSpsPps = hasSps || hasPps;

		uint8_t flags = net::H264AccessUnitFlag_Discardable;
		if (hasIdr) {
			flags |= net::H264AccessUnitFlag_Idr;
			flags |= net::H264AccessUnitFlag_DecoderSync;
			flags &= ~net::H264AccessUnitFlag_Discardable;
		}
		if (hasSpsPps) {
			flags |= net::H264AccessUnitFlag_ContainsSpsPps;
		}

		TraceH264AccessUnit(
			frameId,
			inputFormat,
			accessUnit.size(),
			normalizedAccessUnit.size(),
			nals,
			hasSps,
			hasPps,
			hasIdr,
			flags);

		std::vector<uint8_t> payload(
			net::kH264AccessUnitPayloadHeaderV3Size + normalizedAccessUnit.size());

		net::H264AccessUnitPayloadHeader header{};
		header.frameId = frameId;
		header.ptsUs = ptsUs;
		header.codecConfigId = hasSpsPps ? frameId : 0;
		header.width = static_cast<uint16_t>(width);
		header.height = static_cast<uint16_t>(height);
		header.flags = flags;
		header.nalUnitCount =
			static_cast<uint16_t>((std::min<size_t>)(nals.size(), 0xFFFFu));
		header.accessUnitBytes = static_cast<uint32_t>(normalizedAccessUnit.size());
		header.headerBytes = static_cast<uint16_t>(net::kH264AccessUnitPayloadHeaderV3Size);
		header.accessUnitCrc32 =
			net::ComputeCrc32(normalizedAccessUnit.data(), normalizedAccessUnit.size());
		header.cameraCaptureCompletedTimeUs = cameraCaptureCompletedTimeUs;
		header.encoderOutputTimeUs = encoderOutputTimeUs;

		net::EncodeH264AccessUnitPayloadHeaderV3(payload.data(), header);
		std::memcpy(
			payload.data() + net::kH264AccessUnitPayloadHeaderV3Size,
			normalizedAccessUnit.data(),
			normalizedAccessUnit.size());

		return payload;
	}

	bool IsH264AccessUnitPayloadDecoderSync(
		const std::vector<uint8_t>& payload) {
		net::H264AccessUnitPayloadHeader auHeader{};
		if (!net::DecodeH264AccessUnitPayloadHeader(
				payload.data(),
				payload.size(),
				auHeader)) {
			return false;
		}

		return
			((auHeader.flags & net::H264AccessUnitFlag_Idr) != 0) ||
			((auHeader.flags & net::H264AccessUnitFlag_DecoderSync) != 0);
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
	TraceStartup("run-enter");
	D3DResourceLeakChecker leakCheck;

	if (net::RunNetworkExperimentReplayFromEnv("logs")) {
		TraceStartup("run-replay-exit");
		return 0;
	}

	AppBootstrap bootstrap;
	TraceStartup("bootstrap-before");
	if (!bootstrap.Initialize(hInstance_)) {
		TraceStartup("bootstrap-failed");
		return -1;
	}
	TraceStartup("bootstrap-ok");

	if (FAILED(MFStartup(MF_VERSION))) {
		OutputDebugStringA("[AppMain] MFStartup failed.\n");
		TraceStartup("mfstartup-failed");
		return -1;
	}
	TraceStartup("mfstartup-ok");

	const HWND hwnd = bootstrap.Handle();
	const uint32_t windowWidth = bootstrap.Width();
	const uint32_t windowHeight = bootstrap.Height();

	HRESULT hr = S_OK;


	EngineContext engineContext;
	TraceStartup("engine-initialize-before");
	if (!engineContext.Initialize(hwnd, windowWidth, windowHeight, /*enableDebugLayer=*/true)) {
		TraceStartup("engine-initialize-failed");
		return -1;
	}
	TraceStartup("engine-initialize-ok");

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
	TraceStartup("pipelines-initialize-before");
	if (!appPipelines.Initialize(device.Get())) {
		OutputDebugStringA("[AppMain] AppPipelines initialization failed.\n");
		TraceStartup("pipelines-initialize-failed");
		return 1;
	}
	TraceStartup("pipelines-initialize-ok");
	TraceStartup("scene-initialize-before");
	if (!scene.Initialize(device, srvDescriptorHeap, descriptorSizeSRV)) {
		OutputDebugStringA("[AppMain] AppSceneResources initialization failed.\n");
		TraceStartup("scene-initialize-failed");
		return 1;
	}
	TraceStartup("scene-initialize-ok");

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
	if (std::string codecModeEnv; TryReadEnvString("RNVP_CODEC", codecModeEnv)) {
		runtimeState.networkVideoCodec =
			ParseNetworkVideoCodecEnv(
				codecModeEnv,
				runtimeState.networkVideoCodec);
	}
	H264ScaleFilter h264ScaleFilter = H264ScaleFilter::Fast;
	if (std::string h264ScaleFilterEnv;
		TryReadEnvString("RNVP_H264_SCALE_FILTER", h264ScaleFilterEnv)) {
		h264ScaleFilter =
			ParseH264ScaleFilterEnv(
				h264ScaleFilterEnv,
				h264ScaleFilter);
	}
	{
		std::ostringstream debug;
		debug
			<< "[AppMain] H.264 scale filter="
			<< ToString(h264ScaleFilter)
			<< "\n";
		OutputDebugStringA(debug.str().c_str());
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
	const UINT receivedTexWidth =
		runtimeState.networkVideoCodec == net::CodecType::H264
			? (std::max<UINT>)(
				2u,
				ReadEnvUInt32Clamped(
					"RNVP_H264_MAX_WIDTH",
					kDefaultH264LowLatencyMaxWidth,
					160u,
					texWidth) & ~1u)
			: texWidth;
	const UINT receivedTexHeight =
		runtimeState.networkVideoCodec == net::CodecType::H264
			? (std::max<UINT>)(
				2u,
				ReadEnvUInt32Clamped(
					"RNVP_H264_MAX_HEIGHT",
					kDefaultH264LowLatencyMaxHeight,
					90u,
					texHeight) & ~1u)
			: texHeight;
	ComPtr<ID3D12Resource> texture =
		CreateReceivedVideoTexture(
			device,
			receivedTexWidth,
			receivedTexHeight,
			DXGI_FORMAT_R8G8B8A8_UNORM,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	ComPtr<ID3D12Resource> receivedNv12YTexture =
		CreateReceivedVideoTexture(
			device,
			receivedTexWidth,
			receivedTexHeight,
			DXGI_FORMAT_R8_UNORM,
			D3D12_RESOURCE_FLAG_NONE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	ComPtr<ID3D12Resource> receivedNv12UVTexture =
		CreateReceivedVideoTexture(
			device,
			receivedTexWidth / 2,
			receivedTexHeight / 2,
			DXGI_FORMAT_R8G8_UNORM,
			D3D12_RESOURCE_FLAG_NONE,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	D3D12_CPU_DESCRIPTOR_HANDLE receivedSrvHandleCPU =
		AppRenderResources::GetCPUDescriptorHandle(
			srvDescriptorHeap,
			descriptorSizeSRV,
			kReceivedVideoSrvIndex);
	CreateTextureSrv(
		device.Get(),
		texture.Get(),
		DXGI_FORMAT_R8G8B8A8_UNORM,
		receivedSrvHandleCPU);

	D3D12_GPU_DESCRIPTOR_HANDLE receivedSrvHandleGPU =
		AppRenderResources::GetGPUDescriptorHandle(
			srvDescriptorHeap,
			descriptorSizeSRV,
			kReceivedVideoSrvIndex
		);

	D3D12_CPU_DESCRIPTOR_HANDLE receivedNv12YSrvHandleCPU =
		AppRenderResources::GetCPUDescriptorHandle(
			srvDescriptorHeap,
			descriptorSizeSRV,
			kReceivedVideoNv12YSrvIndex);
	CreateTextureSrv(
		device.Get(),
		receivedNv12YTexture.Get(),
		DXGI_FORMAT_R8_UNORM,
		receivedNv12YSrvHandleCPU);
	D3D12_GPU_DESCRIPTOR_HANDLE receivedNv12YSrvHandleGPU =
		AppRenderResources::GetGPUDescriptorHandle(
			srvDescriptorHeap,
			descriptorSizeSRV,
			kReceivedVideoNv12YSrvIndex);

	D3D12_CPU_DESCRIPTOR_HANDLE receivedNv12UVSrvHandleCPU =
		AppRenderResources::GetCPUDescriptorHandle(
			srvDescriptorHeap,
			descriptorSizeSRV,
			kReceivedVideoNv12UVSrvIndex);
	CreateTextureSrv(
		device.Get(),
		receivedNv12UVTexture.Get(),
		DXGI_FORMAT_R8G8_UNORM,
		receivedNv12UVSrvHandleCPU);
	D3D12_GPU_DESCRIPTOR_HANDLE receivedNv12UVSrvHandleGPU =
		AppRenderResources::GetGPUDescriptorHandle(
			srvDescriptorHeap,
			descriptorSizeSRV,
			kReceivedVideoNv12UVSrvIndex);

	D3D12_CPU_DESCRIPTOR_HANDLE receivedUavHandleCPU =
		AppRenderResources::GetCPUDescriptorHandle(
			srvDescriptorHeap,
			descriptorSizeSRV,
			kReceivedVideoUavIndex);
	CreateTextureUav(
		device.Get(),
		texture.Get(),
		DXGI_FORMAT_R8G8B8A8_UNORM,
		receivedUavHandleCPU);
	D3D12_GPU_DESCRIPTOR_HANDLE receivedUavHandleGPU =
		AppRenderResources::GetGPUDescriptorHandle(
			srvDescriptorHeap,
			descriptorSizeSRV,
			kReceivedVideoUavIndex);

	std::vector<ComPtr<ID3D12Resource>> receivedUploadBuffers =
		CreateUploadBuffersForTexture(
			device,
			texture.Get(),
			kReceivedVideoUploadBufferCount);
	std::vector<ComPtr<ID3D12Resource>> receivedNv12YUploadBuffers =
		CreateUploadBuffersForTexture(
			device,
			receivedNv12YTexture.Get(),
			kReceivedVideoUploadBufferCount);
	std::vector<ComPtr<ID3D12Resource>> receivedNv12UVUploadBuffers =
		CreateUploadBuffersForTexture(
			device,
			receivedNv12UVTexture.Get(),
			kReceivedVideoUploadBufferCount);

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
		std::string actualCodec;
		uint32_t actualEncodeWidth = 0;
		uint32_t actualEncodeHeight = 0;
		uint64_t actualRawFrameBytes = 0;
		uint64_t actualEncodedFrameBytes = 0;
		double captureFps = 0.0;
		std::string cameraCaptureSubtype;
		uint32_t cameraCaptureWidth = 0;
		uint32_t cameraCaptureHeight = 0;
		double cameraCaptureFormatFps = 0.0;
		double encodeMs = 0.0;
		double resizeMs = 0.0;
		double nv12PrepareMs = 0.0;
		double h264EncodeMs = 0.0;
		uint32_t h264EncoderRequestedBitrateKbps = 0;
		uint32_t h264EncoderTargetBitrateKbps = 0;
		uint32_t h264EncoderAppliedBitrateKbps = 0;
		uint64_t h264DynamicBitrateUpdateRequests = 0;
		uint64_t h264DynamicBitrateUpdateSuccesses = 0;
		uint64_t h264DynamicBitrateUpdateFailures = 0;
		uint64_t h264EncoderReinitializations = 0;
		uint32_t pacingTargetBitrateKbps = 0;
		uint32_t h264AuChunkCount = 0;
		bool h264AuIsIdr = false;
		bool h264AuIsDecoderSync = false;
		uint64_t h264RequestedKeyFrameConsumedFrames = 0;
		uint64_t h264EncoderKeyFrameRequests = 0;
		uint64_t h264RecoveryKeyFrameRequests = 0;
		uint64_t h264AwaitingSyncKeyFrameRequests = 0;
		uint64_t h264PeriodicIdrRequests = 0;
		uint64_t h264IdrFrames = 0;
		uint64_t h264DecoderSyncFrames = 0;
		uint32_t h264ConsecutiveIdrFrames = 0;
		uint32_t h264MaxConsecutiveIdrFrames = 0;
		std::string h264AuProtectionLevel;
		bool h264AuDroppedBeforeSend = false;
		std::string h264AuDropReason;
		uint64_t h264AuDroppedBytes = 0;
		uint32_t h264AuDroppedChunks = 0;
		double h264AuPacingQueueDelayMs = 0.0;
		double h264AuEstimatedSendMs = 0.0;
		bool h264InputGatedByPacing = false;
		std::string h264InputGateReason;
		double h264InputGateQueueDelayMs = 0.0;
		double h264InputGateVideoCreditBytes = 0.0;
		double h264InputGateFrameBudgetBytes = 0.0;
		double h264InputGateDurationMs = 0.0;
		uint32_t h264InputGateConsecutiveFrames = 0;
		uint64_t h264InputGateSkippedInputFrames = 0;
		bool h264InputGateForcedOpen = false;
		std::string h264InputGateReleaseReason;
		uint64_t fecProtectedH264KeyFrames = 0;
		uint64_t fecProtectedH264LargeFrames = 0;
		uint32_t h264EncoderDelayFrames = 0;
		double h264EncoderDelayMs = 0.0;
		uint32_t h264EncoderPendingFrames = 0;
		uint32_t h264EncodedInputFrameId = 0;
		double h264EncoderCallMs = 0.0;
		double h264EncoderSampleCreateMs = 0.0;
		double h264EncoderProcessInputMs = 0.0;
		double h264EncoderPreInputPollMs = 0.0;
		double h264EncoderPostInputWaitMs = 0.0;
		double h264EncoderProcessOutputMs = 0.0;
		double h264EncoderOutputCopyMs = 0.0;
		uint32_t h264EncoderProcessOutputAttempts = 0;
		uint32_t h264EncoderAsyncEventCount = 0;
		bool h264EncoderHardware = false;
		bool h264EncoderAsync = false;
		bool h264EncoderNeedInputSignaled = false;
		bool h264EncoderOutputProduced = false;
		bool h264EncoderOutputProducedBeforeInput = false;
		bool h264SubmittedNewInput = false;
		bool h264AsyncSubmittedWithoutOutput = false;
		bool h264AsyncPendingNoOutput = false;
		bool h264AsyncCadenceHoldActive = false;
		uint32_t h264AsyncPendingNoOutputStreak = 0;
		double h264AsyncCadenceScale = 1.0;
		double h264AsyncCadenceHoldRemainingMs = 0.0;
		double h264AsyncOutputPollBackoffMs = 0.0;
		uint32_t h264InputCadenceFps = 0;
		bool h264NeedInputSubmitWake = false;
		double h264NeedInputSubmitLeadMs = 0.0;
		uint64_t encodedCameraFrameId = 0;
		int64_t encodedCameraSourceTimestamp100ns = 0;
		uint64_t encodedCameraCaptureCompletedTimeUs = 0;
		double encodedCameraFrameAgeMs = 0.0;
		double encodedCameraReadSampleMs = 0.0;
		double encodedCameraReadSampleEndToCaptureMs = 0.0;
		double encodedCameraCaptureToPublishMs = 0.0;
		double encodedCameraPublishToAcquireMs = 0.0;
		double encodedCameraAcquireToEncoderInputMs = 0.0;
		double jpegEncodeMs = 0.0;
		double packetizeMs = 0.0;
		double sendFrameIntervalMs = 0.0;
		bool cameraFrameReady = false;
		uint64_t cameraFrameId = 0;
		int64_t cameraSourceTimestamp100ns = 0;
		uint64_t cameraReadSampleStartTimeUs = 0;
		uint64_t cameraReadSampleEndTimeUs = 0;
		uint64_t cameraCaptureCompletedTimeUs = 0;
		uint64_t cameraFramePublishedTimeUs = 0;
		uint64_t cameraSenderAcquireTimeUs = 0;
		double cameraReadSampleMs = 0.0;
		double cameraReadSampleEndToCaptureMs = 0.0;
		double cameraCaptureToPublishMs = 0.0;
		double cameraPublishToAcquireMs = 0.0;
		double cameraAcquireToSendMs = 0.0;
		double cameraFrameAgeMs = 0.0;
		bool cameraFrameCacheUsed = false;
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
		TraceStartup("udp-start-before");
		if (udpReceiver->Start(kRnvpListenPort)) {
			std::cout << "[AppMain] UdpReceiver started. port="
				<< kRnvpListenPort << "\n";
			TraceStartup("udp-start-ok");
		}
		else {
			std::cerr << "[AppMain] Failed to start UdpReceiver. port="
				<< kRnvpListenPort << "\n";
			TraceStartup("udp-start-failed");
		}
	}
	else {
		std::cout << "[AppMain] UdpReceiver disabled for network mode="
			<< net::ToString(runtimeState.networkRuntimeMode) << "\n";
		TraceStartup("udp-start-disabled");
	}
	TraceStartup("udp-start-after");

	if (networkManager) {
		TraceStartup("control-receiver-start-before");
		if (networkManager->StartRNVPControlReceiver()) {
			std::cout << "[AppMain] NetworkManager RNVP control receiver started.\n";
			TraceStartup("control-receiver-start-ok");
		}
		else {
			std::cerr << "[AppMain] Failed to start NetworkManager RNVP control receiver.\n";
			TraceStartup("control-receiver-start-failed");
		}
	}
	TraceStartup("control-receiver-start-after");

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
			stats.repairCanceledByCompleteAckPackets =
				sender->GetRepairCanceledByCompleteAckPacketCount();
			stats.repairSkippedByTtlPackets =
				sender->GetRepairSkippedByTtlPacketCount();
			stats.repairQueuedButCanceledPackets =
				sender->GetRepairQueuedButCanceledPacketCount();
			stats.repairSentAfterCompleteAckPackets =
				sender->GetRepairSentAfterCompleteAckPacketCount();
			stats.repairSentAfterCompleteAckLargePackets =
				sender->GetRepairSentAfterCompleteAckLargePacketCount();
			stats.repairSuppressedByFecLikelyFrames =
				sender->GetRepairSuppressedByFecLikelyFrameCount();
			stats.repairSuppressedByFecLikelyPackets =
				sender->GetRepairSuppressedByFecLikelyPacketCount();
			stats.repairSuppressedByFecLikelyLargeFrames =
				sender->GetRepairSuppressedByFecLikelyLargeFrameCount();
			stats.repairSuppressedByFecLikelyLargePackets =
				sender->GetRepairSuppressedByFecLikelyLargePacketCount();
			stats.repairBudgetSuppressedFrames =
				sender->GetRepairBudgetSuppressedFrameCount();
			stats.repairBudgetSuppressedPackets =
				sender->GetRepairBudgetSuppressedPacketCount();
			stats.repairBudgetSuppressedLargeFrames =
				sender->GetRepairBudgetSuppressedLargeFrameCount();
			stats.repairBudgetSuppressedLargePackets =
				sender->GetRepairBudgetSuppressedLargePacketCount();
			stats.repairRaceGuardSuppressedFrames =
				sender->GetRepairRaceGuardSuppressedFrameCount();
			stats.repairRaceGuardSuppressedPackets =
				sender->GetRepairRaceGuardSuppressedPacketCount();
			stats.repairRaceGuardSuppressedLargePackets =
				sender->GetRepairRaceGuardSuppressedLargePacketCount();
			stats.repairBudgetProfile =
				sender->GetRepairBudgetProfile();
			stats.repairBudgetProfileSwitches =
				sender->GetRepairBudgetProfileSwitchCount();
			stats.repairBudgetSmoothedMissingRate =
				sender->GetRepairBudgetSmoothedMissingRate();
			stats.repairBudgetSmoothedPacingQueueDelayMs =
				sender->GetRepairBudgetSmoothedPacingQueueDelayMs();
			stats.repairBudgetSmoothedDeliveryMs =
				sender->GetRepairBudgetSmoothedDeliveryMs();
			stats.repairFecLikelySuppressedCompletedFrames =
				sender->GetRepairFecLikelySuppressedCompletedFrameCount();
			stats.repairFecLikelySuppressedCompletedPackets =
				sender->GetRepairFecLikelySuppressedCompletedPacketCount();
			stats.repairFecLikelySuppressedExpiredFrames =
				sender->GetRepairFecLikelySuppressedExpiredFrameCount();
			stats.repairFecLikelySuppressedExpiredPackets =
				sender->GetRepairFecLikelySuppressedExpiredPacketCount();
			stats.repairFecLikelySuppressedPendingFrames =
				sender->GetRepairFecLikelySuppressedPendingFrameCount();
			stats.repairFecLikelySuppressedPendingPackets =
				sender->GetRepairFecLikelySuppressedPendingPacketCount();
			stats.repairFecLikelySuppressionRescueFrames =
				sender->GetRepairFecLikelySuppressionRescueFrameCount();
			stats.repairFecLikelySuppressionRescuePackets =
				sender->GetRepairFecLikelySuppressionRescuePacketCount();
			stats.h264KeyTinyMissingCriticalFrames =
				sender->GetH264KeyTinyMissingCriticalFrameCount();
			stats.h264KeyTinyMissingCriticalPackets =
				sender->GetH264KeyTinyMissingCriticalPacketCount();
			stats.h264KeyTinyMissingCriticalSentPackets =
				sender->GetH264KeyTinyMissingCriticalSentPacketCount();
			stats.h264KeyTinyMissingCriticalSkippedPackets =
				sender->GetH264KeyTinyMissingCriticalSkippedPacketCount();
			stats.h264KeyTinyMissingCriticalFeasibilitySuppressedFrames =
				sender
					->GetH264KeyTinyMissingCriticalFeasibilitySuppressedFrameCount();
			stats.h264KeyTinyMissingCriticalFeasibilitySuppressedPackets =
				sender
					->GetH264KeyTinyMissingCriticalFeasibilitySuppressedPacketCount();
			stats.h264KeyTinyMissingCriticalFeasibilityBypassedFrames =
				sender
					->GetH264KeyTinyMissingCriticalFeasibilityBypassedFrameCount();
			stats.h264KeyTinyMissingCriticalFeasibilityBypassedPackets =
				sender
					->GetH264KeyTinyMissingCriticalFeasibilityBypassedPacketCount();
			stats.h264KeyTinyMissingCriticalLastPredictedDeliveryMs =
				sender
					->GetH264KeyTinyMissingCriticalLastPredictedDeliveryMs();
			stats.h264KeyTinyMissingCriticalLastRemainingSlackMs =
				sender
					->GetH264KeyTinyMissingCriticalLastRemainingSlackMs();
			stats.h264KeyTinyMissingCriticalLastFrameId =
				sender->GetH264KeyTinyMissingCriticalLastFrameId();
			stats.h264KeyTinyMissingCriticalLastAckMissingChunks =
				sender->GetH264KeyTinyMissingCriticalLastAckMissingChunks();
			stats.h264KeyTinyMissingCriticalLastRequestedChunks =
				sender->GetH264KeyTinyMissingCriticalLastRequestedChunks();
			stats.h264KeyTinyMissingCriticalLastEvent =
				sender->GetH264KeyTinyMissingCriticalLastEvent();
			stats.h264KeySmallMissingAckFrames =
				sender->GetH264KeySmallMissingAckFrameCount();
			stats.h264KeySmallMissingAckMissingChunks =
				sender->GetH264KeySmallMissingAckMissingChunkCount();
			stats.h264KeySmallMissingAckHistoryMissingFrames =
				sender->GetH264KeySmallMissingAckHistoryMissingFrameCount();
			stats.h264KeySmallMissingAckStaleFrameLagFrames =
				sender->GetH264KeySmallMissingAckStaleFrameLagFrameCount();
			stats.h264KeySmallMissingAckStaleAgeFrames =
				sender->GetH264KeySmallMissingAckStaleAgeFrameCount();
			stats.h264KeySmallMissingAckRetransmitBudgetExhaustedFrames =
				sender
					->GetH264KeySmallMissingAckRetransmitBudgetExhaustedFrameCount();
			stats.h264KeySmallMissingAckDynamicBudgetSuppressedFrames =
				sender
					->GetH264KeySmallMissingAckDynamicBudgetSuppressedFrameCount();
			stats.h264KeySmallMissingAckDynamicBudgetSuppressedPackets =
				sender
					->GetH264KeySmallMissingAckDynamicBudgetSuppressedPacketCount();
			stats.h264KeySmallMissingAckSelectedRepairFrames =
				sender->GetH264KeySmallMissingAckSelectedRepairFrameCount();
			stats.h264KeySmallMissingAckSelectedRepairPackets =
				sender->GetH264KeySmallMissingAckSelectedRepairPacketCount();
			stats.h264KeySelectedRepair1To2Frames =
				sender->GetH264KeySelectedRepair1To2FrameCount();
			stats.h264KeySelectedRepair1To2Packets =
				sender->GetH264KeySelectedRepair1To2PacketCount();
			stats.h264KeySelectedRepair3To4Frames =
				sender->GetH264KeySelectedRepair3To4FrameCount();
			stats.h264KeySelectedRepair3To4Packets =
				sender->GetH264KeySelectedRepair3To4PacketCount();
			stats.h264KeySmallMissingAckLastFrameId =
				sender->GetH264KeySmallMissingAckLastFrameId();
			stats.h264KeySmallMissingAckLastMissingChunks =
				sender->GetH264KeySmallMissingAckLastMissingChunks();
			stats.h264KeySmallMissingAckLastGate =
				sender->GetH264KeySmallMissingAckLastGate();
			stats.lateRepairSavedPackets =
				sender->GetLateRepairSavedPacketCount();
			stats.retransmitClassifiedPackets =
				stats.retransmitUsefulChunks +
				stats.retransmitDuplicatePackets +
				stats.retransmitLateAfterCompletedPackets +
				stats.retransmitLateAfterExpiredPackets +
				stats.retransmitLateAfterRejectedPackets;
			const uint64_t retransmitNotArrivedPackets =
				stats.ackRetransmittedChunks >= stats.retransmitClassifiedPackets
				? stats.ackRetransmittedChunks - stats.retransmitClassifiedPackets
				: 0;
			stats.retransmitNotArrivedPackets =
				retransmitNotArrivedPackets;
			stats.retransmitAccountedPackets =
				stats.retransmitClassifiedPackets +
				stats.retransmitNotArrivedPackets;
			stats.retransmitUnclassifiedPackets =
				stats.ackRetransmittedChunks >= stats.retransmitAccountedPackets
				? stats.ackRetransmittedChunks - stats.retransmitAccountedPackets
				: 0;
			if (stats.ackRetransmittedChunks > 0) {
				stats.retransmitFinalClassificationRatio =
					static_cast<double>(stats.retransmitClassifiedPackets) /
					static_cast<double>(stats.ackRetransmittedChunks);
				stats.retransmitFinalAccountingRatio =
					static_cast<double>(stats.retransmitAccountedPackets) /
					static_cast<double>(stats.ackRetransmittedChunks);
			}
			stats.ackStaleDroppedFrames = sender->GetAckStaleDroppedFrameCount();
			stats.ackKeyFrameRequests = sender->GetAckKeyFrameRequestCount();
			const NetworkManager::H264AckKeyFrameRequestTelemetry
				ackKeyFrameTelemetry =
					sender->GetH264AckKeyFrameRequestTelemetry();
			stats.h264KeyFrameRequestAckHistoryMissing =
				ackKeyFrameTelemetry.historyMissing;
			stats.h264KeyFrameRequestAckStaleFrameLag =
				ackKeyFrameTelemetry.staleFrameLag;
			stats.h264KeyFrameRequestAckStaleAge =
				ackKeyFrameTelemetry.staleAge;
			stats.h264KeyFrameRequestAckRetransmitBudgetExhausted =
				ackKeyFrameTelemetry.retransmitBudgetExhausted;
			stats.h264KeyFrameRequestAckHighMissingRate =
				ackKeyFrameTelemetry.highMissingRate;
			stats.h264KeyFrameRequestAckCooldownSuppressed =
				ackKeyFrameTelemetry.cooldownSuppressed;
			stats.h264KeyFrameRequestAckAlreadyPending =
				ackKeyFrameTelemetry.alreadyPending;
			stats.h264KeyFrameRequestAckCooldownNoise =
				ackKeyFrameTelemetry.cooldownNoise;
			stats.h264KeyFrameRequestAckCooldownSyncRisk =
				ackKeyFrameTelemetry.cooldownSyncRisk;
			stats.h264KeyFrameRequestAckStaleAgeCooldownNoise =
				ackKeyFrameTelemetry.staleAgeCooldownNoise;
			stats.h264KeyFrameRequestAckStaleAgeCooldownSyncRisk =
				ackKeyFrameTelemetry.staleAgeCooldownSyncRisk;
			stats.h264KeyFrameRequestAckLastReason =
				ackKeyFrameTelemetry.lastReason;
			stats.ackKeyFramePending = sender->IsKeyFrameRequestPending();
			stats.fecEnabled = sender->IsFecEnabled();
			stats.adaptiveFecEnabled = sender->IsAdaptiveFecEnabled();
			stats.fecGroupChunkCount = sender->GetFecGroupChunkCount();
			const NetworkManager::AdaptiveFecDecisionTelemetry fecDecision =
				sender->GetAdaptiveFecDecisionTelemetry();
			stats.adaptiveFecDecisionReason =
				fecDecision.decisionReason;
			stats.adaptiveFecHoldReason =
				fecDecision.holdReason;
			stats.adaptiveFecG8ToG4Recovery =
				fecDecision.g8ToG4Recovery;
			stats.adaptiveFecEmergencyG2Active =
				fecDecision.emergencyG2Active;
			stats.adaptiveFecEarlyOffReason =
				fecDecision.earlyOffReason;

			const net::PacketPacerStats pacingStats =
				sender->GetPacingStats();
			stats.pacingEnabled = pacingStats.enabled;
			stats.pacingTargetBitrateBps = pacingStats.targetBitrateBps;
			stats.pacingRepairTargetBitrateBps =
				pacingStats.repairTargetBitrateBps;
			stats.pacingQueuedPackets = pacingStats.queuedPackets;
			stats.pacingHighPriorityQueuedPackets =
				pacingStats.highPriorityQueuedPackets;
			stats.pacingNormalQueuedPackets =
				pacingStats.normalQueuedPackets;
			stats.pacingEnqueuedPackets = pacingStats.enqueuedPackets;
			stats.pacingSentPackets = pacingStats.sentPackets;
			stats.pacingSentBytes = pacingStats.sentBytes;
			stats.pacingRepairSentPackets =
				pacingStats.repairSentPackets;
			stats.pacingRepairSentBytes = pacingStats.repairSentBytes;
			stats.pacingRepairBorrowedPackets =
				pacingStats.repairBorrowedPackets;
			stats.pacingRepairBorrowedBytes =
				pacingStats.repairBorrowedBytes;
			stats.pacingEmergencySentPackets =
				pacingStats.emergencySentPackets;
			stats.pacingEmergencySentBytes =
				pacingStats.emergencySentBytes;
			stats.pacingEmergencyBorrowedPackets =
				pacingStats.emergencyBorrowedPackets;
			stats.pacingEmergencyBorrowedBytes =
				pacingStats.emergencyBorrowedBytes;
			stats.h264KeyTinyEmergencyLastQueueAgeMs =
				pacingStats.emergencyLastQueueAgeMs;
			stats.h264KeyTinyEmergencyMaxQueueAgeMs =
				pacingStats.emergencyMaxQueueAgeMs;
			stats.h264KeyTinyEmergencyAvgQueueAgeMs =
				pacingStats.emergencyAvgQueueAgeMs;
			const uint64_t classifiedEmergencyPackets =
				stats.h264KeyTinyEmergencyArrivedBeforeRetirePackets +
				stats.h264KeyTinyEmergencyDuplicateBeforeRetirePackets +
				stats.h264KeyTinyEmergencyArrivedAfterCompletePackets +
				stats.h264KeyTinyEmergencyArrivedAfterExpirePackets +
				stats.h264KeyTinyEmergencyArrivedAfterRejectedPackets;
			stats.h264KeyTinyEmergencyNotArrivedPackets =
				stats.pacingEmergencySentPackets >= classifiedEmergencyPackets
				? stats.pacingEmergencySentPackets -
					classifiedEmergencyPackets
				: 0;
			stats.pacingDroppedPackets = pacingStats.droppedPackets;
			stats.pacingDeadlineDroppedPackets =
				pacingStats.deadlineDroppedPackets;
			stats.pacingHighPriorityDeadlineDroppedPackets =
				pacingStats.highPriorityDeadlineDroppedPackets;
			stats.pacingNormalDeadlineDroppedPackets =
				pacingStats.normalDeadlineDroppedPackets;
			stats.pacingOverflowDroppedPackets =
				pacingStats.overflowDroppedPackets;
			stats.pacingCurrentQueueDelayMs =
				pacingStats.currentQueueDelayMs;
			stats.pacingMaxQueueDelayMs = pacingStats.maxQueueDelayMs;
			stats.pacingVideoCreditBytes = pacingStats.videoCreditBytes;
			stats.pacingRepairCreditBytes = pacingStats.repairCreditBytes;

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
			stats.transportFeedbackSequenceGapPackets =
				feedbackStats.feedbackSequenceGapPackets;
			stats.transportFeedbackSequenceGapRecoveredPackets =
				feedbackStats.feedbackSequenceGapRecoveredPackets;
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

			stats.sendH264VideoBudgetScale =
				adaptiveState.h264VideoBudgetScale;
			stats.pacingBurstGuardActive =
				adaptiveState.lastPacingBurstGuardActive;
			stats.fixedPacingRecentMaxQueueDelayMs =
				adaptiveState.fixedPacingRecentMaxQueueDelayMs;
			stats.fixedPacingQueueReleaseStableSec =
				adaptiveState.fixedPacingQueueReleaseStableSec;
			stats.fixedPacingQueueRecovered =
				adaptiveState.fixedPacingQueueRecovered;
			stats.fixedPacingQueueReleaseEligible =
				adaptiveState.fixedPacingQueueReleaseEligible;
			stats.adaptiveRepairBudgetUtilization =
				adaptiveState.lastRepairBudgetUtilization;
			stats.adaptiveRepairBorrowedRatio =
				adaptiveState.lastRepairBorrowedRatio;
			stats.adaptiveRepairSentBytesDelta =
				adaptiveState.lastRepairSentBytesDelta;
			stats.adaptiveRepairBorrowedBytesDelta =
				adaptiveState.lastRepairBorrowedBytesDelta;
			stats.adaptiveRepairBudgetGuardActive =
				adaptiveState.lastRepairBudgetGuardActive;
			stats.adaptiveRepairVideoBudgetPressure =
				adaptiveState.lastRepairVideoBudgetPressure;
			stats.adaptiveRetransmitUsefulRatio =
				adaptiveState.lastRetransmitUsefulRatio;
			stats.adaptiveLateRepairWasteRatio =
				adaptiveState.lastLateRepairWasteRatio;
			stats.adaptiveRetransmitNotArrivedRatio =
				adaptiveState.lastRetransmitNotArrivedRatio;
			stats.adaptiveRetransmitAccountingComplete =
				adaptiveState.lastRetransmitAccountingComplete;
			stats.adaptiveLateRepairWastePressure =
				adaptiveState.lastLateRepairWastePressure;
			stats.adaptiveRetransmitNotArrivedPressure =
				adaptiveState.lastRetransmitNotArrivedPressure;
			stats.adaptiveRepairDecisionReason =
				adaptiveState.lastRepairDecisionReason;

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
				stats.sendActualCodec =
					sendTelemetry->actualCodec;
				stats.sendActualEncodeWidth =
					sendTelemetry->actualEncodeWidth;
				stats.sendActualEncodeHeight =
					sendTelemetry->actualEncodeHeight;
				stats.sendActualRawFrameBytes =
					sendTelemetry->actualRawFrameBytes;
				stats.sendActualEncodedFrameBytes =
					sendTelemetry->actualEncodedFrameBytes;
				stats.captureFps = sendTelemetry->captureFps;
				stats.cameraCaptureSubtype =
					sendTelemetry->cameraCaptureSubtype;
				stats.cameraCaptureWidth =
					sendTelemetry->cameraCaptureWidth;
				stats.cameraCaptureHeight =
					sendTelemetry->cameraCaptureHeight;
				stats.cameraCaptureFormatFps =
					sendTelemetry->cameraCaptureFormatFps;
				stats.encodeMs = sendTelemetry->encodeMs;
				stats.sendResizeMs = sendTelemetry->resizeMs;
				stats.sendNv12PrepareMs = sendTelemetry->nv12PrepareMs;
				stats.sendH264EncodeMs = sendTelemetry->h264EncodeMs;
				stats.sendH264EncoderRequestedBitrateKbps =
					sendTelemetry->h264EncoderRequestedBitrateKbps;
				stats.sendH264EncoderTargetBitrateKbps =
					sendTelemetry->h264EncoderTargetBitrateKbps;
				stats.sendH264EncoderAppliedBitrateKbps =
					sendTelemetry->h264EncoderAppliedBitrateKbps;
				stats.h264DynamicBitrateUpdateRequests =
					sendTelemetry->h264DynamicBitrateUpdateRequests;
				stats.h264DynamicBitrateUpdateSuccesses =
					sendTelemetry->h264DynamicBitrateUpdateSuccesses;
				stats.h264DynamicBitrateUpdateFailures =
					sendTelemetry->h264DynamicBitrateUpdateFailures;
				stats.h264EncoderReinitializations =
					sendTelemetry->h264EncoderReinitializations;
				stats.sendPacingTargetBitrateKbps =
					sendTelemetry->pacingTargetBitrateKbps;
				stats.h264AuChunkCount =
					sendTelemetry->h264AuChunkCount;
				stats.h264AuIsIdr =
					sendTelemetry->h264AuIsIdr;
				stats.h264AuIsDecoderSync =
					sendTelemetry->h264AuIsDecoderSync;
				stats.h264RequestedKeyFrameConsumedFrames =
					sendTelemetry->h264RequestedKeyFrameConsumedFrames;
				stats.h264EncoderKeyFrameRequests =
					sendTelemetry->h264EncoderKeyFrameRequests;
				stats.h264RecoveryKeyFrameRequests =
					sendTelemetry->h264RecoveryKeyFrameRequests;
				stats.h264AwaitingSyncKeyFrameRequests =
					sendTelemetry->h264AwaitingSyncKeyFrameRequests;
				stats.h264PeriodicIdrRequests =
					sendTelemetry->h264PeriodicIdrRequests;
				stats.h264IdrFrames =
					sendTelemetry->h264IdrFrames;
				stats.h264DecoderSyncFrames =
					sendTelemetry->h264DecoderSyncFrames;
				stats.h264ConsecutiveIdrFrames =
					sendTelemetry->h264ConsecutiveIdrFrames;
				stats.h264MaxConsecutiveIdrFrames =
					sendTelemetry->h264MaxConsecutiveIdrFrames;
				stats.h264AuProtectionLevel =
					sendTelemetry->h264AuProtectionLevel;
				stats.h264AuDroppedBeforeSend =
					sendTelemetry->h264AuDroppedBeforeSend;
				stats.h264AuDropReason =
					sendTelemetry->h264AuDropReason;
				stats.h264AuDroppedBytes =
					sendTelemetry->h264AuDroppedBytes;
				stats.h264AuDroppedChunks =
					sendTelemetry->h264AuDroppedChunks;
				stats.h264AuPacingQueueDelayMs =
					sendTelemetry->h264AuPacingQueueDelayMs;
				stats.h264AuEstimatedSendMs =
					sendTelemetry->h264AuEstimatedSendMs;
				stats.h264InputGatedByPacing =
					sendTelemetry->h264InputGatedByPacing;
				stats.h264InputGateReason =
					sendTelemetry->h264InputGateReason;
				stats.h264InputGateQueueDelayMs =
					sendTelemetry->h264InputGateQueueDelayMs;
				stats.h264InputGateVideoCreditBytes =
					sendTelemetry->h264InputGateVideoCreditBytes;
				stats.h264InputGateFrameBudgetBytes =
					sendTelemetry->h264InputGateFrameBudgetBytes;
				stats.h264InputGateDurationMs =
					sendTelemetry->h264InputGateDurationMs;
				stats.h264InputGateConsecutiveFrames =
					sendTelemetry->h264InputGateConsecutiveFrames;
				stats.h264InputGateSkippedInputFrames =
					sendTelemetry->h264InputGateSkippedInputFrames;
				stats.h264InputGateForcedOpen =
					sendTelemetry->h264InputGateForcedOpen;
				stats.h264InputGateReleaseReason =
					sendTelemetry->h264InputGateReleaseReason;
				stats.fecProtectedH264KeyFrames =
					sendTelemetry->fecProtectedH264KeyFrames;
				stats.fecProtectedH264LargeFrames =
					sendTelemetry->fecProtectedH264LargeFrames;
				stats.h264EncoderDelayFrames =
					sendTelemetry->h264EncoderDelayFrames;
				stats.h264EncoderDelayMs =
					sendTelemetry->h264EncoderDelayMs;
				stats.h264EncoderPendingFrames =
					sendTelemetry->h264EncoderPendingFrames;
				stats.h264EncodedInputFrameId =
					sendTelemetry->h264EncodedInputFrameId;
				stats.h264EncoderCallMs =
					sendTelemetry->h264EncoderCallMs;
				stats.h264EncoderSampleCreateMs =
					sendTelemetry->h264EncoderSampleCreateMs;
				stats.h264EncoderProcessInputMs =
					sendTelemetry->h264EncoderProcessInputMs;
				stats.h264EncoderPreInputPollMs =
					sendTelemetry->h264EncoderPreInputPollMs;
				stats.h264EncoderPostInputWaitMs =
					sendTelemetry->h264EncoderPostInputWaitMs;
				stats.h264EncoderProcessOutputMs =
					sendTelemetry->h264EncoderProcessOutputMs;
				stats.h264EncoderOutputCopyMs =
					sendTelemetry->h264EncoderOutputCopyMs;
				stats.h264EncoderProcessOutputAttempts =
					sendTelemetry->h264EncoderProcessOutputAttempts;
				stats.h264EncoderAsyncEventCount =
					sendTelemetry->h264EncoderAsyncEventCount;
				stats.h264EncoderHardware =
					sendTelemetry->h264EncoderHardware;
				stats.h264EncoderAsync =
					sendTelemetry->h264EncoderAsync;
				stats.h264EncoderNeedInputSignaled =
					sendTelemetry->h264EncoderNeedInputSignaled;
				stats.h264EncoderOutputProduced =
					sendTelemetry->h264EncoderOutputProduced;
				stats.h264EncoderOutputProducedBeforeInput =
					sendTelemetry->h264EncoderOutputProducedBeforeInput;
				stats.h264SubmittedNewInput =
					sendTelemetry->h264SubmittedNewInput;
				stats.h264AsyncSubmittedWithoutOutput =
					sendTelemetry->h264AsyncSubmittedWithoutOutput;
				stats.h264AsyncPendingNoOutput =
					sendTelemetry->h264AsyncPendingNoOutput;
				stats.h264AsyncCadenceHoldActive =
					sendTelemetry->h264AsyncCadenceHoldActive;
				stats.h264AsyncPendingNoOutputStreak =
					sendTelemetry->h264AsyncPendingNoOutputStreak;
				stats.h264AsyncCadenceScale =
					sendTelemetry->h264AsyncCadenceScale;
				stats.h264AsyncCadenceHoldRemainingMs =
					sendTelemetry->h264AsyncCadenceHoldRemainingMs;
				stats.h264AsyncOutputPollBackoffMs =
					sendTelemetry->h264AsyncOutputPollBackoffMs;
				stats.h264InputCadenceFps =
					sendTelemetry->h264InputCadenceFps;
				stats.h264NeedInputSubmitWake =
					sendTelemetry->h264NeedInputSubmitWake;
				stats.h264NeedInputSubmitLeadMs =
					sendTelemetry->h264NeedInputSubmitLeadMs;
				stats.encodedCameraFrameId =
					sendTelemetry->encodedCameraFrameId;
				stats.encodedCameraSourceTimestamp100ns =
					sendTelemetry->encodedCameraSourceTimestamp100ns;
				stats.encodedCameraCaptureCompletedTimeUs =
					sendTelemetry->encodedCameraCaptureCompletedTimeUs;
				stats.encodedCameraFrameAgeMs =
					sendTelemetry->encodedCameraFrameAgeMs;
				stats.encodedCameraReadSampleMs =
					sendTelemetry->encodedCameraReadSampleMs;
				stats.encodedCameraReadSampleEndToCaptureMs =
					sendTelemetry->encodedCameraReadSampleEndToCaptureMs;
				stats.encodedCameraCaptureToPublishMs =
					sendTelemetry->encodedCameraCaptureToPublishMs;
				stats.encodedCameraPublishToAcquireMs =
					sendTelemetry->encodedCameraPublishToAcquireMs;
				stats.encodedCameraAcquireToEncoderInputMs =
					sendTelemetry->encodedCameraAcquireToEncoderInputMs;
				stats.sendJpegEncodeMs = sendTelemetry->jpegEncodeMs;
				stats.sendPacketizeMs = sendTelemetry->packetizeMs;
				stats.sendFrameIntervalMs =
					sendTelemetry->sendFrameIntervalMs;
				stats.cameraFrameReady =
					sendTelemetry->cameraFrameReady;
				stats.cameraFrameId =
					sendTelemetry->cameraFrameId;
				stats.cameraSourceTimestamp100ns =
					sendTelemetry->cameraSourceTimestamp100ns;
				stats.cameraReadSampleStartTimeUs =
					sendTelemetry->cameraReadSampleStartTimeUs;
				stats.cameraReadSampleEndTimeUs =
					sendTelemetry->cameraReadSampleEndTimeUs;
				stats.cameraCaptureCompletedTimeUs =
					sendTelemetry->cameraCaptureCompletedTimeUs;
				stats.cameraFramePublishedTimeUs =
					sendTelemetry->cameraFramePublishedTimeUs;
				stats.cameraSenderAcquireTimeUs =
					sendTelemetry->cameraSenderAcquireTimeUs;
				stats.cameraReadSampleMs =
					sendTelemetry->cameraReadSampleMs;
				stats.cameraReadSampleEndToCaptureMs =
					sendTelemetry->cameraReadSampleEndToCaptureMs;
				stats.cameraCaptureToPublishMs =
					sendTelemetry->cameraCaptureToPublishMs;
				stats.cameraPublishToAcquireMs =
					sendTelemetry->cameraPublishToAcquireMs;
				stats.cameraAcquireToSendMs =
					sendTelemetry->cameraAcquireToSendMs;
				stats.cameraFrameAgeMs =
					sendTelemetry->cameraFrameAgeMs;
				stats.cameraFrameCacheUsed =
					sendTelemetry->cameraFrameCacheUsed;
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

			stats.adaptiveRecoveryDeadlineRawDelta =
				adaptiveState.lastRecoveryDeadlineRawDelta;
			stats.adaptiveRecoveryDeadlineEffectiveDelta =
				adaptiveState.lastRecoveryDeadlineEffectiveDelta;
			stats.adaptiveRetransmitStaleRawDelta =
				adaptiveState.lastRetransmitStaleRawDelta;
			stats.adaptiveRetransmitStaleEffectiveDelta =
				adaptiveState.lastRetransmitStaleEffectiveDelta;
			stats.adaptiveRecoveryDeadlineNoiseDelta =
				adaptiveState.lastRecoveryDeadlineNoiseDelta;
			stats.adaptiveRecoveryDeadlineSyncRiskDelta =
				adaptiveState.lastRecoveryDeadlineSyncRiskDelta;
			stats.adaptiveRecoveryDeadlineHardSyncLossDelta =
				adaptiveState.lastRecoveryDeadlineHardSyncLossDelta;
			stats.adaptiveRecoveryDeadlineSyncEvidenceActive =
				adaptiveState.lastRecoveryDeadlineSyncEvidenceActive;
			stats.adaptiveRecoveryDeadlineSyncEvidenceAgeMs =
				adaptiveState.lastRecoveryDeadlineSyncEvidenceAgeMs;
			stats.adaptiveRecoveryDeadlineSyncEvidenceSource =
				adaptiveState.lastRecoveryDeadlineSyncEvidenceSource;

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
			stats.adaptiveFecQualityHoldActive =
				adaptiveState.lastAdaptiveFecQualityHoldActive;
			stats.adaptiveFecQualityHoldCanceled =
				adaptiveState.lastAdaptiveFecQualityHoldCanceled;
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
			stats.receiveDecodePopSuccesses =
				videoReceiverStats.decodePopSuccesses;
			stats.receiveDecodePopEmptyPolls =
				videoReceiverStats.decodePopEmptyPolls;
			stats.receiveDecodeLoopLastPopGapMs =
				videoReceiverStats.decodeLoopLastPopGapMs;
			stats.receiveDecodeLoopMaxPopGapMs =
				videoReceiverStats.decodeLoopMaxPopGapMs;
			stats.receiveStartupDecodeLoopMaxPopGapMs =
				videoReceiverStats.startupDecodeLoopMaxPopGapMs;
			stats.receiveSteadyDecodeLoopMaxPopGapMs =
				videoReceiverStats.steadyDecodeLoopMaxPopGapMs;
			stats.receiveSteadyDecodeLoopMaxPopGapAtMs =
				videoReceiverStats.steadyDecodeLoopMaxPopGapAtMs;
			stats.receiveSteadyDecodeLoopMaxPopGapFrameId =
				videoReceiverStats.steadyDecodeLoopMaxPopGapFrameId;
			stats.receiveSteadyDecodeLoopMaxPopGapStreamId =
				videoReceiverStats.steadyDecodeLoopMaxPopGapStreamId;
			stats.receiveSteadyDecodeLoopMaxPopGapCodec =
				videoReceiverStats.steadyDecodeLoopMaxPopGapCodec;
			stats.receiveSteadyDecodeLoopMaxPopGapInputFrameAgeMs =
				videoReceiverStats.steadyDecodeLoopMaxPopGapInputFrameAgeMs;
			stats.receiveSteadyDecodeLoopMaxPopGapDecodedQueueSize =
				videoReceiverStats.steadyDecodeLoopMaxPopGapDecodedQueueSize;
			stats.receiveSteadyDecodeLoopMaxPopGapCompletedQueueSize =
				videoReceiverStats.steadyDecodeLoopMaxPopGapCompletedQueueSize;
			stats.receiveSteadyDecodeLoopMaxPopGapCompletedQueuePopAgeMs =
				videoReceiverStats.steadyDecodeLoopMaxPopGapCompletedQueuePopAgeMs;
			stats.receiveSteadyDecodeLoopMaxPopGapCompletedQueuePushIntervalMs =
				videoReceiverStats
					.steadyDecodeLoopMaxPopGapCompletedQueuePushIntervalMs;
			stats.receiveSteadyDecodeLoopMaxPopGapArrivalRatio =
				videoReceiverStats.steadyDecodeLoopMaxPopGapArrivalRatio;
			stats.receiveSteadyDecodeLoopMaxPopGapReceiverJitterMs =
				videoReceiverStats.steadyDecodeLoopMaxPopGapReceiverJitterMs;
			stats.receiveSteadyDecodeLoopMaxPopGapReceiverLatencyMs =
				videoReceiverStats.steadyDecodeLoopMaxPopGapReceiverLatencyMs;
			stats.receiveSteadyDecodeLoopMaxPopGapClass =
				videoReceiverStats.steadyDecodeLoopMaxPopGapClass;
			stats.receiveH264StartupActive =
				videoReceiverStats.startupActive;
			stats.receiveH264StartupDecoderSynced =
				videoReceiverStats.startupDecoderSynced;
			stats.receiveH264StartupFirstDecoded =
				videoReceiverStats.startupFirstDecoded;
			stats.receiveH264StartupFirstDisplayed =
				videoReceiverStats.startupFirstDisplayed;
			stats.receiveH264StartupReady =
				videoReceiverStats.startupReady;
			stats.receiveH264StartupElapsedMs =
				videoReceiverStats.startupElapsedMs;
			if (stats.receiveH264StartupElapsedMs > 0.0 &&
				stats.receiveSteadyDecodeLoopMaxPopGapAtMs > 0.0) {
				stats.receiveSteadyDecodeLoopMaxPopGapAgeMs =
					stats.receiveH264StartupElapsedMs -
					stats.receiveSteadyDecodeLoopMaxPopGapAtMs;
			}
			else {
				stats.receiveSteadyDecodeLoopMaxPopGapAgeMs = -1.0;
			}
			stats.adaptiveArrivalGapJitterSpikeActive =
				stats.receiveSteadyDecodeLoopMaxPopGapClass ==
				"arrival-gap-jitter-spike" &&
				stats.receiveSteadyDecodeLoopMaxPopGapMs >= 20.0 &&
				stats.receiveSteadyDecodeLoopMaxPopGapArrivalRatio >= 0.70 &&
				stats.receiveSteadyDecodeLoopMaxPopGapReceiverJitterMs >= 20.0 &&
				stats.receiveSteadyDecodeLoopMaxPopGapAgeMs >= 0.0 &&
				stats.receiveSteadyDecodeLoopMaxPopGapAgeMs <= 1500.0;
			stats.receiveH264StartupDecoderSyncMs =
				videoReceiverStats.startupDecoderSyncMs;
			stats.receiveH264StartupFirstDecodedMs =
				videoReceiverStats.startupFirstDecodedMs;
			stats.receiveH264StartupFirstDisplayedMs =
				videoReceiverStats.startupFirstDisplayedMs;
			stats.receiveH264StartupReadyMs =
				videoReceiverStats.startupReadyMs;
			stats.receiveH264StartupQueueFlushFrames =
				videoReceiverStats.startupQueueFlushFrames;
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
			stats.receiveH264AuInvalidFrames =
				videoReceiverStats.h264AuInvalidFrames;
			stats.receiveH264AuCrcMismatches =
				videoReceiverStats.h264AuCrcMismatches;
			stats.receiveH264AuPayloadSizeMismatches =
				videoReceiverStats.h264AuPayloadSizeMismatches;
			stats.receiveH264AuNalCountMismatches =
				videoReceiverStats.h264AuNalCountMismatches;
			stats.receiveH264AuNoAnnexBNals =
				videoReceiverStats.h264AuNoAnnexBNals;
			stats.receiveH264AuIdrFlagMismatches =
				videoReceiverStats.h264AuIdrFlagMismatches;
			stats.receiveH264AuSpsPpsFlagMismatches =
				videoReceiverStats.h264AuSpsPpsFlagMismatches;
			stats.receiveH264AuSyncWithoutIdr =
				videoReceiverStats.h264AuSyncWithoutIdr;
			stats.receiveH264AuIdrWithoutSpsPps =
				videoReceiverStats.h264AuIdrWithoutSpsPps;
			stats.receiveH264AuForbiddenZeroBit =
				videoReceiverStats.h264AuForbiddenZeroBit;
			stats.receiveH264AuLastInvalidReason =
				videoReceiverStats.h264AuLastInvalidReason;
			stats.receiveDecodeInputFrameAgeMs =
				videoReceiverStats.decodeInputFrameAgeMs;
			stats.receiveDecodeInputCameraFrameAgeMs =
				videoReceiverStats.decodeInputCameraFrameAgeMs;
			stats.receiveDecodeInputEncoderOutputAgeMs =
				videoReceiverStats.decodeInputEncoderOutputAgeMs;
			stats.receiveLatestDecodedFrameAgeMs =
				videoReceiverStats.latestDecodedFrameAgeMs;
			stats.receiveLatestDecodedCameraFrameAgeMs =
				videoReceiverStats.latestDecodedCameraFrameAgeMs;
			stats.receiveLatestDecodedEncoderOutputAgeMs =
				videoReceiverStats.latestDecodedEncoderOutputAgeMs;
			stats.receiveFreshnessDropThresholdMs =
				videoReceiverStats.freshnessDropThresholdMs;
			stats.receiveDecodeLastDropReason =
				videoReceiverStats.lastDropReason;
			stats.receiveLastFreshnessDropAgeMs =
				videoReceiverStats.lastFreshnessDropAgeMs;
			stats.receiveMaxFreshnessDropAgeMs =
				videoReceiverStats.maxFreshnessDropAgeMs;
			stats.receiveLastFreshnessDropFrameId =
				videoReceiverStats.lastFreshnessDropFrameId;
			stats.receiveLastFreshnessDropStreamId =
				videoReceiverStats.lastFreshnessDropStreamId;
			stats.receiveLastFreshnessDropCodec =
				videoReceiverStats.lastFreshnessDropCodec;
		}

		return stats;
		};

	runLoop.SetNetworkStatsProvider(collectNetworkStats);
	TraceStartup("network-stats-provider-set");

	net::NetworkCsvLogger networkCsvLogger;
	TraceStartup("network-csv-start-before");
	if (networkCsvLogger.Start("logs") ||
		networkCsvLogger.Start("network_logs")) {
		std::cout << "[AppMain] Network CSV logging started: "
			<< networkCsvLogger.FilePath()
			<< "\n";
		TraceStartup("network-csv-start-ok");
	}
	else {
		std::cerr << "[AppMain] Network CSV logging failed to start.\n";
		TraceStartup("network-csv-start-failed");
	}
	const auto networkCsvStartTime = std::chrono::steady_clock::now();
	auto lastNetworkCsvSampleTime = networkCsvStartTime - std::chrono::seconds(1);
	net::NetworkExperimentReporter networkExperimentReporter;
	auto startNetworkExperimentReporter = [&]() -> bool {
		if (networkExperimentReporter.IsRunning()) {
			return true;
		}
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
			return true;
		}

		std::cerr << "[AppMain] Network experiment summary failed to start.\n";
		return false;
	};
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
		receivedUavHandleGPU,
		receivedSrvHandleGPU,
		receivedTexWidth,
		receivedTexHeight
	);
	runLoop.SetReceivedVideoNv12Textures(
		receivedNv12YTexture,
		std::move(receivedNv12YUploadBuffers),
		receivedNv12YSrvHandleGPU,
		receivedNv12UVTexture,
		std::move(receivedNv12UVUploadBuffers),
		receivedNv12UVSrvHandleGPU
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
			h264ScaleFilter,
			texWidth,
			texHeight
		]() {
		auto nextSendTime = std::chrono::steady_clock::now();
		uint32_t frameId = 1;
		uint64_t lastCameraFrameId = 0;
		std::vector<uint8_t> lastCameraFrameCache;
		CameraCapture::Nv12Frame lastCameraNv12FrameCache;
		CameraCapture::FrameMetadata lastCameraFrameMetadata{};
		H264Encoder h264Encoder;
		bool h264EncoderReady = false;
		uint32_t h264EncoderWidth = 0;
		uint32_t h264EncoderHeight = 0;
		uint32_t h264EncoderBitrate = 0;
		uint32_t h264EncoderFps = 0;
		H264EncoderBitrateHysteresisState h264BitrateHysteresis;
		uint64_t h264DynamicBitrateUpdateRequests = 0;
		uint64_t h264DynamicBitrateUpdateSuccesses = 0;
		uint64_t h264DynamicBitrateUpdateFailures = 0;
		uint64_t h264EncoderReinitializations = 0;
		uint64_t fecProtectedH264KeyFrames = 0;
		uint64_t fecProtectedH264LargeFrames = 0;
		uint64_t h264RequestedKeyFrameConsumedFrames = 0;
		uint64_t h264EncoderKeyFrameRequests = 0;
		uint64_t h264RecoveryKeyFrameRequests = 0;
		uint64_t h264AwaitingSyncKeyFrameRequests = 0;
		uint64_t h264PeriodicIdrRequests = 0;
		uint64_t h264IdrFrames = 0;
		uint64_t h264DecoderSyncFrames = 0;
		uint32_t h264ConsecutiveIdrFrames = 0;
		uint32_t h264MaxConsecutiveIdrFrames = 0;
		bool h264DecoderSyncSent = false;
		bool h264AwaitingDecoderSync = true;
		std::chrono::steady_clock::time_point h264LastKeyProtectionTime{};
		std::vector<uint8_t> h264Nv12Frame;
		struct H264PendingInputFrame {
			uint64_t sequence = 0;
			uint32_t inputFrameId = 0;
			uint64_t cameraFrameId = 0;
			int64_t cameraSourceTimestamp100ns = 0;
			uint64_t cameraCaptureCompletedTimeUs = 0;
			double cameraReadSampleMs = 0.0;
			double cameraReadSampleEndToCaptureMs = 0.0;
			double cameraCaptureToPublishMs = 0.0;
			double cameraPublishToAcquireMs = 0.0;
			uint64_t encoderInputTimeUs = 0;
			double cameraAcquireToEncoderInputMs = 0.0;
		};
		std::queue<H264PendingInputFrame> h264PendingInputFrames;
		uint64_t h264InputSequence = 0;
		uint32_t h264PostKeyFrameSkipFrames = 0;
		bool h264PacingInputGateActive = false;
		std::chrono::steady_clock::time_point h264PacingInputGateStartTime{};
		uint32_t h264PacingInputGateConsecutiveFrames = 0;
		uint64_t h264PacingInputGateSkippedInputFrames = 0;
		std::chrono::steady_clock::time_point h264AsyncCadenceHoldUntil{};
		uint32_t h264AsyncPendingNoOutputStreak = 0;
		auto steadyNowUs = []() {
			return static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()
				).count());
		};

		auto resetH264Encoder = [&]() {
			if (h264EncoderReady) {
				h264Encoder.Shutdown();
			}
			h264EncoderReady = false;
			h264EncoderWidth = 0;
			h264EncoderHeight = 0;
			h264EncoderBitrate = 0;
			h264EncoderFps = 0;
			h264DecoderSyncSent = false;
			h264AwaitingDecoderSync = true;
			h264PendingInputFrames = {};
			h264InputSequence = 0;
			h264AsyncCadenceHoldUntil = {};
			h264AsyncPendingNoOutputStreak = 0;
			h264LastKeyProtectionTime = {};
			h264ConsecutiveIdrFrames = 0;
		};

		while (videoSenderRunning.load()) {
			const auto now = std::chrono::steady_clock::now();
			bool dueForScheduledInput = now >= nextSendTime;
			const bool h264OutputPumpPending =
				runtimeState.networkVideoCodec == net::CodecType::H264 &&
				h264EncoderReady &&
				h264Encoder.IsAsyncHardware() &&
				!h264PendingInputFrames.empty();
			const bool h264CameraPublishWakeEnabled =
				runtimeState.networkVideoCodec == net::CodecType::H264 &&
				cameraCaptureEnabled &&
				cameraCapture != nullptr;
			if (now < nextSendTime) {
				if (!h264OutputPumpPending) {
					bool wokeForCameraPublish = false;
					if (h264CameraPublishWakeEnabled) {
						wokeForCameraPublish =
							cameraCapture->WaitForFrameAfter(
								lastCameraFrameId,
								nextSendTime);
					}
					else {
						std::this_thread::sleep_until(nextSendTime);
					}
					const auto wakeNow = std::chrono::steady_clock::now();
					if (wokeForCameraPublish) {
						nextSendTime = wakeNow;
						dueForScheduledInput = true;
					}
					else {
						dueForScheduledInput = wakeNow >= nextSendTime;
						if (!dueForScheduledInput) {
							continue;
						}
					}
				}
				else {
					const auto pumpWakeTime =
						(std::min)(
							nextSendTime,
							now + std::chrono::milliseconds(1));
					std::this_thread::sleep_until(pumpWakeTime);
					dueForScheduledInput =
						std::chrono::steady_clock::now() >= nextSendTime;
				}
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
			int inputCadenceFps = targetFps;
			bool h264AsyncCadenceHoldActive = false;
			double h264AsyncCadenceScale = 1.0;
			double h264AsyncCadenceHoldRemainingMs = 0.0;
			if (runtimeState.networkVideoCodec == net::CodecType::H264 &&
				adaptiveState.h264VideoBudgetScale < 0.999) {
				const bool latencyFreshnessCadence =
					adaptiveState.lastRepairDecisionReason ==
					"latency-pressure-freshness" ||
					adaptiveState.lastRepairDecisionReason ==
					"latency-pressure-freshness-hard" ||
					adaptiveState.lastRepairDecisionReason ==
					"latency-pressure-freshness-hold";
				const double cadenceScale =
					latencyFreshnessCadence
					? ReadEnvDoubleClamped(
						"RNVP_H264_LATENCY_FRESHNESS_CADENCE_SCALE",
						0.90,
						0.50,
						1.0)
					: (adaptiveState.h264VideoBudgetScale <= 0.70
						? ReadEnvDoubleClamped(
							"RNVP_H264_QUEUE_HARD_CADENCE_SCALE",
							0.67,
							0.35,
							1.0)
						: ReadEnvDoubleClamped(
							"RNVP_H264_QUEUE_CADENCE_SCALE",
							0.80,
							0.35,
							1.0));
				inputCadenceFps =
					std::clamp(
						static_cast<int>(
							std::lround(
								static_cast<double>(targetFps) *
								cadenceScale)),
						10,
						targetFps);
			}
			if (runtimeState.networkVideoCodec == net::CodecType::H264) {
				h264AsyncCadenceHoldActive =
					h264AsyncCadenceHoldUntil.time_since_epoch().count() != 0 &&
					now < h264AsyncCadenceHoldUntil;
				if (h264AsyncCadenceHoldActive) {
					h264AsyncCadenceHoldRemainingMs =
						std::chrono::duration<double, std::milli>(
							h264AsyncCadenceHoldUntil - now).count();
				}
				if (h264AsyncCadenceHoldActive ||
					h264AsyncPendingNoOutputStreak > 0) {
					h264AsyncCadenceScale =
						h264AsyncPendingNoOutputStreak >= 2
						? ReadEnvDoubleClamped(
							"RNVP_H264_ASYNC_HARD_CADENCE_SCALE",
							0.72,
							0.35,
							1.0)
						: ReadEnvDoubleClamped(
							"RNVP_H264_ASYNC_CADENCE_SCALE",
							0.86,
							0.35,
							1.0);
					inputCadenceFps =
						std::clamp(
						static_cast<int>(
							std::lround(
								static_cast<double>(targetFps) *
								h264AsyncCadenceScale)),
						10,
						inputCadenceFps);
				}
			}
			const auto sendInterval =
				std::chrono::duration<double>(
					1.0 / static_cast<double>(inputCadenceFps)
				);
			bool h264NeedInputSubmitWake = false;
			double h264NeedInputSubmitLeadMs = 0.0;
			if (runtimeState.networkVideoCodec == net::CodecType::H264 &&
				h264EncoderReady &&
				h264Encoder.IsAsyncHardware() &&
				!dueForScheduledInput &&
				!h264PendingInputFrames.empty() &&
				h264Encoder.CanAcceptInput()) {
				const auto needInputNow = std::chrono::steady_clock::now();
				if (nextSendTime > needInputNow) {
					h264NeedInputSubmitLeadMs =
						std::chrono::duration<double, std::milli>(
							nextSendTime - needInputNow).count();
				}
				const double maxNeedInputLeadMs =
					ReadEnvDoubleClamped(
						"RNVP_H264_NEED_INPUT_EARLY_SUBMIT_MS",
						12.0,
						0.0,
						33.0);
				h264NeedInputSubmitWake =
					h264NeedInputSubmitLeadMs <= maxNeedInputLeadMs;
			}
			if (dueForScheduledInput &&
				h264PostKeyFrameSkipFrames > 0 &&
				runtimeState.networkVideoCodec == net::CodecType::H264) {
				h264PostKeyFrameSkipFrames--;
				dueForScheduledInput = false;
				nextSendTime +=
					std::chrono::duration_cast<
						std::chrono::steady_clock::duration
					>(sendInterval);
			}

			const uint32_t adaptiveTargetBitrateKbps =
				static_cast<uint32_t>(
					(std::max)(1, adaptiveState.targetBitrateKbps)
				);
			const uint32_t pacingTargetBitrateKbps =
				adaptiveTargetBitrateKbps;
			networkManager->SetPacingTargetBitrateKbps(
				pacingTargetBitrateKbps
			);
			const uint32_t h264EncoderRequestedBitrateKbps =
				ResolveH264EncoderTargetBitrateKbps(
					adaptiveTargetBitrateKbps,
					networkManager->IsFecEnabled(),
					networkManager->GetFecGroupChunkCount(),
					adaptiveState.h264VideoBudgetScale);
			const uint32_t h264EncoderTargetBitrateKbps =
				ApplyH264EncoderBitrateHysteresis(
					h264EncoderRequestedBitrateKbps,
					h264BitrateHysteresis,
					std::chrono::steady_clock::now(),
					!h264EncoderReady);

			uint32_t targetWidth =
				static_cast<uint32_t>(
					std::clamp(
						adaptiveState.targetWidth,
						160,
						static_cast<int>(texWidth)
					)
				);
			uint32_t targetHeight =
				static_cast<uint32_t>(
					std::clamp(
						adaptiveState.targetHeight,
						90,
						static_cast<int>(texHeight)
					)
				);
			targetWidth = (std::max<uint32_t>)(2u, targetWidth & ~1u);
			targetHeight = (std::max<uint32_t>)(2u, targetHeight & ~1u);
			if (runtimeState.networkVideoCodec == net::CodecType::H264) {
				const uint32_t h264MaxWidth =
					ReadEnvUInt32Clamped(
						"RNVP_H264_MAX_WIDTH",
						kDefaultH264LowLatencyMaxWidth,
						160u,
						texWidth);
				const uint32_t h264MaxHeight =
					ReadEnvUInt32Clamped(
						"RNVP_H264_MAX_HEIGHT",
						kDefaultH264LowLatencyMaxHeight,
						90u,
						texHeight);
				targetWidth =
					(std::max<uint32_t>)(
						2u,
						((std::min)(targetWidth, h264MaxWidth)) & ~1u);
				targetHeight =
					(std::max<uint32_t>)(
						2u,
						((std::min)(targetHeight, h264MaxHeight)) & ~1u);
			}

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
			CameraCapture::Nv12Frame cameraNv12Frame;
			CameraCapture::FrameMetadata cameraFrameMetadata{};
			const bool wantsH264 =
				runtimeState.networkVideoCodec == net::CodecType::H264;
			const bool allowCameraAcquire =
				!wantsH264 ||
				dueForScheduledInput ||
				h264NeedInputSubmitWake;
			bool hasCameraNv12Frame = false;
			bool hasCameraRgbaFrame = false;
			if (allowCameraAcquire && cameraCaptureEnabled && cameraCapture) {
				if (wantsH264) {
					hasCameraNv12Frame =
						cameraCapture->TryGetLatestNv12Frame(cameraNv12Frame);
					if (hasCameraNv12Frame) {
						cameraFrameMetadata = cameraNv12Frame.metadata;
					}
				}
				if (!hasCameraNv12Frame) {
					hasCameraRgbaFrame =
						cameraCapture->TryGetLatestRgbaFrame(
							videoFrame,
							cameraFrameMetadata
						);
				}
			}
			bool hasCameraFrame =
				hasCameraNv12Frame || hasCameraRgbaFrame;
			uint64_t cameraFrameId = cameraFrameMetadata.frameId;
			bool freshCameraFrame =
				hasCameraFrame &&
				cameraFrameId != lastCameraFrameId;
			if (hasCameraFrame) {
				lastCameraFrameId = cameraFrameId;
				lastCameraFrameMetadata = cameraFrameMetadata;
				if (hasCameraNv12Frame) {
					lastCameraNv12FrameCache = cameraNv12Frame;
					lastCameraFrameCache.clear();
				}
				else {
					lastCameraFrameCache = videoFrame;
					lastCameraNv12FrameCache = {};
				}
			}

			bool usedCameraCache = false;
			bool selectedCameraNv12Frame = hasCameraNv12Frame;
			if (!hasCameraFrame &&
				cameraCaptureEnabled &&
				!wantsH264 &&
				!lastCameraNv12FrameCache.yPlane.empty() &&
				!lastCameraNv12FrameCache.uvPlane.empty()) {
				cameraNv12Frame = lastCameraNv12FrameCache;
				cameraFrameMetadata = lastCameraFrameMetadata;
				cameraFrameId = cameraFrameMetadata.frameId;
				selectedCameraNv12Frame = true;
				usedCameraCache = true;
			}
			else if (!hasCameraFrame &&
				cameraCaptureEnabled &&
				!wantsH264 &&
				!lastCameraFrameCache.empty()) {
				videoFrame = lastCameraFrameCache;
				cameraFrameMetadata = lastCameraFrameMetadata;
				cameraFrameId = cameraFrameMetadata.frameId;
				selectedCameraNv12Frame = false;
				usedCameraCache = true;
			}
			else if (!hasCameraFrame &&
				cameraCaptureEnabled &&
				!wantsH264) {
				const auto sendInterval =
					std::chrono::duration_cast<
						std::chrono::steady_clock::duration>(
							std::chrono::duration<double>(
								1.0 / static_cast<double>(targetFps)
							));
				nextSendTime += sendInterval;
				continue;
			}
			bool reusedCameraFrame =
				cameraCaptureEnabled &&
				cameraFrameId != 0 &&
				(!hasCameraFrame || !freshCameraFrame || usedCameraCache);
			const uint64_t cameraAgeNowUs =
				static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now().time_since_epoch()
					).count());
			double cameraFrameAgeMs =
				cameraFrameMetadata.captureCompletedTimeUs != 0 &&
				cameraAgeNowUs >= cameraFrameMetadata.captureCompletedTimeUs
				? static_cast<double>(
					cameraAgeNowUs -
					cameraFrameMetadata.captureCompletedTimeUs) /
					1000.0
				: 0.0;
			double cameraReadSampleEndToCaptureMs =
				cameraFrameMetadata.readSampleEndTimeUs != 0 &&
				cameraFrameMetadata.captureCompletedTimeUs != 0 &&
				cameraFrameMetadata.captureCompletedTimeUs >=
					cameraFrameMetadata.readSampleEndTimeUs
				? static_cast<double>(
					cameraFrameMetadata.captureCompletedTimeUs -
					cameraFrameMetadata.readSampleEndTimeUs) /
					1000.0
				: 0.0;
			double cameraCaptureToPublishMs =
				cameraFrameMetadata.captureCompletedTimeUs != 0 &&
				cameraFrameMetadata.framePublishedTimeUs != 0 &&
				cameraFrameMetadata.framePublishedTimeUs >=
					cameraFrameMetadata.captureCompletedTimeUs
				? static_cast<double>(
					cameraFrameMetadata.framePublishedTimeUs -
					cameraFrameMetadata.captureCompletedTimeUs) /
					1000.0
				: 0.0;
			double cameraPublishToAcquireMs =
				cameraFrameMetadata.framePublishedTimeUs != 0 &&
				cameraFrameMetadata.senderAcquireTimeUs != 0 &&
				cameraFrameMetadata.senderAcquireTimeUs >=
					cameraFrameMetadata.framePublishedTimeUs
				? static_cast<double>(
					cameraFrameMetadata.senderAcquireTimeUs -
					cameraFrameMetadata.framePublishedTimeUs) /
					1000.0
				: 0.0;
			double cameraAcquireToSendMs =
				cameraFrameMetadata.senderAcquireTimeUs != 0 &&
				cameraAgeNowUs >= cameraFrameMetadata.senderAcquireTimeUs
				? static_cast<double>(
					cameraAgeNowUs -
					cameraFrameMetadata.senderAcquireTimeUs) /
					1000.0
				: 0.0;
			auto refreshCameraTimingMetrics = [&]() {
				const uint64_t nowUs =
					static_cast<uint64_t>(
						std::chrono::duration_cast<std::chrono::microseconds>(
							std::chrono::steady_clock::now().time_since_epoch()
						).count());
				cameraFrameAgeMs =
					cameraFrameMetadata.captureCompletedTimeUs != 0 &&
					nowUs >= cameraFrameMetadata.captureCompletedTimeUs
					? static_cast<double>(
						nowUs - cameraFrameMetadata.captureCompletedTimeUs) /
						1000.0
					: 0.0;
				cameraReadSampleEndToCaptureMs =
					cameraFrameMetadata.readSampleEndTimeUs != 0 &&
					cameraFrameMetadata.captureCompletedTimeUs != 0 &&
					cameraFrameMetadata.captureCompletedTimeUs >=
						cameraFrameMetadata.readSampleEndTimeUs
					? static_cast<double>(
						cameraFrameMetadata.captureCompletedTimeUs -
						cameraFrameMetadata.readSampleEndTimeUs) /
						1000.0
					: 0.0;
				cameraCaptureToPublishMs =
					cameraFrameMetadata.captureCompletedTimeUs != 0 &&
					cameraFrameMetadata.framePublishedTimeUs != 0 &&
					cameraFrameMetadata.framePublishedTimeUs >=
						cameraFrameMetadata.captureCompletedTimeUs
					? static_cast<double>(
						cameraFrameMetadata.framePublishedTimeUs -
						cameraFrameMetadata.captureCompletedTimeUs) /
						1000.0
					: 0.0;
				cameraPublishToAcquireMs =
					cameraFrameMetadata.framePublishedTimeUs != 0 &&
					cameraFrameMetadata.senderAcquireTimeUs != 0 &&
					cameraFrameMetadata.senderAcquireTimeUs >=
						cameraFrameMetadata.framePublishedTimeUs
					? static_cast<double>(
						cameraFrameMetadata.senderAcquireTimeUs -
						cameraFrameMetadata.framePublishedTimeUs) /
						1000.0
					: 0.0;
				cameraAcquireToSendMs =
					cameraFrameMetadata.senderAcquireTimeUs != 0 &&
					nowUs >= cameraFrameMetadata.senderAcquireTimeUs
					? static_cast<double>(
						nowUs - cameraFrameMetadata.senderAcquireTimeUs) /
						1000.0
					: 0.0;
			};

			{
				std::lock_guard<std::mutex> lock(sendTelemetry.mutex);
				sendTelemetry.cameraFrameReady = freshCameraFrame;
				sendTelemetry.cameraFrameId = cameraFrameId;
				sendTelemetry.cameraSourceTimestamp100ns =
					cameraFrameMetadata.sourceTimestamp100ns;
				sendTelemetry.cameraReadSampleStartTimeUs =
					cameraFrameMetadata.readSampleStartTimeUs;
				sendTelemetry.cameraReadSampleEndTimeUs =
					cameraFrameMetadata.readSampleEndTimeUs;
				sendTelemetry.cameraCaptureCompletedTimeUs =
					cameraFrameMetadata.captureCompletedTimeUs;
				sendTelemetry.cameraFramePublishedTimeUs =
					cameraFrameMetadata.framePublishedTimeUs;
				sendTelemetry.cameraSenderAcquireTimeUs =
					cameraFrameMetadata.senderAcquireTimeUs;
				sendTelemetry.cameraReadSampleMs =
					cameraFrameMetadata.readSampleMs;
				sendTelemetry.cameraReadSampleEndToCaptureMs =
					cameraReadSampleEndToCaptureMs;
				sendTelemetry.cameraCaptureToPublishMs =
					cameraCaptureToPublishMs;
				sendTelemetry.cameraPublishToAcquireMs =
					cameraPublishToAcquireMs;
				sendTelemetry.cameraAcquireToSendMs =
					cameraAcquireToSendMs;
				sendTelemetry.cameraFrameAgeMs = cameraFrameAgeMs;
				sendTelemetry.cameraFrameCacheUsed = usedCameraCache;
				sendTelemetry.captureFps =
					cameraCaptureEnabled && cameraCapture
					? cameraCapture->GetAsyncCaptureFps()
					: 0.0;
				sendTelemetry.cameraCaptureSubtype =
					cameraCaptureEnabled && cameraCapture
					? cameraCapture->GetCaptureSubtypeName()
					: std::string{};
				sendTelemetry.cameraCaptureWidth =
					cameraCaptureEnabled && cameraCapture
					? cameraCapture->GetCaptureWidth()
					: 0u;
				sendTelemetry.cameraCaptureHeight =
					cameraCaptureEnabled && cameraCapture
					? cameraCapture->GetCaptureHeight()
					: 0u;
				sendTelemetry.cameraCaptureFormatFps =
					cameraCaptureEnabled && cameraCapture
					? cameraCapture->GetCaptureFormatFps()
					: 0.0;
			}

			if (!hasCameraFrame && !usedCameraCache && !wantsH264) {
				selectedCameraNv12Frame = false;
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

			const auto encodePipelineStartTime =
				std::chrono::steady_clock::now();
			double resizeMs = 0.0;
			double nv12PrepareMs = 0.0;
			double h264EncodeMs = 0.0;
			uint32_t h264EncoderDelayFrames = 0;
			double h264EncoderDelayMs = 0.0;
			uint32_t h264EncoderPendingFrames = 0;
			uint32_t h264EncodedInputFrameId = 0;
			H264Encoder::FrameTiming h264EncoderTiming{};
			uint64_t encodedCameraFrameId = 0;
			int64_t encodedCameraSourceTimestamp100ns = 0;
			uint64_t encodedCameraCaptureCompletedTimeUs = 0;
			double encodedCameraFrameAgeMs = 0.0;
			double encodedCameraReadSampleMs = 0.0;
			double encodedCameraReadSampleEndToCaptureMs = 0.0;
			double encodedCameraCaptureToPublishMs = 0.0;
			double encodedCameraPublishToAcquireMs = 0.0;
			double encodedCameraAcquireToEncoderInputMs = 0.0;
			double jpegEncodeMs = 0.0;
			double packetizeMs = 0.0;

			std::vector<uint8_t> resizedVideoFrame;
			const std::vector<uint8_t>* adaptiveVideoFrame = &videoFrame;
			auto ensureRgbaVideoFrame = [&]() -> bool {
				if (!videoFrame.empty()) {
					return true;
				}
				if (cameraCaptureEnabled &&
					cameraCapture &&
					cameraCapture->TryGetLatestRgbaFrame(
						videoFrame,
						cameraFrameMetadata)) {
					return true;
				}
				videoFrame.assign(
					static_cast<size_t>(texWidth) *
						static_cast<size_t>(texHeight) *
						4u,
					0u);
				for (size_t i = 3; i < videoFrame.size(); i += 4u) {
					videoFrame[i] = 255;
				}
				return false;
			};
			auto ensureAdaptiveRgbaFrame = [&]() -> const std::vector<uint8_t>* {
				ensureRgbaVideoFrame();
				if (targetWidth == texWidth && targetHeight == texHeight) {
					return &videoFrame;
				}
				if (!resizedVideoFrame.empty()) {
					return &resizedVideoFrame;
				}
				const auto resizeStartTime =
					std::chrono::steady_clock::now();
				resizedVideoFrame =
					ResizeRgbaBilinear(
						videoFrame,
						texWidth,
						texHeight,
						targetWidth,
						targetHeight
					);
				resizeMs =
					std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() -
						resizeStartTime
					).count();
				return &resizedVideoFrame;
			};

			std::vector<uint8_t> encodedPayload;
			net::CodecType sendCodec = runtimeState.networkVideoCodec;
			uint32_t rnvpSendFrameId = frameId;
			bool h264AsyncSubmittedWithoutOutput = false;
			bool h264SubmittedNewInput = false;
			bool h264SkippedInputWaitingForFreshCamera = false;
			bool h264InputGatedByPacing = false;
			std::string h264InputGateReason;
			double h264InputGateQueueDelayMs = 0.0;
			double h264InputGateVideoCreditBytes = 0.0;
			double h264InputGateFrameBudgetBytes = 0.0;
			double h264InputGateDurationMs = 0.0;
			uint32_t h264InputGateConsecutiveFrames = 0;
			uint64_t h264InputGateSkippedInputFrames = 0;
			bool h264InputGateForcedOpen = false;
			std::string h264InputGateReleaseReason;
			const bool requestedKeyFrame =
				networkManager->ConsumeKeyFrameRequest();

			if (sendCodec == net::CodecType::H264) {
				const uint32_t h264Width = targetWidth & ~1u;
				const uint32_t h264Height = targetHeight & ~1u;
				const uint32_t h264Fps =
					static_cast<uint32_t>(std::clamp(targetFps, 1, 30));
				const uint32_t h264Bitrate =
					h264EncoderTargetBitrateKbps * 1000u;
				const uint32_t periodicIdrFrames =
					ResolveH264PeriodicIdrFrames(h264Fps);

				if (!h264EncoderReady ||
					h264EncoderWidth != h264Width ||
					h264EncoderHeight != h264Height ||
					h264EncoderFps != h264Fps) {
					resetH264Encoder();
					h264EncoderReady =
						h264Encoder.Initialize(
							h264Width,
							h264Height,
							h264Bitrate,
							h264Fps);
					h264EncoderReinitializations++;
					if (h264EncoderReady) {
						h264EncoderWidth = h264Width;
						h264EncoderHeight = h264Height;
						h264EncoderBitrate = h264Bitrate;
						h264EncoderFps = h264Fps;
					}
					else {
						OutputDebugStringA(
							"[AppMain] H.264 encoder init failed; falling back to MJPEG.\n");
					}
				}
				else if (h264EncoderBitrate != h264Bitrate) {
					h264DynamicBitrateUpdateRequests++;
					if (h264Encoder.SetTargetBitrate(h264Bitrate)) {
						h264EncoderBitrate = h264Bitrate;
						h264DynamicBitrateUpdateSuccesses++;
					}
					else {
						h264DynamicBitrateUpdateFailures++;
						OutputDebugStringA(
							"[AppMain] H.264 dynamic bitrate update failed; keeping current encoder instance.\n");
					}
				}

				if (h264EncoderReady) {
					const bool h264AwaitingSyncRequest =
						h264AwaitingDecoderSync;
					const bool h264PeriodicIdrRequest =
						periodicIdrFrames > 0 &&
						(frameId % periodicIdrFrames) == 1;
					if (requestedKeyFrame) {
						h264RequestedKeyFrameConsumedFrames++;
						h264RecoveryKeyFrameRequests++;
						h264AwaitingDecoderSync = true;
					}
					if (requestedKeyFrame ||
						h264AwaitingSyncRequest ||
						h264PeriodicIdrRequest) {
						if (h264AwaitingSyncRequest && !requestedKeyFrame) {
							h264AwaitingSyncKeyFrameRequests++;
						}
						if (h264PeriodicIdrRequest &&
							!requestedKeyFrame &&
							!h264AwaitingSyncRequest) {
							h264PeriodicIdrRequests++;
						}
						h264EncoderKeyFrameRequests++;
						h264Encoder.RequestKeyFrame();
					}

					std::vector<BYTE> accessUnit;
					bool h264Encoded = false;
					H264Encoder::FrameTiming h264DrainTiming{};
					if (h264Encoder.IsAsyncHardware()) {
						const auto h264DrainStartTime =
							std::chrono::steady_clock::now();
						const bool h264Drained =
							h264Encoder.DrainOutput(accessUnit, 0);
						h264EncodeMs +=
							std::chrono::duration<double, std::milli>(
								std::chrono::steady_clock::now() -
								h264DrainStartTime
							).count();
						h264DrainTiming =
							h264Encoder.GetLastFrameTiming();
						if (h264Drained) {
							h264EncoderTiming = h264DrainTiming;
							h264Encoded = true;
						}
					}

					if (h264Encoder.IsAsyncHardware() &&
						!dueForScheduledInput &&
						!h264NeedInputSubmitWake &&
						h264EncoderTiming.needInputSignaled &&
						cameraCaptureEnabled &&
						cameraCapture != nullptr) {
						const auto needInputNow =
							std::chrono::steady_clock::now();
						h264NeedInputSubmitLeadMs =
							nextSendTime > needInputNow
							? std::chrono::duration<double, std::milli>(
								nextSendTime - needInputNow).count()
							: 0.0;
						const double maxNeedInputLeadMs =
							ReadEnvDoubleClamped(
								"RNVP_H264_NEED_INPUT_EARLY_SUBMIT_MS",
								12.0,
								0.0,
								33.0);
						if (h264NeedInputSubmitLeadMs <=
							maxNeedInputLeadMs) {
							CameraCapture::Nv12Frame lateNv12Frame;
							CameraCapture::FrameMetadata lateMetadata{};
							std::vector<uint8_t> lateRgbaFrame;
							bool lateHasNv12 =
								cameraCapture->TryGetLatestNv12Frame(
									lateNv12Frame);
							if (lateHasNv12) {
								lateMetadata = lateNv12Frame.metadata;
							}
							bool lateHasRgba = false;
							if (!lateHasNv12) {
								lateHasRgba =
									cameraCapture->TryGetLatestRgbaFrame(
										lateRgbaFrame,
										lateMetadata);
							}
							const uint64_t lateFrameId =
								lateMetadata.frameId;
							if ((lateHasNv12 || lateHasRgba) &&
								lateFrameId != 0 &&
								lateFrameId != lastCameraFrameId) {
								cameraFrameMetadata = lateMetadata;
								cameraFrameId = lateFrameId;
								hasCameraNv12Frame = lateHasNv12;
								hasCameraRgbaFrame = lateHasRgba;
								hasCameraFrame = true;
								freshCameraFrame = true;
								usedCameraCache = false;
								reusedCameraFrame = false;
								selectedCameraNv12Frame = lateHasNv12;
								if (lateHasNv12) {
									cameraNv12Frame =
										std::move(lateNv12Frame);
									lastCameraNv12FrameCache =
										cameraNv12Frame;
									lastCameraFrameCache.clear();
								}
								else {
									videoFrame = std::move(lateRgbaFrame);
									lastCameraFrameCache = videoFrame;
									lastCameraNv12FrameCache = {};
								}
								lastCameraFrameId = cameraFrameId;
								lastCameraFrameMetadata =
									cameraFrameMetadata;
								refreshCameraTimingMetrics();
								h264NeedInputSubmitWake = true;
								{
									std::lock_guard<std::mutex> lock(
										sendTelemetry.mutex);
									sendTelemetry.cameraFrameReady = true;
									sendTelemetry.cameraFrameId =
										cameraFrameId;
									sendTelemetry.cameraSourceTimestamp100ns =
										cameraFrameMetadata.sourceTimestamp100ns;
									sendTelemetry.cameraReadSampleStartTimeUs =
										cameraFrameMetadata.readSampleStartTimeUs;
									sendTelemetry.cameraReadSampleEndTimeUs =
										cameraFrameMetadata.readSampleEndTimeUs;
									sendTelemetry.cameraCaptureCompletedTimeUs =
										cameraFrameMetadata.captureCompletedTimeUs;
									sendTelemetry.cameraFramePublishedTimeUs =
										cameraFrameMetadata.framePublishedTimeUs;
									sendTelemetry.cameraSenderAcquireTimeUs =
										cameraFrameMetadata.senderAcquireTimeUs;
									sendTelemetry.cameraReadSampleMs =
										cameraFrameMetadata.readSampleMs;
									sendTelemetry.
										cameraReadSampleEndToCaptureMs =
										cameraReadSampleEndToCaptureMs;
									sendTelemetry.cameraCaptureToPublishMs =
										cameraCaptureToPublishMs;
									sendTelemetry.cameraPublishToAcquireMs =
										cameraPublishToAcquireMs;
									sendTelemetry.cameraAcquireToSendMs =
										cameraAcquireToSendMs;
									sendTelemetry.cameraFrameAgeMs =
										cameraFrameAgeMs;
									sendTelemetry.cameraFrameCacheUsed = false;
								}
							}
						}
					}

					const bool h264FreshCameraInputAvailable =
						!cameraCaptureEnabled ||
						(hasCameraFrame &&
							freshCameraFrame &&
							!reusedCameraFrame);
					if (h264Encoder.IsAsyncHardware() &&
						!dueForScheduledInput &&
						!h264NeedInputSubmitWake &&
						h264EncoderTiming.needInputSignaled &&
						h264FreshCameraInputAvailable) {
						const auto needInputNow =
							std::chrono::steady_clock::now();
						h264NeedInputSubmitLeadMs =
							nextSendTime > needInputNow
							? std::chrono::duration<double, std::milli>(
								nextSendTime - needInputNow).count()
							: 0.0;
						const double maxNeedInputLeadMs =
							ReadEnvDoubleClamped(
								"RNVP_H264_NEED_INPUT_EARLY_SUBMIT_MS",
								12.0,
								0.0,
								33.0);
						h264NeedInputSubmitWake =
							h264NeedInputSubmitLeadMs <=
							maxNeedInputLeadMs;
					}
					if (h264Encoder.IsAsyncHardware() &&
						dueForScheduledInput &&
						cameraCaptureEnabled &&
						!h264FreshCameraInputAvailable) {
						h264SkippedInputWaitingForFreshCamera = true;
					}
					if (h264Encoder.IsAsyncHardware() &&
						dueForScheduledInput &&
						h264FreshCameraInputAvailable &&
						networkManager &&
						!requestedKeyFrame &&
						!h264AwaitingDecoderSync) {
						const auto gateNow =
							std::chrono::steady_clock::now();
						const net::PacketPacerStats pacingStatsNow =
							networkManager->GetPacingStats();
						h264InputGateQueueDelayMs =
							pacingStatsNow.currentQueueDelayMs;
						h264InputGateVideoCreditBytes =
							pacingStatsNow.videoCreditBytes;
						h264InputGateFrameBudgetBytes =
							static_cast<double>(pacingTargetBitrateKbps) *
							1000.0 /
							8.0 /
							static_cast<double>(
								(std::max)(1, inputCadenceFps));
						const double startGateMs =
							ReadEnvDoubleClamped(
								"RNVP_H264_INPUT_GATE_QUEUE_DELAY_MS",
								55.0,
								5.0,
								250.0);
						const double resumeGateMs =
							ReadEnvDoubleClamped(
								"RNVP_H264_INPUT_GATE_RESUME_DELAY_MS",
								20.0,
								0.0,
								200.0);
						const double maxGateHoldMs =
							ReadEnvDoubleClamped(
								"RNVP_H264_INPUT_GATE_MAX_HOLD_MS",
								220.0,
								20.0,
								500.0);
						const double lowCreditRatio =
							ReadEnvDoubleClamped(
								"RNVP_H264_INPUT_GATE_LOW_CREDIT_RATIO",
								0.75,
								0.0,
								1.0);
						const bool queueStressed =
							h264InputGateQueueDelayMs >= startGateMs ||
							(h264PacingInputGateActive &&
								h264InputGateQueueDelayMs > resumeGateMs);
						const bool videoCreditLow =
							h264InputGateFrameBudgetBytes <= 0.0 ||
							h264InputGateVideoCreditBytes <
								h264InputGateFrameBudgetBytes *
								lowCreditRatio;
						if (h264PacingInputGateActive) {
							h264InputGateDurationMs =
								std::chrono::duration<double, std::milli>(
									gateNow -
									h264PacingInputGateStartTime
								).count();
						}
						const bool maxHoldReached =
							h264PacingInputGateActive &&
							h264InputGateDurationMs >= maxGateHoldMs;
						h264InputGateForcedOpen =
							maxHoldReached;
						h264InputGatedByPacing =
							queueStressed &&
							videoCreditLow &&
							!maxHoldReached;
						if (h264InputGatedByPacing &&
							!h264PacingInputGateActive) {
							h264PacingInputGateActive = true;
							h264PacingInputGateStartTime = gateNow;
							h264PacingInputGateConsecutiveFrames = 0;
							h264InputGateDurationMs = 0.0;
						}
						else if (!h264InputGatedByPacing &&
							h264PacingInputGateActive) {
							const uint32_t previousConsecutiveGateFrames =
								h264PacingInputGateConsecutiveFrames;
							if (maxHoldReached) {
								h264InputGateReleaseReason =
									"max-hold-forced-open";
							}
							else if (!queueStressed) {
								h264InputGateReleaseReason =
									"queue-resumed";
							}
							else if (!videoCreditLow) {
								h264InputGateReleaseReason =
									"credit-resumed";
							}
							h264PacingInputGateActive = false;
							h264PacingInputGateConsecutiveFrames = 0;
							h264InputGateConsecutiveFrames =
								previousConsecutiveGateFrames;
						}
						if (h264InputGatedByPacing) {
							h264InputGateReason =
								"pacing-input-gate";
							h264PacingInputGateConsecutiveFrames++;
							h264PacingInputGateSkippedInputFrames++;
							h264InputGateConsecutiveFrames =
								h264PacingInputGateConsecutiveFrames;
							h264InputGateSkippedInputFrames =
								h264PacingInputGateSkippedInputFrames;
							h264InputGateDurationMs =
								std::chrono::duration<double, std::milli>(
									gateNow -
									h264PacingInputGateStartTime
								).count();
						}
						else {
							if (h264InputGateReleaseReason.empty()) {
								h264InputGateConsecutiveFrames =
									h264PacingInputGateConsecutiveFrames;
							}
							h264InputGateSkippedInputFrames =
								h264PacingInputGateSkippedInputFrames;
						}
					}
					else if (h264PacingInputGateActive &&
						networkManager) {
						const auto gateNow =
							std::chrono::steady_clock::now();
						const net::PacketPacerStats pacingStatsNow =
							networkManager->GetPacingStats();
						const double resumeGateMs =
							ReadEnvDoubleClamped(
								"RNVP_H264_INPUT_GATE_RESUME_DELAY_MS",
								32.0,
								0.0,
								200.0);
						h264InputGateQueueDelayMs =
							pacingStatsNow.currentQueueDelayMs;
						h264InputGateVideoCreditBytes =
							pacingStatsNow.videoCreditBytes;
						h264InputGateDurationMs =
							std::chrono::duration<double, std::milli>(
								gateNow -
								h264PacingInputGateStartTime
							).count();
						h264InputGateConsecutiveFrames =
							h264PacingInputGateConsecutiveFrames;
						h264InputGateSkippedInputFrames =
							h264PacingInputGateSkippedInputFrames;
						if (pacingStatsNow.currentQueueDelayMs <=
							resumeGateMs) {
							h264PacingInputGateActive = false;
							h264PacingInputGateConsecutiveFrames = 0;
							h264InputGateReleaseReason =
								"queue-resumed";
						}
					}
					const bool h264ShouldPrepareInput =
						!h264Encoder.IsAsyncHardware() ||
						((dueForScheduledInput ||
							h264NeedInputSubmitWake) &&
							h264FreshCameraInputAvailable &&
							!h264InputGatedByPacing);

					const auto nv12StartTime =
						std::chrono::steady_clock::now();
					if (h264ShouldPrepareInput) {
						if (selectedCameraNv12Frame &&
							!cameraNv12Frame.yPlane.empty() &&
							!cameraNv12Frame.uvPlane.empty()) {
							ConvertScaledNv12ToNv12Fast(
								cameraNv12Frame.yPlane.data(),
								cameraNv12Frame.uvPlane.data(),
								cameraNv12Frame.width,
								cameraNv12Frame.height,
								cameraNv12Frame.yPitch,
								cameraNv12Frame.uvPitch,
								h264Width,
								h264Height,
								h264Nv12Frame);
						}
						else if (h264ScaleFilter == H264ScaleFilter::Bilinear) {
							ensureRgbaVideoFrame();
							h264Nv12Frame =
								ConvertScaledRgbaToNv12Bilinear(
									videoFrame,
									texWidth,
									texHeight,
									h264Width,
									h264Height);
						}
						else {
							ensureRgbaVideoFrame();
							ConvertScaledRgbaToNv12Fast(
								videoFrame,
								texWidth,
								texHeight,
								h264Width,
								h264Height,
								h264Nv12Frame);
						}
					}
					nv12PrepareMs =
						std::chrono::duration<double, std::milli>(
							std::chrono::steady_clock::now() -
							nv12StartTime
					).count();
					if (h264ShouldPrepareInput && !h264Nv12Frame.empty()) {
						const size_t pendingAfterDrainedOutput =
							!accessUnit.empty() &&
								!h264PendingInputFrames.empty()
							? h264PendingInputFrames.size() - 1u
							: h264PendingInputFrames.size();
						const bool asyncSubmitAllowed =
							h264Encoder.IsAsyncHardware() &&
							(dueForScheduledInput ||
								h264NeedInputSubmitWake) &&
							h264FreshCameraInputAvailable &&
							!h264InputGatedByPacing &&
							h264Encoder.CanAcceptInput() &&
							pendingAfterDrainedOutput <
								(h264EncoderTiming.needInputSignaled
									? ReadEnvUInt32Clamped(
										"RNVP_H264_NEED_INPUT_MAX_PENDING",
										2u,
										1u,
										3u)
									: 1u);
						const bool syncSubmitAllowed =
							!h264Encoder.IsAsyncHardware();
						if (asyncSubmitAllowed || syncSubmitAllowed) {
							H264PendingInputFrame pendingInput{};
							pendingInput.sequence = h264InputSequence + 1;
							pendingInput.inputFrameId = frameId;
							pendingInput.cameraFrameId = cameraFrameId;
							pendingInput.cameraSourceTimestamp100ns =
								cameraFrameMetadata.sourceTimestamp100ns;
							pendingInput.cameraCaptureCompletedTimeUs =
								cameraFrameMetadata.captureCompletedTimeUs;
							pendingInput.encoderInputTimeUs = steadyNowUs();
							pendingInput.cameraReadSampleMs =
								cameraFrameMetadata.readSampleMs;
							pendingInput.cameraReadSampleEndToCaptureMs =
								cameraReadSampleEndToCaptureMs;
							pendingInput.cameraCaptureToPublishMs =
								cameraCaptureToPublishMs;
							pendingInput.cameraPublishToAcquireMs =
								cameraPublishToAcquireMs;
							if (cameraFrameMetadata.senderAcquireTimeUs != 0 &&
								pendingInput.encoderInputTimeUs >=
									cameraFrameMetadata.senderAcquireTimeUs) {
								pendingInput.cameraAcquireToEncoderInputMs =
									static_cast<double>(
										pendingInput.encoderInputTimeUs -
										cameraFrameMetadata.senderAcquireTimeUs) /
									1000.0;
							}
							const auto h264EncodeStartTime =
								std::chrono::steady_clock::now();
							std::vector<BYTE> immediateAccessUnit;
							bool h264InputAccepted = false;
							if (h264Encoder.IsAsyncHardware()) {
								h264InputAccepted =
									h264Encoder.SubmitFrameNoWait(
										h264Nv12Frame.data(),
										static_cast<UINT>(h264Nv12Frame.size()));
							}
							else {
								h264InputAccepted =
									h264Encoder.EncodeFrame(
										h264Nv12Frame.data(),
										static_cast<UINT>(h264Nv12Frame.size()),
										immediateAccessUnit);
							}
							h264EncodeMs +=
								std::chrono::duration<double, std::milli>(
									std::chrono::steady_clock::now() -
									h264EncodeStartTime
								).count();
							const H264Encoder::FrameTiming submitTiming =
								h264Encoder.GetLastFrameTiming();
							if (accessUnit.empty()) {
								h264EncoderTiming = submitTiming;
							}
							if (!immediateAccessUnit.empty() &&
								accessUnit.empty()) {
								accessUnit = std::move(immediateAccessUnit);
							}
							if (h264InputAccepted) {
								h264InputSequence = pendingInput.sequence;
								h264PendingInputFrames.push(pendingInput);
								h264SubmittedNewInput = true;
								h264Encoded = true;
							}
						}
					}
					if (h264Encoded && !accessUnit.empty()) {
						H264PendingInputFrame encodedInput{};
						if (!h264PendingInputFrames.empty()) {
							encodedInput = h264PendingInputFrames.front();
							h264PendingInputFrames.pop();
						}
						const uint64_t encodedOutputTimeUs = steadyNowUs();
						h264EncodedInputFrameId = encodedInput.inputFrameId;
						if (encodedInput.inputFrameId != 0) {
							rnvpSendFrameId = encodedInput.inputFrameId;
						}
						encodedCameraFrameId = encodedInput.cameraFrameId;
						encodedCameraSourceTimestamp100ns =
							encodedInput.cameraSourceTimestamp100ns;
						encodedCameraCaptureCompletedTimeUs =
							encodedInput.cameraCaptureCompletedTimeUs;
						encodedCameraReadSampleMs =
							encodedInput.cameraReadSampleMs;
						encodedCameraReadSampleEndToCaptureMs =
							encodedInput.cameraReadSampleEndToCaptureMs;
						encodedCameraCaptureToPublishMs =
							encodedInput.cameraCaptureToPublishMs;
						encodedCameraPublishToAcquireMs =
							encodedInput.cameraPublishToAcquireMs;
						encodedCameraAcquireToEncoderInputMs =
							encodedInput.cameraAcquireToEncoderInputMs;
						if (encodedInput.sequence != 0 &&
							h264InputSequence >= encodedInput.sequence) {
							h264EncoderDelayFrames =
								static_cast<uint32_t>(
									h264InputSequence - encodedInput.sequence);
						}
						if (encodedInput.encoderInputTimeUs != 0 &&
							encodedOutputTimeUs >= encodedInput.encoderInputTimeUs) {
							h264EncoderDelayMs =
								static_cast<double>(
									encodedOutputTimeUs -
									encodedInput.encoderInputTimeUs) /
								1000.0;
						}
						if (encodedInput.cameraCaptureCompletedTimeUs != 0 &&
							encodedOutputTimeUs >=
							encodedInput.cameraCaptureCompletedTimeUs) {
							encodedCameraFrameAgeMs =
								static_cast<double>(
									encodedOutputTimeUs -
									encodedInput.cameraCaptureCompletedTimeUs) /
								1000.0;
						}
						h264EncoderPendingFrames =
							static_cast<uint32_t>(h264PendingInputFrames.size());
						const uint64_t ptsUs =
							static_cast<uint64_t>(
								(rnvpSendFrameId - 1u) *
								(1000000ull / (std::max<uint32_t>)(1u, h264Fps)));
						const auto packetizeStartTime =
							std::chrono::steady_clock::now();
						encodedPayload =
								PackH264AccessUnitPayload(
								accessUnit,
								rnvpSendFrameId,
								h264Width,
								h264Height,
								ptsUs,
								encodedInput.cameraCaptureCompletedTimeUs,
								encodedOutputTimeUs);
						packetizeMs =
							std::chrono::duration<double, std::milli>(
								std::chrono::steady_clock::now() -
								packetizeStartTime
							).count();
						const bool isDecoderSync =
							IsH264AccessUnitPayloadDecoderSync(encodedPayload);
						if (isDecoderSync) {
							h264DecoderSyncSent = true;
							h264AwaitingDecoderSync = false;
						}
						else if (!h264DecoderSyncSent ||
							h264AwaitingDecoderSync) {
							encodedPayload.clear();
						}
					}
					if (h264Encoder.IsAsyncHardware() &&
						(h264Encoded || !h264PendingInputFrames.empty()) &&
						encodedPayload.empty()) {
						h264AsyncSubmittedWithoutOutput = true;
					}
				}
			}

			if (sendCodec == net::CodecType::H264 &&
				encodedPayload.empty() &&
				(h264AsyncSubmittedWithoutOutput ||
					h264SkippedInputWaitingForFreshCamera ||
					h264InputGatedByPacing)) {
				h264EncoderPendingFrames =
					static_cast<uint32_t>(h264PendingInputFrames.size());
				const uint64_t actualRawFrameBytes =
					static_cast<uint64_t>(targetWidth) *
					static_cast<uint64_t>(targetHeight) *
					4u;
				const double encodeMs =
					std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() -
						encodePipelineStartTime
					).count();
				const bool h264AsyncPendingNoOutput =
					h264AsyncSubmittedWithoutOutput &&
					!h264SubmittedNewInput &&
					!h264SkippedInputWaitingForFreshCamera &&
					!h264InputGatedByPacing &&
					!h264PendingInputFrames.empty();
				double h264AsyncOutputPollBackoffMs = 0.0;
				if (h264AsyncPendingNoOutput) {
					h264AsyncPendingNoOutputStreak =
						(std::min<uint32_t>)(
							h264AsyncPendingNoOutputStreak + 1u,
							8u);
					const double holdMs =
						h264AsyncPendingNoOutputStreak >= 2u
						? ReadEnvDoubleClamped(
							"RNVP_H264_ASYNC_HARD_CADENCE_HOLD_MS",
							420.0,
							50.0,
							2000.0)
						: ReadEnvDoubleClamped(
							"RNVP_H264_ASYNC_CADENCE_HOLD_MS",
							180.0,
							20.0,
							1000.0);
					h264AsyncCadenceHoldUntil =
						(std::max)(
							h264AsyncCadenceHoldUntil,
							std::chrono::steady_clock::now() +
								std::chrono::duration_cast<
									std::chrono::steady_clock::duration>(
									std::chrono::duration<double, std::milli>(
										holdMs)));
					h264AsyncCadenceHoldActive = true;
					h264AsyncCadenceHoldRemainingMs =
						(std::max)(h264AsyncCadenceHoldRemainingMs, holdMs);
					h264AsyncOutputPollBackoffMs =
						ReadEnvDoubleClamped(
							"RNVP_H264_ASYNC_OUTPUT_POLL_BACKOFF_MS",
							1.5,
							0.0,
							20.0);
					if (h264AsyncOutputPollBackoffMs > 0.0) {
						const auto backoffTime =
							std::chrono::steady_clock::now() +
							std::chrono::duration_cast<
								std::chrono::steady_clock::duration>(
								std::chrono::duration<double, std::milli>(
									h264AsyncOutputPollBackoffMs));
						if (nextSendTime < backoffTime) {
							nextSendTime = backoffTime;
						}
					}
				}
				else if (!encodedPayload.empty() ||
					h264EncoderTiming.outputProduced) {
					h264AsyncPendingNoOutputStreak = 0;
				}
				{
					std::lock_guard<std::mutex> lock(sendTelemetry.mutex);
					sendTelemetry.actualCodec = net::ToString(sendCodec);
					sendTelemetry.actualEncodeWidth = targetWidth;
					sendTelemetry.actualEncodeHeight = targetHeight;
					sendTelemetry.actualRawFrameBytes = actualRawFrameBytes;
					sendTelemetry.actualEncodedFrameBytes = 0;
					sendTelemetry.encodeMs = encodeMs;
					sendTelemetry.resizeMs = resizeMs;
					sendTelemetry.nv12PrepareMs = nv12PrepareMs;
					sendTelemetry.h264EncodeMs = h264EncodeMs;
					sendTelemetry.h264EncoderRequestedBitrateKbps =
						h264EncoderRequestedBitrateKbps;
					sendTelemetry.h264EncoderTargetBitrateKbps =
						h264EncoderTargetBitrateKbps;
					sendTelemetry.h264EncoderAppliedBitrateKbps =
						h264EncoderBitrate / 1000u;
					sendTelemetry.h264DynamicBitrateUpdateRequests =
						h264DynamicBitrateUpdateRequests;
					sendTelemetry.h264DynamicBitrateUpdateSuccesses =
						h264DynamicBitrateUpdateSuccesses;
					sendTelemetry.h264DynamicBitrateUpdateFailures =
						h264DynamicBitrateUpdateFailures;
					sendTelemetry.h264EncoderReinitializations =
						h264EncoderReinitializations;
					sendTelemetry.pacingTargetBitrateKbps =
						pacingTargetBitrateKbps;
					sendTelemetry.h264AuChunkCount = 0;
					sendTelemetry.h264AuIsIdr = false;
					sendTelemetry.h264AuIsDecoderSync = false;
					sendTelemetry.h264RequestedKeyFrameConsumedFrames =
						h264RequestedKeyFrameConsumedFrames;
					sendTelemetry.h264EncoderKeyFrameRequests =
						h264EncoderKeyFrameRequests;
					sendTelemetry.h264RecoveryKeyFrameRequests =
						h264RecoveryKeyFrameRequests;
					sendTelemetry.h264AwaitingSyncKeyFrameRequests =
						h264AwaitingSyncKeyFrameRequests;
					sendTelemetry.h264PeriodicIdrRequests =
						h264PeriodicIdrRequests;
					sendTelemetry.h264IdrFrames =
						h264IdrFrames;
					sendTelemetry.h264DecoderSyncFrames =
						h264DecoderSyncFrames;
					sendTelemetry.h264ConsecutiveIdrFrames =
						h264ConsecutiveIdrFrames;
					sendTelemetry.h264MaxConsecutiveIdrFrames =
						h264MaxConsecutiveIdrFrames;
					sendTelemetry.h264AuProtectionLevel =
						h264SkippedInputWaitingForFreshCamera
						? "fresh-camera-wait"
						: (h264InputGatedByPacing
							? "pacing-input-gate"
							: "async-output-pending");
					sendTelemetry.h264AuDroppedBeforeSend = false;
					sendTelemetry.h264AuDropReason.clear();
					sendTelemetry.h264AuDroppedBytes = 0;
					sendTelemetry.h264AuDroppedChunks = 0;
					sendTelemetry.h264AuPacingQueueDelayMs = 0.0;
					sendTelemetry.h264AuEstimatedSendMs = 0.0;
					sendTelemetry.h264InputGatedByPacing =
						h264InputGatedByPacing;
					sendTelemetry.h264InputGateReason =
						h264InputGateReason;
					sendTelemetry.h264InputGateQueueDelayMs =
						h264InputGateQueueDelayMs;
					sendTelemetry.h264InputGateVideoCreditBytes =
						h264InputGateVideoCreditBytes;
					sendTelemetry.h264InputGateFrameBudgetBytes =
						h264InputGateFrameBudgetBytes;
					sendTelemetry.h264InputGateDurationMs =
						h264InputGateDurationMs;
					sendTelemetry.h264InputGateConsecutiveFrames =
						h264InputGateConsecutiveFrames;
					sendTelemetry.h264InputGateSkippedInputFrames =
						h264InputGateSkippedInputFrames;
					sendTelemetry.h264InputGateForcedOpen =
						h264InputGateForcedOpen;
					sendTelemetry.h264InputGateReleaseReason =
						h264InputGateReleaseReason;
					sendTelemetry.fecProtectedH264KeyFrames =
						fecProtectedH264KeyFrames;
					sendTelemetry.fecProtectedH264LargeFrames =
						fecProtectedH264LargeFrames;
					sendTelemetry.h264EncoderDelayFrames =
						h264EncoderDelayFrames;
					sendTelemetry.h264EncoderDelayMs =
						h264EncoderDelayMs;
					sendTelemetry.h264EncoderPendingFrames =
						h264EncoderPendingFrames;
					sendTelemetry.h264EncodedInputFrameId =
						h264EncodedInputFrameId;
					sendTelemetry.h264EncoderCallMs =
						h264EncoderTiming.callMs;
					sendTelemetry.h264EncoderSampleCreateMs =
						h264EncoderTiming.sampleCreateMs;
					sendTelemetry.h264EncoderProcessInputMs =
						h264EncoderTiming.processInputMs;
					sendTelemetry.h264EncoderPreInputPollMs =
						h264EncoderTiming.preInputPollMs;
					sendTelemetry.h264EncoderPostInputWaitMs =
						h264EncoderTiming.postInputWaitMs;
					sendTelemetry.h264EncoderProcessOutputMs =
						h264EncoderTiming.processOutputMs;
					sendTelemetry.h264EncoderOutputCopyMs =
						h264EncoderTiming.outputCopyMs;
					sendTelemetry.h264EncoderProcessOutputAttempts =
						h264EncoderTiming.processOutputAttempts;
					sendTelemetry.h264EncoderAsyncEventCount =
						h264EncoderTiming.asyncEventCount;
					sendTelemetry.h264EncoderHardware =
						h264EncoderTiming.hardware;
					sendTelemetry.h264EncoderAsync =
						h264EncoderTiming.async;
					sendTelemetry.h264EncoderNeedInputSignaled =
						h264EncoderTiming.needInputSignaled;
					sendTelemetry.h264EncoderOutputProduced =
						h264EncoderTiming.outputProduced;
					sendTelemetry.h264EncoderOutputProducedBeforeInput =
						h264EncoderTiming.outputProducedBeforeInput;
					sendTelemetry.h264SubmittedNewInput =
						h264SubmittedNewInput;
					sendTelemetry.h264AsyncSubmittedWithoutOutput =
						h264AsyncSubmittedWithoutOutput;
					sendTelemetry.h264AsyncPendingNoOutput =
						h264AsyncPendingNoOutput;
					sendTelemetry.h264AsyncCadenceHoldActive =
						h264AsyncCadenceHoldActive;
					sendTelemetry.h264AsyncPendingNoOutputStreak =
						h264AsyncPendingNoOutputStreak;
					sendTelemetry.h264AsyncCadenceScale =
						h264AsyncCadenceScale;
					sendTelemetry.h264AsyncCadenceHoldRemainingMs =
						h264AsyncCadenceHoldRemainingMs;
					sendTelemetry.h264AsyncOutputPollBackoffMs =
						h264AsyncOutputPollBackoffMs;
					sendTelemetry.h264InputCadenceFps =
						static_cast<uint32_t>(inputCadenceFps);
					sendTelemetry.h264NeedInputSubmitWake =
						h264NeedInputSubmitWake;
					sendTelemetry.h264NeedInputSubmitLeadMs =
						h264NeedInputSubmitLeadMs;
					sendTelemetry.encodedCameraFrameId = 0;
					sendTelemetry.encodedCameraSourceTimestamp100ns = 0;
					sendTelemetry.encodedCameraCaptureCompletedTimeUs = 0;
					sendTelemetry.encodedCameraFrameAgeMs = 0.0;
					sendTelemetry.encodedCameraReadSampleMs = 0.0;
					sendTelemetry.encodedCameraReadSampleEndToCaptureMs = 0.0;
					sendTelemetry.encodedCameraCaptureToPublishMs = 0.0;
					sendTelemetry.encodedCameraPublishToAcquireMs = 0.0;
					sendTelemetry.encodedCameraAcquireToEncoderInputMs = 0.0;
					sendTelemetry.jpegEncodeMs = 0.0;
					sendTelemetry.packetizeMs = 0.0;
				}
				if (h264SubmittedNewInput) {
					frameId++;
					const auto scheduleNow = std::chrono::steady_clock::now();
					if (h264NeedInputSubmitWake &&
						nextSendTime > scheduleNow) {
						nextSendTime =
							scheduleNow +
							std::chrono::duration_cast<
								std::chrono::steady_clock::duration
							>(sendInterval);
					}
					else {
						nextSendTime +=
							std::chrono::duration_cast<
								std::chrono::steady_clock::duration
							>(sendInterval);
					}
					if (nextSendTime <
						scheduleNow - std::chrono::milliseconds(100)) {
						nextSendTime =
							scheduleNow +
							std::chrono::duration_cast<
								std::chrono::steady_clock::duration
							>(sendInterval);
					}
				}
				else if (h264InputGatedByPacing) {
					frameId++;
					nextSendTime +=
						std::chrono::duration_cast<
							std::chrono::steady_clock::duration
						>(sendInterval);
					const auto scheduleNow = std::chrono::steady_clock::now();
					if (nextSendTime <
						scheduleNow - std::chrono::milliseconds(100)) {
						nextSendTime =
							scheduleNow +
							std::chrono::duration_cast<
								std::chrono::steady_clock::duration
							>(sendInterval);
					}
				}
				else if (h264SkippedInputWaitingForFreshCamera) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				continue;
			}

			if (sendCodec != net::CodecType::H264 ||
				encodedPayload.empty()) {
				sendCodec = net::CodecType::MJPEG;
				adaptiveVideoFrame = ensureAdaptiveRgbaFrame();
				const auto jpegEncodeStartTime =
					std::chrono::steady_clock::now();
				encodedPayload =
					EncodeJpegFrame(
						*adaptiveVideoFrame,
						targetWidth,
						targetHeight,
						adaptiveState.targetJpegQuality
					);
				jpegEncodeMs =
					std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() -
						jpegEncodeStartTime
					).count();
			}

			if (encodedPayload.empty()) {
				ensureRgbaVideoFrame();
				const auto rawPacketizeStartTime =
					std::chrono::steady_clock::now();
				encodedPayload =
					PackRawRgbaPayload(videoFrame, texWidth, texHeight);
				packetizeMs +=
					std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() -
						rawPacketizeStartTime
					).count();
				sendCodec = net::CodecType::Raw;
			}

			const uint32_t actualEncodeWidth =
				sendCodec == net::CodecType::Raw ? texWidth : targetWidth;
			const uint32_t actualEncodeHeight =
				sendCodec == net::CodecType::Raw ? texHeight : targetHeight;
			const uint64_t actualRawFrameBytes =
				sendCodec == net::CodecType::Raw
					? static_cast<uint64_t>(videoFrame.size())
					: static_cast<uint64_t>(actualEncodeWidth) *
						static_cast<uint64_t>(actualEncodeHeight) *
						4u;
			const uint64_t actualEncodedFrameBytes =
				static_cast<uint64_t>(encodedPayload.size());
			const double encodeMs =
				std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() -
					encodePipelineStartTime
				).count();
			h264EncoderPendingFrames =
				static_cast<uint32_t>(h264PendingInputFrames.size());
			if (sendCodec == net::CodecType::H264 &&
				!encodedPayload.empty()) {
				h264AsyncPendingNoOutputStreak = 0;
			}

			net::H264AccessUnitPayloadHeader h264AuHeader{};
			const bool h264AuHeaderValid =
				sendCodec == net::CodecType::H264 &&
				net::DecodeH264AccessUnitPayloadHeader(
					encodedPayload.data(),
					encodedPayload.size(),
					h264AuHeader);
			const bool h264AuIsIdr =
				h264AuHeaderValid &&
				((h264AuHeader.flags & net::H264AccessUnitFlag_Idr) != 0);
			const bool h264AuIsDecoderSync =
				h264AuHeaderValid &&
				((h264AuHeader.flags &
					net::H264AccessUnitFlag_DecoderSync) != 0);
			if (sendCodec == net::CodecType::H264 &&
				!encodedPayload.empty()) {
				if (h264AuIsIdr) {
					h264IdrFrames++;
					h264ConsecutiveIdrFrames++;
					h264MaxConsecutiveIdrFrames =
						(std::max)(
							h264MaxConsecutiveIdrFrames,
							h264ConsecutiveIdrFrames);
				}
				else {
					h264ConsecutiveIdrFrames = 0;
				}
				if (h264AuIsDecoderSync) {
					h264DecoderSyncFrames++;
				}
			}
			const uint32_t h264AuChunkCount =
				sendCodec == net::CodecType::H264 && !encodedPayload.empty()
				? static_cast<uint32_t>(
					(encodedPayload.size() + net::kMaxUdpPayloadSize - 1u) /
					net::kMaxUdpPayloadSize)
				: 0u;
			const double h264FrameBudgetBytes =
				static_cast<double>(pacingTargetBitrateKbps) *
				1000.0 /
				8.0 /
				static_cast<double>((std::max)(1, targetFps));
			const double h264AuBudgetRatio =
				sendCodec == net::CodecType::H264 &&
					h264FrameBudgetBytes > 0.0
				? static_cast<double>(encodedPayload.size()) /
					h264FrameBudgetBytes
				: 0.0;
			const bool h264LargeAu =
				sendCodec == net::CodecType::H264 &&
				(h264AuChunkCount >= 8u ||
					(h264FrameBudgetBytes > 0.0 &&
						static_cast<double>(encodedPayload.size()) >
						h264FrameBudgetBytes * 1.35));
			bool h264AuDroppedBeforeSend = false;
			std::string h264AuDropReason;
			uint64_t h264AuDroppedBytes = 0;
			uint32_t h264AuDroppedChunks = 0;
			double h264AuPacingQueueDelayMs = 0.0;
			double h264AuEstimatedSendMs = 0.0;
			NetworkManager::RnvpFrameProtectionOptions protection{};
			std::string h264AuProtectionLevel =
				sendCodec == net::CodecType::H264 ? "none" : "";
			const bool h264AuProtectionEnabled =
				ReadEnvBool("RNVP_H264_AU_PROTECTION_ENABLED", false);
			const bool h264KeyProtectionEnabled =
				ReadEnvBool("RNVP_H264_KEY_PROTECTION_ENABLED", true);
			const double h264KeyProtectionIntervalMs =
				ReadEnvDoubleClamped(
					"RNVP_H264_KEY_PROTECTION_INTERVAL_MS",
					900.0,
					250.0,
					5000.0);
			if (sendCodec == net::CodecType::H264 &&
				(h264AuIsIdr || h264AuIsDecoderSync)) {
				const auto keyProtectionNow =
					std::chrono::steady_clock::now();
				const bool keyProtectionNeverSent =
					h264LastKeyProtectionTime.time_since_epoch().count() == 0;
				const double keyProtectionElapsedMs =
					keyProtectionNeverSent
					? h264KeyProtectionIntervalMs
					: std::chrono::duration<double, std::milli>(
						keyProtectionNow -
						h264LastKeyProtectionTime).count();
				const bool h264StrongKeyProtectionDue =
					h264KeyProtectionEnabled &&
					h264AuIsIdr &&
					(keyProtectionNeverSent ||
						keyProtectionElapsedMs >=
							h264KeyProtectionIntervalMs);
				if (h264AuProtectionEnabled ||
					h264StrongKeyProtectionDue) {
					protection.fecGroupChunkCountOverride = 4;
					protection.forceFec = true;
					protection.highPriorityData = true;
					protection.highPriorityFec = true;
					protection.extraPacingDeadlineUs = 30000;
					h264AuProtectionLevel =
						h264StrongKeyProtectionDue
						? "keyframe-g4-priority"
						: "decoder-sync-g4";
					fecProtectedH264KeyFrames++;
					if (h264StrongKeyProtectionDue) {
						h264LastKeyProtectionTime = keyProtectionNow;
					}
				}
				else {
					h264AuProtectionLevel = "decoder-sync-observed";
				}
			}
			else if (h264LargeAu) {
				if (h264AuProtectionEnabled) {
					protection.fecGroupChunkCountOverride = 8;
					protection.forceFec = true;
					h264AuProtectionLevel = "large-au-g8";
					fecProtectedH264LargeFrames++;
				}
				else {
					h264AuProtectionLevel = "large-au-observed";
				}
			}

			if (sendCodec == net::CodecType::H264 &&
				!encodedPayload.empty() &&
				networkManager) {
				const net::PacketPacerStats pacingStatsNow =
					networkManager->GetPacingStats();
				h264AuPacingQueueDelayMs =
					pacingStatsNow.currentQueueDelayMs;
				const double pacingBps =
					static_cast<double>(
						(std::max)(1u, pacingTargetBitrateKbps)) *
					1000.0;
				h264AuEstimatedSendMs =
					static_cast<double>(encodedPayload.size()) *
					8.0 *
					1000.0 /
					pacingBps;
				const double deadlineBudgetMs =
					ReadEnvDoubleClamped(
						"RNVP_H264_AU_SEND_DEADLINE_MS",
						115.0,
						40.0,
						250.0);
				const double largeAuQueueGuardMs =
					ReadEnvDoubleClamped(
						"RNVP_H264_LARGE_AU_QUEUE_GUARD_MS",
						55.0,
						0.0,
						200.0);
				const double staleAuAgeMs =
					ReadEnvDoubleClamped(
						"RNVP_H264_STALE_AU_AGE_MS",
						45.0,
						10.0,
						200.0);
				const bool queueWouldMissDeadline =
					h264AuPacingQueueDelayMs +
						h264AuEstimatedSendMs >
					deadlineBudgetMs;
				const bool queueAlreadyStressed =
					h264AuPacingQueueDelayMs >= largeAuQueueGuardMs;
				const bool staleAu =
					encodedCameraFrameAgeMs >= staleAuAgeMs ||
					h264EncoderDelayMs >=
						ReadEnvDoubleClamped(
							"RNVP_H264_STALE_ENCODER_DELAY_MS",
							18.0,
							5.0,
							120.0);
				const bool canDropDeltaAu =
					!h264AuIsIdr && !h264AuIsDecoderSync;
				if (canDropDeltaAu &&
					((h264LargeAu && queueWouldMissDeadline) ||
						(staleAu && queueAlreadyStressed))) {
					h264AuDroppedBeforeSend = true;
					h264AuDropReason =
						queueWouldMissDeadline
						? "large-au-would-miss-pacing-deadline"
						: "large-au-queue-stressed";
				}
				if (h264AuDroppedBeforeSend) {
					h264AuDroppedBytes =
						static_cast<uint64_t>(encodedPayload.size());
					h264AuDroppedChunks = h264AuChunkCount;
					h264AuProtectionLevel = h264AuDropReason;
				}
			}

			{
				std::lock_guard<std::mutex> lock(sendTelemetry.mutex);
				sendTelemetry.actualCodec = net::ToString(sendCodec);
				sendTelemetry.actualEncodeWidth = actualEncodeWidth;
				sendTelemetry.actualEncodeHeight = actualEncodeHeight;
				sendTelemetry.actualRawFrameBytes = actualRawFrameBytes;
				sendTelemetry.actualEncodedFrameBytes = actualEncodedFrameBytes;
				sendTelemetry.encodeMs = encodeMs;
				sendTelemetry.resizeMs = resizeMs;
				sendTelemetry.nv12PrepareMs = nv12PrepareMs;
				sendTelemetry.h264EncodeMs = h264EncodeMs;
				sendTelemetry.h264EncoderRequestedBitrateKbps =
					sendCodec == net::CodecType::H264
					? h264EncoderRequestedBitrateKbps
					: 0;
				sendTelemetry.h264EncoderTargetBitrateKbps =
					sendCodec == net::CodecType::H264
					? h264EncoderTargetBitrateKbps
					: 0;
				sendTelemetry.h264EncoderAppliedBitrateKbps =
					sendCodec == net::CodecType::H264
					? h264EncoderBitrate / 1000u
					: 0;
				sendTelemetry.h264DynamicBitrateUpdateRequests =
					h264DynamicBitrateUpdateRequests;
				sendTelemetry.h264DynamicBitrateUpdateSuccesses =
					h264DynamicBitrateUpdateSuccesses;
				sendTelemetry.h264DynamicBitrateUpdateFailures =
					h264DynamicBitrateUpdateFailures;
				sendTelemetry.h264EncoderReinitializations =
					h264EncoderReinitializations;
				sendTelemetry.pacingTargetBitrateKbps =
					pacingTargetBitrateKbps;
				sendTelemetry.h264AuChunkCount =
					h264AuChunkCount;
				sendTelemetry.h264AuIsIdr =
					h264AuIsIdr;
				sendTelemetry.h264AuIsDecoderSync =
					h264AuIsDecoderSync;
				sendTelemetry.h264RequestedKeyFrameConsumedFrames =
					h264RequestedKeyFrameConsumedFrames;
				sendTelemetry.h264EncoderKeyFrameRequests =
					h264EncoderKeyFrameRequests;
				sendTelemetry.h264RecoveryKeyFrameRequests =
					h264RecoveryKeyFrameRequests;
				sendTelemetry.h264AwaitingSyncKeyFrameRequests =
					h264AwaitingSyncKeyFrameRequests;
				sendTelemetry.h264PeriodicIdrRequests =
					h264PeriodicIdrRequests;
				sendTelemetry.h264IdrFrames =
					h264IdrFrames;
				sendTelemetry.h264DecoderSyncFrames =
					h264DecoderSyncFrames;
				sendTelemetry.h264ConsecutiveIdrFrames =
					h264ConsecutiveIdrFrames;
				sendTelemetry.h264MaxConsecutiveIdrFrames =
					h264MaxConsecutiveIdrFrames;
				sendTelemetry.h264AuProtectionLevel =
					h264AuProtectionLevel;
				sendTelemetry.h264AuDroppedBeforeSend =
					h264AuDroppedBeforeSend;
				sendTelemetry.h264AuDropReason =
					h264AuDropReason;
				sendTelemetry.h264AuDroppedBytes =
					h264AuDroppedBytes;
				sendTelemetry.h264AuDroppedChunks =
					h264AuDroppedChunks;
				sendTelemetry.h264AuPacingQueueDelayMs =
					h264AuPacingQueueDelayMs;
				sendTelemetry.h264AuEstimatedSendMs =
					h264AuEstimatedSendMs;
				sendTelemetry.h264InputGatedByPacing =
					h264InputGatedByPacing;
				sendTelemetry.h264InputGateReason =
					h264InputGateReason;
				sendTelemetry.h264InputGateQueueDelayMs =
					h264InputGateQueueDelayMs;
				sendTelemetry.h264InputGateVideoCreditBytes =
					h264InputGateVideoCreditBytes;
				sendTelemetry.h264InputGateFrameBudgetBytes =
					h264InputGateFrameBudgetBytes;
				sendTelemetry.h264InputGateDurationMs =
					h264InputGateDurationMs;
				sendTelemetry.h264InputGateConsecutiveFrames =
					h264InputGateConsecutiveFrames;
				sendTelemetry.h264InputGateSkippedInputFrames =
					h264InputGateSkippedInputFrames;
				sendTelemetry.h264InputGateForcedOpen =
					h264InputGateForcedOpen;
				sendTelemetry.h264InputGateReleaseReason =
					h264InputGateReleaseReason;
				sendTelemetry.fecProtectedH264KeyFrames =
					fecProtectedH264KeyFrames;
				sendTelemetry.fecProtectedH264LargeFrames =
					fecProtectedH264LargeFrames;
				sendTelemetry.h264EncoderDelayFrames =
					h264EncoderDelayFrames;
				sendTelemetry.h264EncoderDelayMs =
					h264EncoderDelayMs;
				sendTelemetry.h264EncoderPendingFrames =
					h264EncoderPendingFrames;
				sendTelemetry.h264EncodedInputFrameId =
					h264EncodedInputFrameId;
				sendTelemetry.h264EncoderCallMs =
					h264EncoderTiming.callMs;
				sendTelemetry.h264EncoderSampleCreateMs =
					h264EncoderTiming.sampleCreateMs;
				sendTelemetry.h264EncoderProcessInputMs =
					h264EncoderTiming.processInputMs;
				sendTelemetry.h264EncoderPreInputPollMs =
					h264EncoderTiming.preInputPollMs;
				sendTelemetry.h264EncoderPostInputWaitMs =
					h264EncoderTiming.postInputWaitMs;
				sendTelemetry.h264EncoderProcessOutputMs =
					h264EncoderTiming.processOutputMs;
				sendTelemetry.h264EncoderOutputCopyMs =
					h264EncoderTiming.outputCopyMs;
				sendTelemetry.h264EncoderProcessOutputAttempts =
					h264EncoderTiming.processOutputAttempts;
				sendTelemetry.h264EncoderAsyncEventCount =
					h264EncoderTiming.asyncEventCount;
				sendTelemetry.h264EncoderHardware =
					h264EncoderTiming.hardware;
				sendTelemetry.h264EncoderAsync =
					h264EncoderTiming.async;
				sendTelemetry.h264EncoderNeedInputSignaled =
					h264EncoderTiming.needInputSignaled;
				sendTelemetry.h264EncoderOutputProduced =
					h264EncoderTiming.outputProduced;
				sendTelemetry.h264EncoderOutputProducedBeforeInput =
					h264EncoderTiming.outputProducedBeforeInput;
				sendTelemetry.h264SubmittedNewInput =
					h264SubmittedNewInput;
				sendTelemetry.h264AsyncSubmittedWithoutOutput =
					h264AsyncSubmittedWithoutOutput;
				sendTelemetry.h264AsyncPendingNoOutput = false;
				sendTelemetry.h264AsyncCadenceHoldActive =
					h264AsyncCadenceHoldActive;
				sendTelemetry.h264AsyncPendingNoOutputStreak =
					h264AsyncPendingNoOutputStreak;
				sendTelemetry.h264AsyncCadenceScale =
					h264AsyncCadenceScale;
				sendTelemetry.h264AsyncCadenceHoldRemainingMs =
					h264AsyncCadenceHoldRemainingMs;
				sendTelemetry.h264AsyncOutputPollBackoffMs = 0.0;
				sendTelemetry.h264InputCadenceFps =
					static_cast<uint32_t>(inputCadenceFps);
				sendTelemetry.h264NeedInputSubmitWake =
					h264NeedInputSubmitWake;
				sendTelemetry.h264NeedInputSubmitLeadMs =
					h264NeedInputSubmitLeadMs;
				sendTelemetry.encodedCameraFrameId =
					encodedCameraFrameId;
				sendTelemetry.encodedCameraSourceTimestamp100ns =
					encodedCameraSourceTimestamp100ns;
				sendTelemetry.encodedCameraCaptureCompletedTimeUs =
					encodedCameraCaptureCompletedTimeUs;
				sendTelemetry.encodedCameraFrameAgeMs =
					encodedCameraFrameAgeMs;
				sendTelemetry.encodedCameraReadSampleMs =
					encodedCameraReadSampleMs;
				sendTelemetry.encodedCameraReadSampleEndToCaptureMs =
					encodedCameraReadSampleEndToCaptureMs;
				sendTelemetry.encodedCameraCaptureToPublishMs =
					encodedCameraCaptureToPublishMs;
				sendTelemetry.encodedCameraPublishToAcquireMs =
					encodedCameraPublishToAcquireMs;
				sendTelemetry.encodedCameraAcquireToEncoderInputMs =
					encodedCameraAcquireToEncoderInputMs;
				sendTelemetry.jpegEncodeMs = jpegEncodeMs;
				sendTelemetry.packetizeMs = packetizeMs;
			}

			bool sendAsKeyFrame =
				requestedKeyFrame ||
				sendCodec == net::CodecType::MJPEG;
			if (sendCodec == net::CodecType::H264) {
				if (h264AuHeaderValid) {
					sendAsKeyFrame =
						sendAsKeyFrame ||
						h264AuIsIdr ||
						h264AuIsDecoderSync;
				}
			}

			{
				std::lock_guard<std::mutex> lock(*adaptiveMutex);
				adaptiveController->ReportEncodedFrame(
					static_cast<size_t>(actualRawFrameBytes),
					encodedPayload.size()
				);
			}

			if (!h264AuDroppedBeforeSend) {
				networkManager->SendRNVPFragmented(
					encodedPayload,
					rnvpSendFrameId,
					sendCodec,
					1,
					sendAsKeyFrame,
					protection
				);
			}

			if (sendCodec == net::CodecType::H264 &&
				(sendAsKeyFrame || h264AuDroppedBeforeSend)) {
				const double frameBudgetBytes =
					static_cast<double>(pacingTargetBitrateKbps) *
					1000.0 /
					8.0 /
					static_cast<double>((std::max)(1, targetFps));
				const double burstBytes =
					static_cast<double>(encodedPayload.size());
				if (frameBudgetBytes > 0.0 &&
					burstBytes > frameBudgetBytes * 1.35) {
					const uint32_t skipFrames =
						static_cast<uint32_t>(
							std::ceil(
								(burstBytes - frameBudgetBytes) /
								frameBudgetBytes));
					h264PostKeyFrameSkipFrames =
						(std::min<uint32_t>)(
							2u,
							(std::max<uint32_t>)(
								h264PostKeyFrameSkipFrames,
								skipFrames));
				}
			}

			const bool advancedInputFrame =
				sendCodec != net::CodecType::H264 ||
				h264SubmittedNewInput;
			if (advancedInputFrame) {
				frameId++;
			}

			if ((frameId % 60) == 0) {
				std::ostringstream oss;
				oss << "[AppMain] RNVP video frame sent. frameId="
					<< rnvpSendFrameId
					<< " targetFps="
					<< targetFps
					<< " targetBitrateKbps="
					<< adaptiveState.targetBitrateKbps
					<< " h264EncoderTargetKbps="
					<< (sendCodec == net::CodecType::H264
						? h264EncoderTargetBitrateKbps
						: 0)
					<< " pacingTargetKbps="
					<< pacingTargetBitrateKbps
					<< " targetQuality="
					<< adaptiveState.targetJpegQuality
					<< " targetResolution="
					<< targetWidth
					<< "x"
					<< targetHeight
					<< " codec="
					<< net::ToString(sendCodec)
					<< " source="
					<< (hasCameraFrame
						? (freshCameraFrame ? "camera" : "camera-cache")
						: (usedCameraCache ? "camera-hold" : "fallback"))
					<< " size="
					<< encodedPayload.size()
					<< " rawSize="
					<< actualRawFrameBytes
					<< " keyFrame="
					<< (sendAsKeyFrame ? "true" : "false")
					<< " requestedKeyFrame="
					<< (requestedKeyFrame ? "true" : "false");

				OutputDebugStringA(oss.str().c_str());
				OutputDebugStringA("\n");
			}

			if (advancedInputFrame) {
				nextSendTime +=
					std::chrono::duration_cast<
						std::chrono::steady_clock::duration
					>(sendInterval);
			}

			const auto scheduleNow = std::chrono::steady_clock::now();
			if (nextSendTime < scheduleNow - std::chrono::milliseconds(100)) {
				nextSendTime =
					scheduleNow +
					std::chrono::duration_cast<
						std::chrono::steady_clock::duration
					>(sendInterval);
			}
		}

		resetH264Encoder();
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

			if (networkExperimentRunner.IsActive()) {
				startNetworkExperimentReporter();
			}

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
				const NetworkManager::H264AckKeyFrameRequestTelemetry
					adaptiveAckKeyFrameTelemetry =
						networkManager->GetH264AckKeyFrameRequestTelemetry();
				adaptiveInput.h264KeyFrameRequestAckStaleAgeCooldownNoise =
					adaptiveAckKeyFrameTelemetry.staleAgeCooldownNoise;
				adaptiveInput.h264KeyFrameRequestAckStaleAgeCooldownSyncRisk =
					adaptiveAckKeyFrameTelemetry.staleAgeCooldownSyncRisk;
				adaptiveInput
					.h264KeyFrameRequestReceiverDeadlineNackMissingCooldownNoise =
					receiverStats
						.h264KeyFrameRequestReceiverDeadlineNackMissingCooldownNoise;
				adaptiveInput
					.h264KeyFrameRequestReceiverDeadlineNackMissingCooldownSyncRisk =
					receiverStats
						.h264KeyFrameRequestReceiverDeadlineNackMissingCooldownSyncRisk;
				adaptiveInput.h264KeyFrameRequestReceiverTrueSyncLoss =
					receiverStats.h264KeyFrameRequestReceiverTrueSyncLoss;
				adaptiveInput.h264KeyFrameRequestReceiverHardSyncLoss =
					receiverStats.h264KeyFrameRequestReceiverWaitingForIdr +
					receiverStats.h264KeyFrameRequestReceiverDecodeFailure +
					receiverStats.h264KeyFrameRequestReceiverInitWaitIdr +
					receiverStats.h264KeyFrameRequestReceiverAuInvalid +
					receiverStats
						.h264KeyFrameRequestReceiverPayloadHeaderFailure;
				adaptiveInput.receiveFreshnessDroppedFrames =
					videoReceiverStats.freshnessDroppedFrames;
				adaptiveInput.receiveDecodeQueueDroppedFrames =
					videoReceiverStats.decodeQueueDroppedFrames;
				adaptiveInput.receiveDecodeRenderOverwriteFrames =
					videoReceiverStats.decodeRenderOverwriteFrames;
				adaptiveInput.receiveDecodeInputFrameAgeMs =
					videoReceiverStats.decodeInputFrameAgeMs;
				adaptiveInput.receiveLatestDecodedFrameAgeMs =
					videoReceiverStats.latestDecodedFrameAgeMs;
				adaptiveInput.receiveFreshnessDropThresholdMs =
					videoReceiverStats.freshnessDropThresholdMs;
				adaptiveInput.receiveDecodeLastDropReason =
					videoReceiverStats.lastDropReason;
				adaptiveInput.receiveLastFreshnessDropAgeMs =
					videoReceiverStats.lastFreshnessDropAgeMs;
				adaptiveInput.lastOutputQueueDropReason =
					receiverStats.lastOutputQueueDropReason;
				adaptiveInput.receiveStartupElapsedMs =
					videoReceiverStats.startupElapsedMs;
				adaptiveInput.receiveSteadyDecodeLoopMaxPopGapMs =
					videoReceiverStats.steadyDecodeLoopMaxPopGapMs;
				adaptiveInput.receiveSteadyDecodeLoopMaxPopGapAtMs =
					videoReceiverStats.steadyDecodeLoopMaxPopGapAtMs;
				adaptiveInput.receiveSteadyDecodeLoopMaxPopGapArrivalRatio =
					videoReceiverStats.steadyDecodeLoopMaxPopGapArrivalRatio;
				adaptiveInput.receiveSteadyDecodeLoopMaxPopGapReceiverJitterMs =
					videoReceiverStats.steadyDecodeLoopMaxPopGapReceiverJitterMs;
				adaptiveInput.receiveSteadyDecodeLoopMaxPopGapClass =
					videoReceiverStats.steadyDecodeLoopMaxPopGapClass;
				const net::PacketPacerStats pacingStats =
					networkManager->GetPacingStats();
				adaptiveInput.pacingEnabled = pacingStats.enabled;
				adaptiveInput.pacingDeadlineDroppedPackets =
					pacingStats.deadlineDroppedPackets;
				adaptiveInput.pacingHighPriorityDeadlineDroppedPackets =
					pacingStats.highPriorityDeadlineDroppedPackets;
				adaptiveInput.pacingNormalDeadlineDroppedPackets =
					pacingStats.normalDeadlineDroppedPackets;
				adaptiveInput.pacingRepairTargetBitrateBps =
					pacingStats.repairTargetBitrateBps;
				adaptiveInput.pacingRepairSentBytes =
					pacingStats.repairSentBytes;
				adaptiveInput.pacingRepairBorrowedPackets =
					pacingStats.repairBorrowedPackets;
				adaptiveInput.pacingRepairBorrowedBytes =
					pacingStats.repairBorrowedBytes;
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
				adaptiveInput.retransmitUsefulChunks =
					receiverStats.retransmitUsefulChunks;
				adaptiveInput.retransmitDuplicatePackets =
					receiverStats.retransmitDuplicatePackets;
				adaptiveInput.retransmitLateAfterCompletedPackets =
					receiverStats.retransmitLateAfterCompletedPackets;
				adaptiveInput.retransmitLateAfterExpiredPackets =
					receiverStats.retransmitLateAfterExpiredPackets;
				adaptiveInput.retransmitLateAfterRejectedPackets =
					receiverStats.retransmitLateAfterRejectedPackets;
				const uint64_t adaptiveRetransmitClassifiedPackets =
					adaptiveInput.retransmitUsefulChunks +
					adaptiveInput.retransmitDuplicatePackets +
					adaptiveInput.retransmitLateAfterCompletedPackets +
					adaptiveInput.retransmitLateAfterExpiredPackets +
					adaptiveInput.retransmitLateAfterRejectedPackets;
				const uint64_t adaptiveAckRetransmittedChunks =
					networkManager->GetAckRetransmittedChunkCount();
				adaptiveInput.retransmitNotArrivedPackets =
					adaptiveAckRetransmittedChunks >=
					adaptiveRetransmitClassifiedPackets
					? adaptiveAckRetransmittedChunks -
						adaptiveRetransmitClassifiedPackets
					: 0;
				adaptiveInput.retransmitAccountedPackets =
					adaptiveRetransmitClassifiedPackets +
					adaptiveInput.retransmitNotArrivedPackets;
				adaptiveInput.retransmitFinalAccountingRatio =
					adaptiveAckRetransmittedChunks > 0
					? static_cast<double>(
						adaptiveInput.retransmitAccountedPackets) /
						static_cast<double>(adaptiveAckRetransmittedChunks)
					: 0.0;
				adaptiveInput.nackFecGraceSuppressedFrames =
					receiverStats.nackFecGraceSuppressedFrames;
				adaptiveInput.nackFecGraceSuppressedChunks =
					receiverStats.nackFecGraceSuppressedChunks;

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
