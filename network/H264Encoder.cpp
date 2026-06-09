#include "H264Encoder.h"
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d11.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <comdef.h>
#include <propvarutil.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <cassert>

#include <initguid.h> 

// SPS/PPS を挿入するかどうか（1 = 各IDRの先頭に追加）
DEFINE_GUID(CODECAPI_AVEncH264SPSPPSInsertion,
    0x6827f7f2, 0x52e5, 0x4d41, 0xb5, 0x1c, 0x42, 0xc2, 0xb6, 0x93, 0x23, 0x46);

// Media Foundation の SPS/PPS 挿入モード属性（IDRごと or 最初だけ）
DEFINE_GUID(MF_VIDEO_ENCODER_HEADER_INSERTION_MODE,
    0x5295e444, 0x1f7a, 0x4f44, 0xa0, 0xa3, 0x7f, 0x7e, 0x0b, 0x66, 0x95, 0x88);

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")     // CLSIDなどの定義
#pragma comment(lib, "wmcodecdspuuid.lib") // H.264 encoder MFT
#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

namespace {
void LogLine(const std::string& message) {
    OutputDebugStringA(message.c_str());
    OutputDebugStringA("\n");

    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    CreateDirectoryA("logs", nullptr);
    std::ofstream file("logs\\h264_encoder.log", std::ios::out | std::ios::app);
    if (file.is_open()) {
        file << message << '\n';
    }
}

std::string ReadEnvLower(const char* name) {
    char text[64]{};
    const DWORD length = GetEnvironmentVariableA(
        name,
        text,
        static_cast<DWORD>(sizeof(text)));
    if (length == 0 || length >= sizeof(text)) {
        return {};
    }

    std::string value(text, text + length);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool ForceSoftwareEncoder() {
    const std::string mode = ReadEnvLower("RNVP_H264_ENCODER");
    return mode == "software" || mode == "soft" || mode == "sw";
}

bool PreferHardwareEncoder() {
    const std::string mode = ReadEnvLower("RNVP_H264_ENCODER");
    return mode == "hardware" || mode == "hard" || mode == "hw";
}

bool EnvFlagEnabled(const char* name) {
    const std::string value = ReadEnvLower(name);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

UINT32 ReadEnvUInt32(const char* name, UINT32 fallback, UINT32 minValue, UINT32 maxValue) {
    char text[32]{};
    const DWORD length = GetEnvironmentVariableA(
        name,
        text,
        static_cast<DWORD>(sizeof(text)));
    if (length == 0 || length >= sizeof(text)) {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == text || parsed == 0) {
        return fallback;
    }

    const UINT32 clamped =
        (std::min<UINT32>)((std::max<UINT32>)(static_cast<UINT32>(parsed), minValue), maxValue);
    return clamped;
}

UINT32 ResolveH264GopSize(UINT32 fps) {
    const UINT32 safeFps = (std::max<UINT32>)(1, fps);
    if (EnvFlagEnabled("RNVP_H264_INTRA_ONLY")) {
        return 1;
    }
    return ReadEnvUInt32("RNVP_H264_GOP", safeFps * 2, 1, safeFps * 10);
}

UINT32 ResolveAsyncOutputWaitMs() {
    return ReadEnvUInt32("RNVP_H264_OUTPUT_WAIT_MS", 3, 0, 100);
}

void LogHr(const char* label, HRESULT hr) {
    char buffer[256]{};
    sprintf_s(buffer, "[H264Encoder] %s hr=0x%08X", label, static_cast<unsigned>(hr));
    LogLine(buffer);
}

std::string WideToUtf8(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }

    const int requiredBytes =
        WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (requiredBytes <= 1) {
        return {};
    }

    std::string utf8(static_cast<size_t>(requiredBytes - 1), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text,
        -1,
        utf8.data(),
        requiredBytes,
        nullptr,
        nullptr);
    return utf8;
}

bool SetCodecApiU32(IMFTransform* transform, const GUID& key, ULONG value, const char* label) {
    if (!transform) {
        return false;
    }

    ComPtr<ICodecAPI> codecApi;
    if (FAILED(transform->QueryInterface(IID_PPV_ARGS(&codecApi))) || !codecApi) {
        LogLine("[H264Encoder] ICodecAPI unavailable.");
        return false;
    }

    VARIANT variant{};
    VariantInit(&variant);
    variant.vt = VT_UI4;
    variant.ulVal = value;
    const HRESULT hr = codecApi->SetValue(&key, &variant);
    VariantClear(&variant);
    if (FAILED(hr)) {
        LogHr(label, hr);
        return false;
    }
    char buffer[256]{};
    sprintf_s(buffer, "[H264Encoder] %s applied=%lu", label, value);
    LogLine(buffer);
    return true;
}

bool SetCodecApiU64(IMFTransform* transform, const GUID& key, ULONGLONG value, const char* label) {
    if (!transform) {
        return false;
    }

    ComPtr<ICodecAPI> codecApi;
    if (FAILED(transform->QueryInterface(IID_PPV_ARGS(&codecApi))) || !codecApi) {
        LogLine("[H264Encoder] ICodecAPI unavailable.");
        return false;
    }

    VARIANT variant{};
    VariantInit(&variant);
    variant.vt = VT_UI8;
    variant.ullVal = value;
    const HRESULT hr = codecApi->SetValue(&key, &variant);
    VariantClear(&variant);
    if (FAILED(hr)) {
        LogHr(label, hr);
        return false;
    }
    char buffer[256]{};
    sprintf_s(buffer, "[H264Encoder] %s applied=%llu", label, value);
    LogLine(buffer);
    return true;
}

void ApplyLowLatencyCodecSettings(
    IMFTransform* transform,
    UINT32 bitrate,
    UINT32 fps,
    const char* phase) {
    if (!transform) {
        return;
    }

    char log[160]{};
    sprintf_s(log, "[H264Encoder] Applying low-latency settings: %s", phase);
    LogLine(log);

    const ULONG gopSize = ResolveH264GopSize(fps);

    SetCodecApiU32(transform, CODECAPI_AVLowLatencyMode, 1, "AVLowLatencyMode");
    SetCodecApiU32(transform, CODECAPI_AVEncCommonLowLatency, 1, "AVEncCommonLowLatency");
    SetCodecApiU32(transform, CODECAPI_AVEncCommonRealTime, 1, "AVEncCommonRealTime");
    SetCodecApiU32(transform, CODECAPI_AVEncCommonQualityVsSpeed, 100, "AVEncCommonQualityVsSpeed");
    SetCodecApiU32(transform, CODECAPI_AVEncCommonMeanBitRate, bitrate, "AVEncCommonMeanBitRate");
    SetCodecApiU32(transform, CODECAPI_AVEncCommonMaxBitRate, bitrate, "AVEncCommonMaxBitRate");
    SetCodecApiU32(transform, CODECAPI_AVEncVideoMaxKeyframeDistance, gopSize, "AVEncVideoMaxKeyframeDistance");
    SetCodecApiU32(transform, CODECAPI_AVEncMPVGOPSize, gopSize, "AVEncMPVGOPSize");
    SetCodecApiU32(transform, CODECAPI_AVEncMPVGOPOpen, 0, "AVEncMPVGOPOpen");
    SetCodecApiU32(transform, CODECAPI_AVEncMPVDefaultBPictureCount, 0, "AVEncMPVDefaultBPictureCount");
    SetCodecApiU32(transform, CODECAPI_AVEncVideoMaxNumRefFrame, 1, "AVEncVideoMaxNumRefFrame");
    SetCodecApiU32(transform, CODECAPI_AVEncH264CABACEnable, 0, "AVEncH264CABACEnable");
}

void ApplyQuickSyncLatencyCodecSettings(
    IMFTransform* transform,
    UINT32 bitrate,
    UINT32 fps,
    const char* phase) {
    if (!transform) {
        return;
    }

    char log[192]{};
    sprintf_s(log, "[H264Encoder] Applying Quick Sync latency settings: %s", phase);
    LogLine(log);

    const ULONG safeFps = (std::max<UINT32>)(1, fps);
    const ULONG gopSize = ResolveH264GopSize(fps);
    const bool strictVbv = EnvFlagEnabled("RNVP_H264_STRICT_VBV");
    const ULONG oneFrameBufferBits =
        (std::max<ULONG>)(16000ul, static_cast<ULONG>(bitrate / safeFps));
    const ULONGLONG oneFrameInterval100ns =
        10000000ull / static_cast<ULONGLONG>(safeFps);

    if (strictVbv) {
        if (!SetCodecApiU32(
            transform,
            CODECAPI_AVEncCommonRateControlMode,
            eAVEncCommonRateControlMode_LowDelayVBR,
            "QuickSync AVEncCommonRateControlMode LowDelayVBR")) {
            SetCodecApiU32(
                transform,
                CODECAPI_AVEncCommonRateControlMode,
                eAVEncCommonRateControlMode_CBR,
                "QuickSync AVEncCommonRateControlMode CBR");
        }
        SetCodecApiU32(
            transform,
            CODECAPI_AVEncCommonBufferSize,
            oneFrameBufferBits,
            "QuickSync AVEncCommonBufferSize");
        SetCodecApiU64(
            transform,
            CODECAPI_AVEncCommonMeanBitRateInterval,
            oneFrameInterval100ns,
            "QuickSync AVEncCommonMeanBitRateInterval");
        SetCodecApiU32(
            transform,
            CODECAPI_AVEncCommonAllowFrameDrops,
            1,
            "QuickSync AVEncCommonAllowFrameDrops");
    }
    SetCodecApiU32(
        transform,
        CODECAPI_AVEncCommonMultipassMode,
        0,
        "QuickSync AVEncCommonMultipassMode");
    SetCodecApiU32(
        transform,
        CODECAPI_AVEncVideoTemporalLayerCount,
        1,
        "QuickSync AVEncVideoTemporalLayerCount");
    SetCodecApiU32(
        transform,
        CODECAPI_AVEncVideoMaxTemporalLayers,
        1,
        "QuickSync AVEncVideoMaxTemporalLayers");
    SetCodecApiU32(
        transform,
        CODECAPI_AVEncVideoNumGOPsPerIDR,
        1,
        "QuickSync AVEncVideoNumGOPsPerIDR");
    SetCodecApiU32(
        transform,
        CODECAPI_AVEncMPVGOPSizeMin,
        gopSize,
        "QuickSync AVEncMPVGOPSizeMin");
    SetCodecApiU32(
        transform,
        CODECAPI_AVEncMPVGOPSizeMax,
        gopSize,
        "QuickSync AVEncMPVGOPSizeMax");
    SetCodecApiU32(
        transform,
        CODECAPI_AVEncMPVGOPSInSeq,
        1,
        "QuickSync AVEncMPVGOPSInSeq");
}

void LogActivateName(IMFActivate* activate, const wchar_t* prefix) {
    if (!activate) {
        return;
    }

    WCHAR* name = nullptr;
    UINT32 nameLength = 0;
    if (SUCCEEDED(activate->GetAllocatedString(
            MFT_FRIENDLY_NAME_Attribute,
            &name,
            &nameLength)) &&
        name != nullptr) {
        OutputDebugStringW(prefix);
        OutputDebugStringW(name);
        OutputDebugStringW(L"\n");

        const std::string prefixUtf8 = WideToUtf8(prefix);
        const std::string nameUtf8 = WideToUtf8(name);
        if (!nameUtf8.empty()) {
            LogLine(prefixUtf8 + nameUtf8);
        }
        CoTaskMemFree(name);
    }
}

void UnlockAsyncTransformIfNeeded(IMFTransform* transform) {
    if (!transform) {
        return;
    }

    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(transform->GetAttributes(&attributes)) && attributes) {
        attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    }
}

bool ConfigureD3D11DeviceManager(
    IMFTransform* transform,
    ComPtr<ID3D11Device>& device,
    ComPtr<IMFDXGIDeviceManager>& deviceManager,
    UINT& resetToken) {
    if (!transform) {
        return false;
    }

    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selectedFeatureLevel{};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        featureLevels,
        static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0])),
        D3D11_SDK_VERSION,
        &device,
        &selectedFeatureLevel,
        &context);
    if (FAILED(hr)) {
        LogHr("D3D11CreateDevice for hardware H.264 encoder failed", hr);
        return false;
    }

    ComPtr<IMFDXGIDeviceManager> manager;
    UINT token = 0;
    hr = MFCreateDXGIDeviceManager(&token, &manager);
    if (FAILED(hr)) {
        LogHr("MFCreateDXGIDeviceManager failed", hr);
        return false;
    }

    hr = manager->ResetDevice(device.Get(), token);
    if (FAILED(hr)) {
        LogHr("IMFDXGIDeviceManager::ResetDevice failed", hr);
        return false;
    }

    hr = transform->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(manager.Get()));
    if (FAILED(hr)) {
        LogHr("MFT_MESSAGE_SET_D3D_MANAGER failed", hr);
        return false;
    }

    deviceManager = manager;
    resetToken = token;
    LogLine("[H264Encoder] D3D11 device manager attached to hardware encoder.");
    return true;
}

bool CreateD3D11Nv12InputSample(
    ID3D11Device* device,
    const BYTE* data,
    UINT dataSize,
    UINT width,
    UINT height,
    ComPtr<IMFSample>& sample) {
    if (device == nullptr || data == nullptr || dataSize == 0 ||
        width == 0 || height == 0) {
        return false;
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_NV12;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData{};
    initialData.pSysMem = data;
    initialData.SysMemPitch = width;
    initialData.SysMemSlicePitch = dataSize;

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device->CreateTexture2D(
        &textureDesc,
        &initialData,
        &texture);
    if (FAILED(hr)) {
        LogHr("CreateTexture2D NV12 input failed", hr);
        return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D),
        texture.Get(),
        0,
        FALSE,
        &buffer);
    if (FAILED(hr)) {
        LogHr("MFCreateDXGISurfaceBuffer input failed", hr);
        return false;
    }

    hr = buffer->SetCurrentLength(dataSize);
    if (FAILED(hr)) {
        LogHr("SetCurrentLength DXGI input buffer failed", hr);
        return false;
    }

    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        LogHr("MFCreateSample D3D11 input failed", hr);
        return false;
    }

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
        LogHr("AddBuffer D3D11 input sample failed", hr);
        sample.Reset();
        return false;
    }

    return true;
}

bool TryCreateHardwareEncoder(ComPtr<IMFTransform>& outEncoder) {
    if (ForceSoftwareEncoder()) {
        LogLine("[H264Encoder] RNVP_H264_ENCODER=software; using software encoder.");
        return false;
    }
    if (!PreferHardwareEncoder()) {
        LogLine("[H264Encoder] Trying hardware encoder by default.");
    }

    MFT_REGISTER_TYPE_INFO inputInfo{};
    inputInfo.guidMajorType = MFMediaType_Video;
    inputInfo.guidSubtype = MFVideoFormat_NV12;

    MFT_REGISTER_TYPE_INFO outputInfo{};
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype = MFVideoFormat_H264;

    {
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &inputInfo,
            &outputInfo,
            &activates,
            &count);
        if (FAILED(hr) || activates == nullptr || count == 0) {
            if (FAILED(hr)) {
                LogHr("MFTEnumEx hardware H.264 encoder failed", hr);
            }
            else {
                LogLine("[H264Encoder] No hardware H.264 encoder found.");
            }
            if (activates) {
                CoTaskMemFree(activates);
            }
            return false;
        }

        for (UINT32 i = 0; i < count; ++i) {
            if (!activates[i]) {
                continue;
            }

            LogActivateName(activates[i], L"[H264Encoder] Trying hardware encoder: ");

            ComPtr<IMFTransform> candidate;
            hr = activates[i]->ActivateObject(IID_PPV_ARGS(&candidate));
            if (SUCCEEDED(hr) && candidate) {
                UnlockAsyncTransformIfNeeded(candidate.Get());
                outEncoder = candidate;
                LogActivateName(activates[i], L"[H264Encoder] Selected hardware encoder: ");

                for (UINT32 j = 0; j < count; ++j) {
                    if (activates[j]) {
                        activates[j]->Release();
                    }
                }
                CoTaskMemFree(activates);
                return true;
            }
            LogHr("Activate hardware encoder failed", hr);
        }

        for (UINT32 i = 0; i < count; ++i) {
            if (activates[i]) {
                activates[i]->Release();
            }
        }
        CoTaskMemFree(activates);
    }

    return false;
}

bool CreatePreferredEncoder(
    ComPtr<IMFTransform>& outEncoder,
    bool allowHardware,
    bool& usingHardware) {
    usingHardware = false;
    if (allowHardware && TryCreateHardwareEncoder(outEncoder)) {
        usingHardware = true;
        return true;
    }

    HRESULT hr = CoCreateInstance(
        CLSID_CMSH264EncoderMFT,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&outEncoder));
    if (FAILED(hr) || !outEncoder) {
        return false;
    }

    LogLine("[H264Encoder] Selected software encoder: Microsoft H.264 Encoder MFT");
    return true;
}
}

bool H264Encoder::Initialize(UINT32 width, UINT32 height, UINT32 bitrate, UINT32 fps) {
    if (InitializeInternal(width, height, bitrate, fps, true)) {
        return true;
    }

    Shutdown();
    LogLine("[H264Encoder] Hardware encoder initialization failed; retrying software encoder.");
    return InitializeInternal(width, height, bitrate, fps, false);
}

bool H264Encoder::InitializeInternal(
    UINT32 width,
    UINT32 height,
    UINT32 bitrate,
    UINT32 fps,
    bool allowHardware) {
    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_ = bitrate;
    frameCount_ = 0;
    inputBufferBytes_ = 0;
    outputBufferBytes_ = 0;
    outputProvidesSamples_ = false;
    usingHardwareEncoder_ = false;
    asyncHardwareEncoder_ = false;
    hardwareNeedsInput_ = false;
    dxgiDeviceManagerResetToken_ = 0;
    spsPpsBuffer_.clear();
    asyncEventGenerator_.Reset();
    dxgiDeviceManager_.Reset();
    d3d11Device_.Reset();
    encoder_.Reset();

    

    // MFT インスタンス生成
    HRESULT hr = S_OK;
    if (!CreatePreferredEncoder(
            encoder_,
            allowHardware,
            usingHardwareEncoder_)) {
        return false;
    }

    if (usingHardwareEncoder_ &&
        !ConfigureD3D11DeviceManager(
            encoder_.Get(),
            d3d11Device_,
            dxgiDeviceManager_,
            dxgiDeviceManagerResetToken_)) {
        return false;
    }

    if (usingHardwareEncoder_) {
        HRESULT eventHr = encoder_.As(&asyncEventGenerator_);
        if (FAILED(eventHr) || !asyncEventGenerator_) {
            LogHr("Hardware encoder IMFMediaEventGenerator unavailable", eventHr);
            return false;
        }
        asyncHardwareEncoder_ = true;
        LogLine("[H264Encoder] Hardware encoder async event generator attached.");
    }

    ApplyLowLatencyCodecSettings(
        encoder_.Get(),
        bitrate,
        fps,
        "after-activate");
    if (usingHardwareEncoder_) {
        ApplyQuickSyncLatencyCodecSettings(
            encoder_.Get(),
            bitrate,
            fps,
            "after-activate");
    }

    
    // --- 出力タイプを H.264 に設定 ---
    ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) return false;

    hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return false;
    hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(hr)) return false;
    hr = MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
    if (FAILED(hr)) return false;
    hr = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (FAILED(hr)) return false;
   /* hr = outputType->SetUINT32(MF_MT_MPEG_SEQUENCE_HEADER, TRUE);
    if (FAILED(hr)) return false;*/
    hr = outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (FAILED(hr)) return false;
    outputType->SetUINT32(MF_LOW_LATENCY, TRUE);
    hr = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return false;
    hr = outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    if (FAILED(hr)) return false;


    // 🔽 ★追加：SPS/PPS を出力に含める
    hr = outputType->SetUINT32(CODECAPI_AVEncH264SPSPPSInsertion, TRUE);
    if (FAILED(hr)) {
        LogHr("optional output SPS/PPS insertion attribute rejected", hr);
    }

    // 🔽 ★追加：全キーフレームに SPS/PPS を入れる
    hr = outputType->SetUINT32(MF_VIDEO_ENCODER_HEADER_INSERTION_MODE, 1);
    if (FAILED(hr)) {
        LogHr("optional output header insertion mode rejected", hr);
    }
   
    hr = encoder_->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr)) {
        LogHr("SetOutputType failed", hr);
        return false;
    }
    ApplyLowLatencyCodecSettings(
        encoder_.Get(),
        bitrate,
        fps,
        "after-output-type");
    if (usingHardwareEncoder_) {
        ApplyQuickSyncLatencyCodecSettings(
            encoder_.Get(),
            bitrate,
            fps,
            "after-output-type");
    }

    // --- 入力タイプを NV12 に設定 ---
    ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return false;

    // 1) Major Type
    hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return false;

    // 2) Subtype (YUV)
    hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(hr)) return false;

    // 3) 解像度・フレームレート
    hr = MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height);
    if (FAILED(hr)) return false;
    hr = MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (FAILED(hr)) return false;
    inputType->SetUINT32(MF_LOW_LATENCY, TRUE);

    // 4) インタレースモード
    hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return false;

    // 5) デフォルトストライド（行バイト長）
    //    NV12 のストライドは width そのまま
    hr = inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    if (FAILED(hr)) return false;

    // 6) 固定サイズサンプル & 独立サンプル
    hr = inputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (FAILED(hr)) return false;
    hr = inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return false;

    // 7) サンプルあたりのバッファサイズ
    hr = inputType->SetUINT32(MF_MT_SAMPLE_SIZE, width * height * 3 / 2);
    if (FAILED(hr)) return false;

   

    GUID subtype = { 0 };
    inputType->GetGUID(MF_MT_SUBTYPE, &subtype);

    LPOLESTR guidStr = nullptr;
    HRESULT hr2 = StringFromCLSID(subtype, &guidStr);
    if (SUCCEEDED(hr2)) {
        OutputDebugStringW(L"[DEBUG] Encoder InputType Subtype: ");
        OutputDebugStringW(guidStr);
        OutputDebugStringW(L"\n");
        CoTaskMemFree(guidStr);  // メモリ解放
    }

    // 8) 入力タイプを MFT に設定
    hr = encoder_->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        OutputDebugStringA("SetInputType failed. 対応フォーマットを列挙します...\n");
        return false;
    }

    // ストリーミング開始通知
    ApplyLowLatencyCodecSettings(
        encoder_.Get(),
        bitrate,
        fps,
        "after-input-type");
    if (usingHardwareEncoder_) {
        ApplyQuickSyncLatencyCodecSettings(
            encoder_.Get(),
            bitrate,
            fps,
            "after-input-type");
    }

    MFT_INPUT_STREAM_INFO inputInfo{};
    hr = encoder_->GetInputStreamInfo(0, &inputInfo);
    if (FAILED(hr)) {
        return false;
    }
    inputBufferBytes_ =
        (std::max<DWORD>)(
            inputInfo.cbSize,
            static_cast<DWORD>(width * height * 3u / 2u));

    MFT_OUTPUT_STREAM_INFO outputInfo{};
    hr = encoder_->GetOutputStreamInfo(0, &outputInfo);
    if (FAILED(hr)) {
        return false;
    }
    outputBufferBytes_ = outputInfo.cbSize;
    outputProvidesSamples_ =
        (outputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    if (outputBufferBytes_ == 0) {
        outputBufferBytes_ =
            (std::max<DWORD>)(
                static_cast<DWORD>(width * height),
                1024u * 1024u);
    }

   // encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    return true;
}

void H264Encoder::RequestKeyFrame() {
    if (!encoder_) {
        return;
    }

    ComPtr<ICodecAPI> codecApi;
    if (FAILED(encoder_.As(&codecApi)) || !codecApi) {
        return;
    }

    VARIANT value{};
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = 1;
    codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &value);
    VariantClear(&value);
}

bool H264Encoder::SetTargetBitrate(UINT32 bitrate) {
    if (!encoder_) {
        return false;
    }
    if (bitrate == 0) {
        return false;
    }
    if (bitrate_ == bitrate) {
        return true;
    }

    const bool meanApplied =
        SetCodecApiU32(
            encoder_.Get(),
            CODECAPI_AVEncCommonMeanBitRate,
            bitrate,
            "dynamic AVEncCommonMeanBitRate");
    const bool maxApplied =
        SetCodecApiU32(
            encoder_.Get(),
            CODECAPI_AVEncCommonMaxBitRate,
            bitrate,
            "dynamic AVEncCommonMaxBitRate");
    if (meanApplied || maxApplied) {
        bitrate_ = bitrate;
        return true;
    }

    return false;
}

bool H264Encoder::ProcessAvailableOutput(std::vector<BYTE>& outH264Data) {
    outH264Data.clear();
    if (!encoder_) {
        return false;
    }

    HRESULT hr = S_OK;
    ComPtr<IMFSample> outputSample;
    ComPtr<IMFMediaBuffer> outBuffer;
    if (!outputProvidesSamples_) {
        hr = MFCreateSample(&outputSample);
        if (FAILED(hr)) {
            LogHr("MFCreateSample async output failed", hr);
            return false;
        }

        hr = MFCreateMemoryBuffer(outputBufferBytes_, &outBuffer);
        if (FAILED(hr)) {
            LogHr("MFCreateMemoryBuffer async output failed", hr);
            return false;
        }

        hr = outputSample->AddBuffer(outBuffer.Get());
        if (FAILED(hr)) {
            LogHr("AddBuffer async output sample failed", hr);
            return false;
        }
    }

    MFT_OUTPUT_DATA_BUFFER outputData{};
    if (!outputProvidesSamples_) {
        outputData.pSample = outputSample.Get();
    }
    DWORD status = 0;

    for (uint32_t outputAttempt = 0; outputAttempt < 3; ++outputAttempt) {
    hr = encoder_->ProcessOutput(0, 1, &outputData, &status);
    ComPtr<IMFCollection> outputEvents;
    if (outputData.pEvents) {
        outputEvents.Attach(outputData.pEvents);
        outputData.pEvents = nullptr;
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return true;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        LogLine("[H264Encoder] Async ProcessOutput requested stream change.");
        ComPtr<IMFMediaType> changedOutputType;
        HRESULT typeHr =
            encoder_->GetOutputAvailableType(0, 0, &changedOutputType);
        if (FAILED(typeHr) || !changedOutputType) {
            LogHr("GetOutputAvailableType after stream change failed", typeHr);
            return false;
        }

        typeHr = encoder_->SetOutputType(0, changedOutputType.Get(), 0);
        if (FAILED(typeHr)) {
            LogHr("SetOutputType after stream change failed", typeHr);
            return false;
        }

        MFT_OUTPUT_STREAM_INFO outputInfo{};
        typeHr = encoder_->GetOutputStreamInfo(0, &outputInfo);
        if (FAILED(typeHr)) {
            LogHr("GetOutputStreamInfo after stream change failed", typeHr);
            return false;
        }

        outputBufferBytes_ = outputInfo.cbSize;
        outputProvidesSamples_ =
            (outputInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        if (outputBufferBytes_ == 0) {
            outputBufferBytes_ =
                (std::max<DWORD>)(
                    static_cast<DWORD>(width_ * height_),
                    1024u * 1024u);
        }

        outputSample.Reset();
        outBuffer.Reset();
        if (!outputProvidesSamples_) {
            HRESULT sampleHr = MFCreateSample(&outputSample);
            if (FAILED(sampleHr)) {
                LogHr("MFCreateSample stream-change output failed", sampleHr);
                return false;
            }
            sampleHr = MFCreateMemoryBuffer(outputBufferBytes_, &outBuffer);
            if (FAILED(sampleHr)) {
                LogHr("MFCreateMemoryBuffer stream-change output failed", sampleHr);
                return false;
            }
            sampleHr = outputSample->AddBuffer(outBuffer.Get());
            if (FAILED(sampleHr)) {
                LogHr("AddBuffer stream-change output failed", sampleHr);
                return false;
            }
        }

        outputData = {};
        if (!outputProvidesSamples_) {
            outputData.pSample = outputSample.Get();
        }
        continue;
    }
    if (FAILED(hr)) {
        LogHr("Async ProcessOutput failed", hr);
        return false;
    }
    break;
    }

    if (FAILED(hr) || hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        LogHr("Async ProcessOutput did not settle", hr);
        return false;
    }

    ComPtr<IMFSample> providedOutputSample;
    ComPtr<IMFMediaBuffer> resultBuffer;
    if (outputProvidesSamples_) {
        if (outputData.pSample == nullptr) {
            LogLine("[H264Encoder] Async ProcessOutput returned no output sample.");
            return false;
        }
        providedOutputSample.Attach(outputData.pSample);
        outputData.pSample = nullptr;
        hr = providedOutputSample->ConvertToContiguousBuffer(&resultBuffer);
        if (FAILED(hr)) {
            LogHr("Async ConvertToContiguousBuffer output failed", hr);
            return false;
        }
    }
    else {
        resultBuffer = outBuffer;
    }

    BYTE* data = nullptr;
    DWORD len = 0;
    hr = resultBuffer->Lock(&data, nullptr, &len);
    if (FAILED(hr)) {
        LogHr("Lock async output buffer failed", hr);
        return false;
    }

    outH264Data.assign(data, data + len);
    resultBuffer->Unlock();
    ExtractSpsPps(outH264Data);
    return true;
}

void H264Encoder::ExtractSpsPps(const std::vector<BYTE>& h264Data) {
    if (!spsPpsBuffer_.empty() || h264Data.empty()) {
        return;
    }

    const uint8_t* ptr = h264Data.data();
    const uint8_t* end = h264Data.data() + h264Data.size();
    while (ptr + 4 < end) {
        if (ptr[0] == 0x00 &&
            ptr[1] == 0x00 &&
            ptr[2] == 0x00 &&
            ptr[3] == 0x01) {
            const uint8_t nalType = ptr[4] & 0x1F;
            if (nalType == 7 || nalType == 8) {
                const uint8_t* next = ptr + 4;
                while (next + 4 < end &&
                    !(next[0] == 0x00 &&
                        next[1] == 0x00 &&
                        next[2] == 0x00 &&
                        next[3] == 0x01)) {
                    ++next;
                }
                spsPpsBuffer_.insert(spsPpsBuffer_.end(), ptr, next);
                ptr = next;
            }
            else {
                ptr += 4;
            }
        }
        else {
            ++ptr;
        }
    }
}

bool H264Encoder::PumpHardwareEncoderEvents(
    DWORD timeoutMs,
    std::vector<BYTE>* outH264Data) {
    if (!asyncEventGenerator_) {
        return false;
    }

    const ULONGLONG start = GetTickCount64();
    bool sawEvent = false;
    for (;;) {
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = asyncEventGenerator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            if (GetTickCount64() - start >= timeoutMs) {
                return sawEvent;
            }
            Sleep(1);
            continue;
        }
        if (FAILED(hr)) {
            LogHr("Async hardware GetEvent failed", hr);
            return false;
        }
        if (!event) {
            continue;
        }

        sawEvent = true;
        MediaEventType type = MEUnknown;
        hr = event->GetType(&type);
        if (FAILED(hr)) {
            LogHr("Async hardware GetEvent type failed", hr);
            continue;
        }

        HRESULT eventStatus = S_OK;
        if (FAILED(event->GetStatus(&eventStatus)) || FAILED(eventStatus)) {
            LogHr("Async hardware event status failed", eventStatus);
            continue;
        }

        static uint32_t eventLogCount = 0;
        if (eventLogCount < 24) {
            char buffer[160]{};
            sprintf_s(
                buffer,
                "[H264Encoder] Async hardware event type=%u",
                static_cast<unsigned>(type));
            LogLine(buffer);
            ++eventLogCount;
        }

        if (type == METransformNeedInput) {
            hardwareNeedsInput_ = true;
        }
        else if (type == METransformHaveOutput) {
            std::vector<BYTE> output;
            if (!ProcessAvailableOutput(output)) {
                return false;
            }
            if (outH264Data != nullptr && !output.empty() && outH264Data->empty()) {
                *outH264Data = std::move(output);
            }
        }

        if (timeoutMs == 0) {
            return true;
        }
        if (GetTickCount64() - start >= timeoutMs) {
            return true;
        }
    }
}

bool H264Encoder::EncodeHardwareFrameAsync(
    const BYTE* data,
    UINT dataSize,
    std::vector<BYTE>& outH264Data) {
    outH264Data.clear();
    if (!encoder_ || !asyncEventGenerator_) {
        return false;
    }

    if (!hardwareNeedsInput_) {
        PumpHardwareEncoderEvents(10, &outH264Data);
    }

    if (!hardwareNeedsInput_) {
        static uint32_t notReadyLogCount = 0;
        if (notReadyLogCount < 12) {
            LogLine("[H264Encoder] Async hardware encoder did not signal NeedInput.");
            ++notReadyLogCount;
        }
        return false;
    }

    ComPtr<IMFSample> sample;
    if (!CreateD3D11Nv12InputSample(
            d3d11Device_.Get(),
            data,
            dataSize,
            width_,
            height_,
            sample)) {
        LogLine("[H264Encoder] Failed to create async hardware input sample.");
        return false;
    }

    sample->SetSampleTime(frameCount_ * 10000000 / fps_);
    sample->SetSampleDuration(10000000 / fps_);

    HRESULT hr = encoder_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
        LogHr("Async hardware ProcessInput failed", hr);
        return false;
    }

    hardwareNeedsInput_ = false;
    ++frameCount_;
    PumpHardwareEncoderEvents(ResolveAsyncOutputWaitMs(), &outH264Data);
    return true;
}


//bool H264Encoder::EncodeFrame(const BYTE* rgbData, UINT dataSize, std::vector<BYTE>& outH264Data) {
//    if (!encoder_) return false;
//
//    // 入力サンプル作成
//    ComPtr<IMFSample> sample;
//    MFCreateSample(&sample);
//
//   /* ComPtr<IMFMediaBuffer> buffer;
//    MFT_OUTPUT_STREAM_INFO info;
//    encoder_->GetOutputStreamInfo(0, &info);
//    MFCreateMemoryBuffer(info.cbSize, &buffer);*/
//
//
//    /*ComPtr<IMFMediaBuffer> buffer;
//    MFCreateMemoryBuffer(dataSize, &buffer);*/
//
//    ComPtr<IMFMediaBuffer> buffer;
//        // ここを GetInputStreamInfo に変更
//    MFT_INPUT_STREAM_INFO inInfo;
//    HRESULT hr = encoder_->GetInputStreamInfo(0, &inInfo);
//    if (FAILED(hr)) return false;
//    hr = MFCreateMemoryBuffer(inInfo.cbSize, &buffer);
//    if (FAILED(hr)) return false;
//
//    BYTE* dest = nullptr;
//    DWORD maxLen = 0;
//    buffer->Lock(&dest, &maxLen, nullptr);
//    memcpy(dest, rgbData, dataSize);
//    buffer->Unlock();
//    buffer->SetCurrentLength(dataSize);
//
//    sample->AddBuffer(buffer.Get());
//    sample->SetSampleTime(frameCount_ * 10000000 / fps_);
//    sample->SetSampleDuration(10000000 / fps_);
//    frameCount_++;
//
//    hr = encoder_->ProcessInput(0, sample.Get(), 0);
//    if (FAILED(hr)) return false;
//
//    // 出力の取得
//    MFT_OUTPUT_DATA_BUFFER outputData = {};
//    DWORD status = 0;
//    ComPtr<IMFSample> outputSample;
//    MFCreateSample(&outputSample);
//
//    //ComPtr<IMFMediaBuffer> outBuffer;
//    //MFCreateMemoryBuffer(1024 * 1024, &outBuffer); // 1MB
//
//    ComPtr<IMFMediaBuffer> outBuffer;
//        // ここで出力ストリーム情報から適切なサイズを取得
//    MFT_OUTPUT_STREAM_INFO outInfo;
//    hr = encoder_->GetOutputStreamInfo(0, &outInfo);
//    if (FAILED(hr)) return false;
//    hr = MFCreateMemoryBuffer(outInfo.cbSize, &outBuffer);
//    if (FAILED(hr)) return false;
//
//    outputSample->AddBuffer(outBuffer.Get());
//    outputData.pSample = outputSample.Get();
//    outputData.dwStreamID = 0;
//
//    hr = encoder_->ProcessOutput(0, 1, &outputData, &status);
//    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
//        return true; // データが出力されないが、エラーではない
//    }
//    else if (FAILED(hr)) {
//        return false;
//    }
//
//    // バッファからデータを取り出す
//    BYTE* data = nullptr;
//    DWORD len = 0;
//    outBuffer->Lock(&data, nullptr, &len);
//    outH264Data.assign(data, data + len);
//    outBuffer->Unlock();
//
//    // 最初の数フレームでSPS/PPSを抽出
//    if (spsPpsBuffer_.empty()) {
//        const uint8_t* ptr = data;
//        const uint8_t* end = data + len;
//        while (ptr + 4 < end) {
//            if (ptr[0] == 0x00 && ptr[1] == 0x00 && ptr[2] == 0x00 && ptr[3] == 0x01) {
//                uint8_t nalType = ptr[4] & 0x1F;
//                if (nalType == 7 || nalType == 8) {  // SPSまたはPPS
//                    const uint8_t* next = ptr + 4;
//                    while (next + 4 < end && !(next[0] == 0x00 && next[1] == 0x00 && next[2] == 0x00 && next[3] == 0x01)) {
//                        ++next;
//                    }
//                    spsPpsBuffer_.insert(spsPpsBuffer_.end(), ptr, next);
//                    ptr = next;
//                }
//                else {
//                    ptr += 4;
//                }
//            }
//            else {
//                ++ptr;
//            }
//        }
//    }
//
//
//    return true;
//}

bool H264Encoder::EncodeFrame(const BYTE* rgbData, UINT dataSize, std::vector<BYTE>& outH264Data) {
    if (!encoder_) {
#ifdef _DEBUG
        LogLine("[H264Encoder] EncodeFrame failed: encoder is not initialized.");
#endif
        return false;
    }

    if (rgbData == nullptr || dataSize == 0) {
#ifdef _DEBUG
        LogLine("[H264Encoder] EncodeFrame failed: input is empty.");
#endif
        return false;
    }

    if (asyncHardwareEncoder_) {
        return EncodeHardwareFrameAsync(rgbData, dataSize, outH264Data);
    }

    HRESULT hr = S_OK;

    // 入力サンプルの作成
    ComPtr<IMFSample> sample;
    if (usingHardwareEncoder_) {
        if (!CreateD3D11Nv12InputSample(
                d3d11Device_.Get(),
                rgbData,
                dataSize,
                width_,
                height_,
                sample)) {
#ifdef _DEBUG
            LogLine("[H264Encoder] Failed to create D3D11 NV12 input sample.");
#endif
            return false;
        }
    }
    else {
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
#ifdef _DEBUG
        LogHr("MFCreateSample input failed", hr);
#endif
        return false;
    }

    const DWORD inputBufferBytes = (std::max<DWORD>)(inputBufferBytes_, dataSize);
    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer(inputBufferBytes, &buffer);
    if (FAILED(hr)) {
#ifdef _DEBUG
        LogHr("MFCreateMemoryBuffer input failed", hr);
#endif
        return false;
    }

    BYTE* dest = nullptr;
    DWORD maxLen = 0;
    hr = buffer->Lock(&dest, &maxLen, nullptr);
    if (FAILED(hr)) {
#ifdef _DEBUG
        LogHr("Lock input buffer failed", hr);
#endif
        return false;
    }

    if (maxLen < dataSize) {
        buffer->Unlock();
#ifdef _DEBUG
        LogLine("[H264Encoder] H.264 input buffer smaller than NV12 frame.");
#endif
        return false;
    }

    memcpy(dest, rgbData, dataSize);
    buffer->Unlock();
    buffer->SetCurrentLength(dataSize);

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
#ifdef _DEBUG
        LogHr("AddBuffer input sample failed", hr);
#endif
        return false;
    }

    }

    sample->SetSampleTime(frameCount_ * 10000000 / fps_);
    sample->SetSampleDuration(10000000 / fps_);
    frameCount_++;

    hr = encoder_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
#ifdef _DEBUG
        LogHr("ProcessInput failed", hr);
#endif
        return false;
    }

    // 出力取得
    ComPtr<IMFSample> outputSample;
    ComPtr<IMFMediaBuffer> outBuffer;
    if (!outputProvidesSamples_) {
        hr = MFCreateSample(&outputSample);
        if (FAILED(hr)) {
#ifdef _DEBUG
            LogHr("MFCreateSample output failed", hr);
#endif
            return false;
        }

        hr = MFCreateMemoryBuffer(outputBufferBytes_, &outBuffer);
        if (FAILED(hr)) {
#ifdef _DEBUG
            LogHr("MFCreateMemoryBuffer output failed", hr);
#endif
            return false;
        }

        hr = outputSample->AddBuffer(outBuffer.Get());
        if (FAILED(hr)) {
#ifdef _DEBUG
            LogHr("AddBuffer output sample failed", hr);
#endif
            return false;
        }
    }

    MFT_OUTPUT_DATA_BUFFER outputData = {};
    if (!outputProvidesSamples_) {
        outputData.pSample = outputSample.Get();
    }
   // outputData.dwStreamID = 0;
    DWORD status = 0;

    hr = encoder_->ProcessOutput(0, 1, &outputData, &status);
    ComPtr<IMFCollection> outputEvents;
    if (outputData.pEvents) {
        outputEvents.Attach(outputData.pEvents);
        outputData.pEvents = nullptr;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
#ifdef _DEBUG
        static uint32_t needMoreInputLogCount = 0;
        if (needMoreInputLogCount < 12) {
            LogLine("[H264Encoder] ProcessOutput: NEED_MORE_INPUT");
            ++needMoreInputLogCount;
        }
#endif
        return true;
    }
    else if (FAILED(hr)) {
#ifdef _DEBUG
        LogHr("ProcessOutput failed", hr);
#endif
        return false;
    }

    ComPtr<IMFSample> providedOutputSample;
    ComPtr<IMFMediaBuffer> resultBuffer;
    if (outputProvidesSamples_) {
        if (outputData.pSample == nullptr) {
#ifdef _DEBUG
            LogLine("[H264Encoder] ProcessOutput returned no provided output sample.");
#endif
            return false;
        }
        providedOutputSample.Attach(outputData.pSample);
        outputData.pSample = nullptr;
        hr = providedOutputSample->ConvertToContiguousBuffer(&resultBuffer);
        if (FAILED(hr)) {
#ifdef _DEBUG
            LogHr("ConvertToContiguousBuffer output failed", hr);
#endif
            return false;
        }
    }
    else {
        resultBuffer = outBuffer;
    }

    BYTE* data = nullptr;
    DWORD len = 0;
    hr = resultBuffer->Lock(&data, nullptr, &len);
    if (FAILED(hr)) {
#ifdef _DEBUG
        LogHr("Lock output buffer failed", hr);
#endif
        return false;
    }

    outH264Data.assign(data, data + len);
    resultBuffer->Unlock();

    // SPS/PPS 抽出
    if (spsPpsBuffer_.empty()) {
        const uint8_t* ptr = outH264Data.data();
        const uint8_t* end = outH264Data.data() + outH264Data.size();
        while (ptr + 4 < end) {
            if (ptr[0] == 0x00 && ptr[1] == 0x00 && ptr[2] == 0x00 && ptr[3] == 0x01) {
                uint8_t nalType = ptr[4] & 0x1F;
                if (nalType == 7 || nalType == 8) {  // SPSまたはPPS
                    const uint8_t* next = ptr + 4;
                    while (next + 4 < end && !(next[0] == 0x00 && next[1] == 0x00 && next[2] == 0x00 && next[3] == 0x01)) {
                        ++next;
                    }
                    spsPpsBuffer_.insert(spsPpsBuffer_.end(), ptr, next);
                    ptr = next;
                }
                else {
                    ptr += 4;
                }
            }
            else {
                ++ptr;
            }
        }
#ifdef _DEBUG
        OutputDebugStringA("[INFO] SPS/PPS extracted\n");
#endif
    }

    return true;
}


void H264Encoder::Shutdown() {
    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder_.Reset();
    }
    asyncEventGenerator_.Reset();
    dxgiDeviceManager_.Reset();
    d3d11Device_.Reset();
    dxgiDeviceManagerResetToken_ = 0;
    usingHardwareEncoder_ = false;
    asyncHardwareEncoder_ = false;
    hardwareNeedsInput_ = false;
    bitrate_ = 0;
    /*CoUninitialize();*/

}

std::vector<uint8_t> H264Encoder::GetSpsPps() const {
    return spsPpsBuffer_;
}

//bool H264Encoder::EncodeSample(IMFSample* inputSample, std::vector<uint8_t>& outData) {
//    if (!encoder_) return false;
//
//    HRESULT hr = encoder_->ProcessInput(0, inputSample, 0);
//    if (FAILED(hr)) {
//        OutputDebugStringA("[ERROR] ProcessInput failed\n");
//        return false;
//    }
//
//    // 出力取得の試行
//    MFT_OUTPUT_STREAM_INFO streamInfo = {};
//    hr = encoder_->GetOutputStreamInfo(0, &streamInfo);
//    if (FAILED(hr)) {
//        OutputDebugStringA("[ERROR] GetOutputStreamInfo failed\n");
//        return false;
//    }
//
//    ComPtr<IMFMediaBuffer> outputBuffer;
//    hr = MFCreateMemoryBuffer(streamInfo.cbSize, &outputBuffer);
//    if (FAILED(hr)) {
//        OutputDebugStringA("[ERROR] MFCreateMemoryBuffer failed\n");
//        return false;
//    }
//
//    ComPtr<IMFSample> outputSample;
//    MFCreateSample(&outputSample);
//    outputSample->AddBuffer(outputBuffer.Get());
//
//    MFT_OUTPUT_DATA_BUFFER outputData = {};
//    outputData.pSample = outputSample.Get();
//    DWORD status = 0;
//
//    //if (!outputSample || !outputBuffer) {
//    //    OutputDebugStringA("[ERROR] outputSample or outputBuffer is null\n");
//    //    return false;
//    //}
//
//
//    hr = encoder_->ProcessOutput(0, 1, &outputData, &status);
//
//    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
//        OutputDebugStringA("[INFO] NEED_MORE_INPUT (正常、出力なし)\n");
//        return true; // 正常だが出力はまだない
//    }
//    else if (FAILED(hr)) {
//        char buf[128];
//        sprintf_s(buf, "[ERROR] ProcessOutput failed: 0x%08X\n", hr);
//        OutputDebugStringA(buf);
//        return false;
//    }
//
//    // 出力がある場合のみ outData に格納
//    BYTE* pData = nullptr;
//    DWORD maxLen = 0, curLen = 0;
//    hr = outputBuffer->Lock(&pData, &maxLen, &curLen);
//    if (SUCCEEDED(hr)) {
//        outData.assign(pData, pData + curLen);
//        outputBuffer->Unlock();
//    }
//    else {
//        OutputDebugStringA("[ERROR] Failed to lock output buffer\n");
//        return false;
//    }
//
//    return true;
//}

bool H264Encoder::EncodeSample(IMFSample* inputSample, std::vector<uint8_t>& outData) {
    if (!encoder_) return false;

    HRESULT hr = encoder_->ProcessInput(0, inputSample, 0);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] ProcessInput failed\n");
        return false;
    }

    // 出力取得の試行
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    hr = encoder_->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] GetOutputStreamInfo failed\n");
        return false;
    }

    ComPtr<IMFMediaBuffer> outputBuffer;
    hr = MFCreateMemoryBuffer(streamInfo.cbSize, &outputBuffer);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] MFCreateMemoryBuffer failed\n");
        return false;
    }

    ComPtr<IMFSample> outputSample;
    MFCreateSample(&outputSample);
    outputSample->AddBuffer(outputBuffer.Get());

    MFT_OUTPUT_DATA_BUFFER outputData = {};
    outputData.pSample = outputSample.Get();
    DWORD status = 0;

    hr = encoder_->ProcessOutput(0, 1, &outputData, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        OutputDebugStringA("[INFO] NEED_MORE_INPUT (正常、出力なし)\n");
        return true; // 正常だが出力はまだない
    }
    else if (FAILED(hr)) {
        char buf[128];
        sprintf_s(buf, "[ERROR] ProcessOutput failed: 0x%08X\n", hr);
        OutputDebugStringA(buf);
        return false;
    }

    // 出力がある場合のみ outData に格納
    BYTE* pData = nullptr;
    DWORD maxLen = 0, curLen = 0;
    hr = outputBuffer->Lock(&pData, &maxLen, &curLen);
    if (SUCCEEDED(hr)) {
        outData.assign(pData, pData + curLen);
        outputBuffer->Unlock();
    }
    else {
        OutputDebugStringA("[ERROR] Failed to lock output buffer\n");
        return false;
    }

//    // 🔧 毎回 SPS/PPS を先頭に追加
//    if (!spsPpsBuffer_.empty()) {
//        outData.insert(outData.begin(), spsPpsBuffer_.begin(), spsPpsBuffer_.end());
//#ifdef _DEBUG
//        OutputDebugStringA("[DEBUG] SPS/PPS prepended to encoded frame\n");
//
//        {
//            char log[128];
//            sprintf_s(log, "[DEBUG] spsPpsBuffer size: %zu\n", spsPpsBuffer_.size());
//            OutputDebugStringA(log);
//        }
//
//
//#endif
//    }

    return true;
}


bool H264Encoder::FlushDelayedFrames(
    std::vector<std::vector<BYTE>>& flushedFrames)
{
    if (!encoder_) return false;

    HRESULT hr = S_OK;
    // (1) 入力終了を通知
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (FAILED(hr)) {
        OutputDebugStringA("[Flush] NotifyEndOfStream failed.\n");
        return false;
    }

    // (2) ドレイン開始を指示
    hr = encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr)) {
        OutputDebugStringA("[Flush] CommandDrain failed.\n");
        return false;
    }

    // (3) ProcessOutput で残りを取り出す
    while (true) {
        // サンプルとバッファを用意
        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) break;

        MFT_OUTPUT_STREAM_INFO info = {};
        hr = encoder_->GetOutputStreamInfo(0, &info);
        if (FAILED(hr)) break;

        ComPtr<IMFMediaBuffer> buf;
        hr = MFCreateMemoryBuffer(info.cbSize, &buf);
        if (FAILED(hr)) break;

        sample->AddBuffer(buf.Get());

        MFT_OUTPUT_DATA_BUFFER outBuf = {};
        outBuf.pSample = sample.Get();
        outBuf.dwStreamID = 0;
        DWORD status = 0;

        hr = encoder_->ProcessOutput(0, 1, &outBuf, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            // もう出力なし
            break;
        }
        if (FAILED(hr)) {
            OutputDebugStringA("[Flush] ProcessOutput failed.\n");
            break;
        }

        // 成功したらバッファをロックしてデータ取得
        BYTE* pData = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = buf->Lock(&pData, &maxLen, &curLen);
        if (SUCCEEDED(hr) && curLen > 0) {
            flushedFrames.emplace_back(pData, pData + curLen);
            buf->Unlock();
            char log[128];
            sprintf_s(log, "[Flush] Got %u bytes\n", curLen);
            OutputDebugStringA(log);
        }
        // outBuf.pSample は MFCreateSample で作ったものなので Release は不要
    }

    return true;
}

