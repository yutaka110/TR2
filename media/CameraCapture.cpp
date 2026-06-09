#include "CameraCapture.h"

#include <Windows.h>
#include <dshow.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>

#pragma comment(lib, "strmiids.lib")

namespace {
const char* VideoSubtypeName(const GUID& subtype) {
    if (subtype == MFVideoFormat_NV12) {
        return "NV12";
    }
    if (subtype == MFVideoFormat_YUY2) {
        return "YUY2";
    }
    if (subtype == MFVideoFormat_RGB32) {
        return "RGB32";
    }
    if (subtype == MFVideoFormat_MJPG) {
        return "MJPG";
    }
    if (subtype == MFVideoFormat_I420) {
        return "I420";
    }
    return "unknown";
}

uint8_t ClampByte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

void YuvToRgb(int y, int u, int v, uint8_t& r, uint8_t& g, uint8_t& b) {
    const int c = y - 16;
    const int d = u - 128;
    const int e = v - 128;
    r = ClampByte((298 * c + 409 * e + 128) >> 8);
    g = ClampByte((298 * c - 100 * d - 208 * e + 128) >> 8);
    b = ClampByte((298 * c + 516 * d + 128) >> 8);
}

std::string WideToUtf8(const wchar_t* text) {
    if (!text || text[0] == L'\0') {
        return {};
    }

    const int byteCount = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (byteCount <= 1) {
        return {};
    }

    std::string result(static_cast<size_t>(byteCount - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), byteCount, nullptr, nullptr);
    return result;
}
}

void CameraCapture::Log(const std::string& message) const {
    OutputDebugStringA(message.c_str());
    OutputDebugStringA("\n");

    std::ofstream file("camera_capture.log", std::ios::app);
    if (file) {
        file << message << "\n";
    }
}

void CameraCapture::LogHr(const char* label, HRESULT hr) const {
    std::ostringstream oss;
    oss << "[CameraCapture] "
        << label
        << " hr=0x"
        << std::hex
        << std::uppercase
        << static_cast<unsigned long>(hr);
    Log(oss.str());
}

void CameraCapture::LogDirectShowVideoDevices() const {
    ICreateDevEnum* devEnum = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_SystemDeviceEnum,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&devEnum)
    );
    if (FAILED(hr) || devEnum == nullptr) {
        LogHr("DirectShow CoCreateInstance(CLSID_SystemDeviceEnum) failed", hr);
        return;
    }

    IEnumMoniker* enumMoniker = nullptr;
    hr = devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    devEnum->Release();

    if (hr == S_FALSE || enumMoniker == nullptr) {
        Log("[CameraCapture] DirectShow video devices found: 0");
        return;
    }

    if (FAILED(hr)) {
        LogHr("DirectShow CreateClassEnumerator(video input) failed", hr);
        return;
    }

    uint32_t count = 0;
    IMoniker* moniker = nullptr;
    ULONG fetched = 0;
    while (enumMoniker->Next(1, &moniker, &fetched) == S_OK && moniker != nullptr) {
        ++count;

        IPropertyBag* propertyBag = nullptr;
        hr = moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&propertyBag));
        if (SUCCEEDED(hr) && propertyBag != nullptr) {
            VARIANT friendlyName;
            VariantInit(&friendlyName);
            hr = propertyBag->Read(L"FriendlyName", &friendlyName, nullptr);
            if (SUCCEEDED(hr) && friendlyName.vt == VT_BSTR) {
                std::ostringstream oss;
                oss << "[CameraCapture] DirectShow device["
                    << count - 1
                    << "]: "
                    << WideToUtf8(friendlyName.bstrVal);
                Log(oss.str());
            } else {
                LogHr("DirectShow property FriendlyName read failed", hr);
            }
            VariantClear(&friendlyName);

            VARIANT devicePath;
            VariantInit(&devicePath);
            hr = propertyBag->Read(L"DevicePath", &devicePath, nullptr);
            if (SUCCEEDED(hr) && devicePath.vt == VT_BSTR) {
                std::ostringstream oss;
                oss << "[CameraCapture] DirectShow device path["
                    << count - 1
                    << "]: "
                    << WideToUtf8(devicePath.bstrVal);
                Log(oss.str());
            }
            VariantClear(&devicePath);

            propertyBag->Release();
        } else {
            LogHr("DirectShow BindToStorage(IPropertyBag) failed", hr);
        }

        moniker->Release();
        moniker = nullptr;
    }

    enumMoniker->Release();

    std::ostringstream oss;
    oss << "[CameraCapture] DirectShow video devices found: " << count;
    Log(oss.str());
}

bool CameraCapture::Initialize(UINT32 width, UINT32 height) {
    {
        std::ofstream file("camera_capture.log", std::ios::trunc);
        if (file) {
            file << "[CameraCapture] log start\n";
        }
    }

    outputWidth_ = width;
    outputHeight_ = height;
    captureWidth_ = width;
    captureHeight_ = height;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        LogHr("MFStartup failed", hr);
        return false;
    }
    mfStarted_ = true;
    Log("[CameraCapture] MFStartup succeeded.");

    if (!OpenReader(width, height)) {
        Shutdown();
        return false;
    }

    return true;
}

bool CameraCapture::OpenReader(UINT32 width, UINT32 height) {
    IMFAttributes* attr = nullptr;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (FAILED(hr)) {
        LogHr("MFCreateAttributes failed", hr);
        return false;
    }

    attr->SetGUID(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
    );

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attr, &devices, &count);
    attr->Release();

    if (FAILED(hr) || count == 0 || devices == nullptr) {
        LogHr("MFEnumDeviceSources failed or no camera device found", hr);
        LogDirectShowVideoDevices();
        return false;
    }
    {
        std::ostringstream oss;
        oss << "[CameraCapture] Camera devices found: " << count;
        Log(oss.str());
    }

    IMFMediaSource* mediaSource = nullptr;
    hr = devices[0]->ActivateObject(__uuidof(IMFMediaSource), reinterpret_cast<void**>(&mediaSource));

    for (UINT32 i = 0; i < count; ++i) {
        if (devices[i]) {
            devices[i]->Release();
        }
    }
    CoTaskMemFree(devices);

    if (FAILED(hr) || mediaSource == nullptr) {
        LogHr("ActivateObject failed", hr);
        return false;
    }
    Log("[CameraCapture] Camera media source activated.");

    IMFAttributes* readerAttributes = nullptr;
    hr = MFCreateAttributes(&readerAttributes, 4);
    if (SUCCEEDED(hr)) {
        readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
        readerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
        readerAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        readerAttributes->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
    }

    hr = MFCreateSourceReaderFromMediaSource(mediaSource, readerAttributes, &reader_);

    if (readerAttributes) {
        readerAttributes->Release();
    }
    mediaSource->Release();

    if (FAILED(hr) || reader_ == nullptr) {
        LogHr("MFCreateSourceReaderFromMediaSource failed", hr);
        return false;
    }
    Log("[CameraCapture] Source reader created.");

    hr = reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(hr)) {
        LogHr("SetStreamSelection(all=false) failed", hr);
    }
    hr = reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    if (FAILED(hr)) {
        LogHr("SetStreamSelection(video=true) failed", hr);
    }

    if (!ConfigureLowLatencyOutput(width, height) &&
        !ConfigureRgb32Output(width, height, true)) {
        Log("[CameraCapture] Requested RGB32 size failed; falling back to camera default size.");
        if (!ConfigureRgb32Output(width, height, false)) {
            Log("[CameraCapture] Configure RGB32 output failed.");
            ReleaseReader();
            return false;
        }
    }

    UpdateCurrentFrameSize();

    std::ostringstream oss;
    oss << "[CameraCapture] Initialized "
        << VideoSubtypeName(captureSubtype_)
        << " capture "
        << captureWidth_
        << "x"
        << captureHeight_
        << " -> "
        << outputWidth_
        << "x"
        << outputHeight_
        << ".\n";
    Log(oss.str());
    return true;
}

void CameraCapture::ReleaseReader() {
    if (reader_) {
        reader_->Release();
        reader_ = nullptr;
    }
}

bool CameraCapture::RecoverReader() {
    if (!mfStarted_) {
        return false;
    }

    Log("[CameraCapture] Recovering camera source reader.");
    ReleaseReader();
    frameFailureLogCount_ = 0;

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    const bool recovered = OpenReader(outputWidth_, outputHeight_);
    Log(recovered
        ? "[CameraCapture] Camera source reader recovered."
        : "[CameraCapture] Camera source reader recovery failed.");
    return recovered;
}

bool CameraCapture::ConfigureLowLatencyOutput(UINT32 width, UINT32 height) {
    if (!reader_) {
        return false;
    }

    IMFMediaType* bestType = nullptr;
    int bestScore = (std::numeric_limits<int>::min)();
    UINT32 bestWidth = 0;
    UINT32 bestHeight = 0;
    GUID bestSubtype = GUID_NULL;

    for (DWORD index = 0;; ++index) {
        IMFMediaType* nativeType = nullptr;
        HRESULT hr = reader_->GetNativeMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            index,
            &nativeType);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr) || nativeType == nullptr) {
            continue;
        }

        GUID subtype = GUID_NULL;
        UINT32 nativeWidth = 0;
        UINT32 nativeHeight = 0;
        UINT32 fpsNum = 0;
        UINT32 fpsDen = 0;
        nativeType->GetGUID(MF_MT_SUBTYPE, &subtype);
        MFGetAttributeSize(nativeType, MF_MT_FRAME_SIZE, &nativeWidth, &nativeHeight);
        MFGetAttributeRatio(nativeType, MF_MT_FRAME_RATE, &fpsNum, &fpsDen);

        {
            std::ostringstream oss;
            oss
                << "[CameraCapture] Native format[" << index << "]: "
                << VideoSubtypeName(subtype)
                << " "
                << nativeWidth
                << "x"
                << nativeHeight;
            if (fpsNum != 0 && fpsDen != 0) {
                oss
                    << " @ "
                    << std::fixed
                    << std::setprecision(2)
                    << (static_cast<double>(fpsNum) / static_cast<double>(fpsDen))
                    << "fps";
            }
            Log(oss.str());
        }

        int subtypeScore = 0;
        if (subtype == MFVideoFormat_NV12) {
            subtypeScore = 300000;
        }
        else if (subtype == MFVideoFormat_YUY2) {
            subtypeScore = 200000;
        }
        else {
            nativeType->Release();
            continue;
        }

        const int sizeScore =
            (nativeWidth == width && nativeHeight == height)
            ? 50000
            : -static_cast<int>(
                std::abs(
                    static_cast<long long>(nativeWidth) *
                    static_cast<long long>(nativeHeight) -
                    static_cast<long long>(width) *
                    static_cast<long long>(height)) /
                1024);
        const int fpsScore =
            (fpsNum != 0 && fpsDen != 0)
            ? static_cast<int>(
                (std::min<double>)(
                    120.0,
                    static_cast<double>(fpsNum) / static_cast<double>(fpsDen)))
            : 0;
        const int score = subtypeScore + sizeScore + fpsScore;

        if (score > bestScore) {
            if (bestType) {
                bestType->Release();
            }
            bestType = nativeType;
            bestScore = score;
            bestWidth = nativeWidth;
            bestHeight = nativeHeight;
            bestSubtype = subtype;
        }
        else {
            nativeType->Release();
        }
    }

    if (!bestType) {
        Log("[CameraCapture] No native NV12/YUY2 format found; RGB32 fallback will be used.");
        return false;
    }

    HRESULT hr = reader_->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
        nullptr,
        bestType);
    bestType->Release();
    if (FAILED(hr)) {
        LogHr("SetCurrentMediaType native NV12/YUY2 failed", hr);
        return false;
    }

    std::ostringstream oss;
    oss
        << "[CameraCapture] Selected native low-latency format: "
        << VideoSubtypeName(bestSubtype)
        << " "
        << bestWidth
        << "x"
        << bestHeight
        << " -> "
        << width
        << "x"
        << height;
    Log(oss.str());
    return true;
}

bool CameraCapture::ConfigureRgb32Output(UINT32 width, UINT32 height, bool setFrameSize) {
    if (!reader_) {
        return false;
    }

    IMFMediaType* mediaType = nullptr;
    HRESULT hr = MFCreateMediaType(&mediaType);
    if (FAILED(hr) || mediaType == nullptr) {
        return false;
    }

    mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

    if (setFrameSize) {
        hr = MFSetAttributeSize(mediaType, MF_MT_FRAME_SIZE, width, height);
        if (FAILED(hr)) {
            LogHr("MFSetAttributeSize failed", hr);
        }
    }
    hr = MFSetAttributeRatio(mediaType, MF_MT_FRAME_RATE, 30, 1);
    if (FAILED(hr)) {
        LogHr("MFSetAttributeRatio failed", hr);
    }

    hr = reader_->SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
        nullptr,
        mediaType
    );
    mediaType->Release();

    if (FAILED(hr)) {
        LogHr(setFrameSize ? "SetCurrentMediaType RGB32 exact failed" : "SetCurrentMediaType RGB32 default failed", hr);
    }

    return SUCCEEDED(hr);
}

bool CameraCapture::UpdateCurrentFrameSize() {
    if (!reader_) {
        return false;
    }

    IMFMediaType* currentType = nullptr;
    HRESULT hr = reader_->GetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
        &currentType
    );
    if (FAILED(hr) || currentType == nullptr) {
        LogHr("GetCurrentMediaType failed", hr);
        return false;
    }

    UINT32 width = 0;
    UINT32 height = 0;
    GUID subtype = GUID_NULL;
    LONG stride = 0;
    currentType->GetGUID(MF_MT_SUBTYPE, &subtype);
    hr = MFGetAttributeSize(currentType, MF_MT_FRAME_SIZE, &width, &height);
    UINT32 strideAttribute = 0;
    if (SUCCEEDED(currentType->GetUINT32(
            MF_MT_DEFAULT_STRIDE,
            &strideAttribute))) {
        stride = static_cast<LONG>(strideAttribute);
    }
    else if (width > 0) {
        MFGetStrideForBitmapInfoHeader(subtype.Data1, width, &stride);
    }
    if (stride == 0 && width > 0) {
        if (subtype == MFVideoFormat_NV12) {
            stride = static_cast<LONG>(width);
        }
        else if (subtype == MFVideoFormat_YUY2) {
            stride = static_cast<LONG>(width) * 2;
        }
        else if (subtype == MFVideoFormat_RGB32) {
            stride = static_cast<LONG>(width) * 4;
        }
    }
    currentType->Release();

    if (SUCCEEDED(hr) && width > 0 && height > 0) {
        captureWidth_ = width;
        captureHeight_ = height;
        captureSubtype_ = subtype;
        captureStride_ = stride;
        std::ostringstream oss;
        oss
            << "[CameraCapture] Current media type: "
            << VideoSubtypeName(captureSubtype_)
            << " "
            << width
            << "x"
            << height
            << " stride="
            << captureStride_;
        Log(oss.str());
        return true;
    }

    LogHr("MFGetAttributeSize(MF_MT_FRAME_SIZE) failed", hr);
    return false;
}

uint64_t CameraCapture::NowMicroseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count());
}

bool CameraCapture::GetFrame(
    IMFSample** outSample,
    int64_t* outSourceTimestamp100ns,
    double* outReadSampleMs) {
    if (!reader_ || !outSample) {
        return false;
    }

    *outSample = nullptr;
    if (outSourceTimestamp100ns) {
        *outSourceTimestamp100ns = 0;
    }
    if (outReadSampleMs) {
        *outReadSampleMs = 0.0;
    }

    DWORD streamIndex = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    const auto readStart = std::chrono::steady_clock::now();
    HRESULT hr = reader_->ReadSample(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
        0,
        &streamIndex,
        &flags,
        &timestamp,
        outSample
    );
    const double readMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - readStart
        ).count();
    if (outSourceTimestamp100ns) {
        *outSourceTimestamp100ns = static_cast<int64_t>(timestamp);
    }
    if (outReadSampleMs) {
        *outReadSampleMs = readMs;
    }
    if (readMs > 250.0 && frameFailureLogCount_ < 30) {
        std::ostringstream oss;
        oss << "[CameraCapture] ReadSample slow. ms="
            << std::fixed
            << std::setprecision(2)
            << readMs
            << " flags=0x"
            << std::hex
            << std::uppercase
            << flags;
        Log(oss.str());
        ++frameFailureLogCount_;
    }

    if (FAILED(hr)) {
        if (frameFailureLogCount_ < 30) {
            LogHr("ReadSample failed", hr);
            ++frameFailureLogCount_;
        }
        return false;
    }

    if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
        Log("[CameraCapture] ReadSample media type changed; updating frame size.");
        UpdateCurrentFrameSize();
        return false;
    }

    if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 ||
        (flags & MF_SOURCE_READERF_ERROR) != 0) {
        if (frameFailureLogCount_ < 30) {
            std::ostringstream oss;
            oss << "[CameraCapture] ReadSample returned flags=0x"
                << std::hex
                << std::uppercase
                << flags;
            Log(oss.str());
            ++frameFailureLogCount_;
        }
        return false;
    }

    if (*outSample == nullptr && frameFailureLogCount_ < 30) {
        std::ostringstream oss;
        oss << "[CameraCapture] ReadSample returned no sample. flags=0x"
            << std::hex
            << std::uppercase
            << flags;
        Log(oss.str());
        ++frameFailureLogCount_;
    }
    if (*outSample == nullptr &&
        (flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
        const HRESULT flushHr = reader_->Flush(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
        if (FAILED(flushHr) && frameFailureLogCount_ < 30) {
            LogHr("Flush after stream tick failed", flushHr);
            ++frameFailureLogCount_;
        }
    }

    return *outSample != nullptr;
}

bool CameraCapture::TryGetRgbaFrame(std::vector<uint8_t>& outRgba) {
    FrameMetadata ignoredMetadata{};
    return TryGetRgbaFrame(outRgba, ignoredMetadata);
}

bool CameraCapture::TryGetRgbaFrame(
    std::vector<uint8_t>& outRgba,
    FrameMetadata& outMetadata) {
    outRgba.clear();
    outMetadata = {};

    if (!reader_ || outputWidth_ == 0 || outputHeight_ == 0) {
        return false;
    }

    IMFSample* sample = nullptr;
    int64_t sourceTimestamp100ns = 0;
    double readSampleMs = 0.0;
    if (!GetFrame(&sample, &sourceTimestamp100ns, &readSampleMs) ||
        sample == nullptr) {
        return false;
    }

    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    sample->Release();

    if (FAILED(hr) || buffer == nullptr) {
        if (frameFailureLogCount_ < 30) {
            LogHr("ConvertToContiguousBuffer failed", hr);
            ++frameFailureLogCount_;
        }
        return false;
    }

    BYTE* src = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    hr = buffer->Lock(&src, &maxLength, &currentLength);
    if (FAILED(hr) || src == nullptr) {
        if (frameFailureLogCount_ < 30) {
            LogHr("MediaBuffer Lock failed", hr);
            ++frameFailureLogCount_;
        }
        buffer->Release();
        return false;
    }

    const UINT32 srcWidth = (std::max<UINT32>)(1, captureWidth_);
    const UINT32 srcHeight = (std::max<UINT32>)(1, captureHeight_);
    const size_t availableBytes =
        static_cast<size_t>(currentLength != 0 ? currentLength : maxLength);
    const size_t srcPitch =
        captureStride_ != 0
        ? static_cast<size_t>(std::abs(captureStride_))
        : (captureSubtype_ == MFVideoFormat_YUY2
            ? static_cast<size_t>(srcWidth) * 2u
            : captureSubtype_ == MFVideoFormat_RGB32
            ? static_cast<size_t>(srcWidth) * 4u
            : static_cast<size_t>(srcWidth));

    if (availableBytes < 4u) {
        if (frameFailureLogCount_ < 30) {
            std::ostringstream oss;
            oss << "[CameraCapture] MediaBuffer too small. currentLength="
                << currentLength
                << " maxLength="
                << maxLength;
            Log(oss.str());
            ++frameFailureLogCount_;
        }
        buffer->Unlock();
        buffer->Release();
        return false;
    }

    outRgba.resize(
        static_cast<size_t>(outputWidth_) *
        static_cast<size_t>(outputHeight_) *
        4u
    );

    bool converted = false;

    if (captureSubtype_ == MFVideoFormat_NV12) {
        const size_t yPlaneBytes = srcPitch * static_cast<size_t>(srcHeight);
        const size_t uvPlaneBytes = srcPitch * static_cast<size_t>(srcHeight / 2u);
        if (availableBytes >= yPlaneBytes + uvPlaneBytes &&
            srcPitch >= srcWidth) {
            const uint8_t* yPlane = src;
            const uint8_t* uvPlane = src + yPlaneBytes;
            for (UINT32 y = 0; y < outputHeight_; ++y) {
                const UINT32 srcY =
                    (std::min<UINT32>)(
                        srcHeight - 1u,
                        (y * srcHeight) / outputHeight_);
                const uint8_t* yRow =
                    yPlane + static_cast<size_t>(srcY) * srcPitch;
                const uint8_t* uvRow =
                    uvPlane + static_cast<size_t>(srcY / 2u) * srcPitch;

                for (UINT32 x = 0; x < outputWidth_; ++x) {
                    const UINT32 srcX =
                        (std::min<UINT32>)(
                            srcWidth - 1u,
                            (x * srcWidth) / outputWidth_);
                    const UINT32 uvX = srcX & ~1u;
                    uint8_t r = 0;
                    uint8_t g = 0;
                    uint8_t b = 0;
                    YuvToRgb(yRow[srcX], uvRow[uvX], uvRow[uvX + 1u], r, g, b);

                    const size_t dstIndex =
                        (static_cast<size_t>(y) *
                            static_cast<size_t>(outputWidth_) +
                            static_cast<size_t>(x)) *
                        4u;
                    outRgba[dstIndex + 0] = r;
                    outRgba[dstIndex + 1] = g;
                    outRgba[dstIndex + 2] = b;
                    outRgba[dstIndex + 3] = 255;
                }
            }
            converted = true;
        }
    }
    else if (captureSubtype_ == MFVideoFormat_YUY2) {
        const size_t requiredBytes = srcPitch * static_cast<size_t>(srcHeight);
        if (availableBytes >= requiredBytes &&
            srcPitch >= static_cast<size_t>(srcWidth) * 2u) {
            for (UINT32 y = 0; y < outputHeight_; ++y) {
                const UINT32 srcY =
                    (std::min<UINT32>)(
                        srcHeight - 1u,
                        (y * srcHeight) / outputHeight_);
                const uint8_t* row =
                    src + static_cast<size_t>(srcY) * srcPitch;

                for (UINT32 x = 0; x < outputWidth_; ++x) {
                    const UINT32 srcX =
                        (std::min<UINT32>)(
                            srcWidth - 1u,
                            (x * srcWidth) / outputWidth_);
                    const UINT32 pairX = srcX & ~1u;
                    const uint8_t* pair = row + static_cast<size_t>(pairX) * 2u;
                    const int yy = pair[(srcX & 1u) ? 2u : 0u];
                    const int u = pair[1];
                    const int v = pair[3];
                    uint8_t r = 0;
                    uint8_t g = 0;
                    uint8_t b = 0;
                    YuvToRgb(yy, u, v, r, g, b);

                    const size_t dstIndex =
                        (static_cast<size_t>(y) *
                            static_cast<size_t>(outputWidth_) +
                            static_cast<size_t>(x)) *
                        4u;
                    outRgba[dstIndex + 0] = r;
                    outRgba[dstIndex + 1] = g;
                    outRgba[dstIndex + 2] = b;
                    outRgba[dstIndex + 3] = 255;
                }
            }
            converted = true;
        }
    }
    else if (captureSubtype_ == MFVideoFormat_RGB32) {
        const size_t requiredBytes = srcPitch * static_cast<size_t>(srcHeight);
        if (availableBytes >= requiredBytes &&
            srcPitch >= static_cast<size_t>(srcWidth) * 4u) {
            for (UINT32 y = 0; y < outputHeight_; ++y) {
                const UINT32 srcY =
                    (std::min<UINT32>)(
                        srcHeight - 1u,
                        (y * srcHeight) / outputHeight_);
                const uint8_t* row =
                    src + static_cast<size_t>(srcY) * srcPitch;

                for (UINT32 x = 0; x < outputWidth_; ++x) {
                    const UINT32 srcX =
                        (std::min<UINT32>)(
                            srcWidth - 1u,
                            (x * srcWidth) / outputWidth_);
                    const size_t srcIndex = static_cast<size_t>(srcX) * 4u;
                    const size_t dstIndex =
                        (static_cast<size_t>(y) *
                            static_cast<size_t>(outputWidth_) +
                            static_cast<size_t>(x)) *
                        4u;
                    outRgba[dstIndex + 0] = row[srcIndex + 2];
                    outRgba[dstIndex + 1] = row[srcIndex + 1];
                    outRgba[dstIndex + 2] = row[srcIndex + 0];
                    outRgba[dstIndex + 3] = 255;
                }
            }
            converted = true;
        }
    }

    buffer->Unlock();
    buffer->Release();
    if (!converted) {
        if (frameFailureLogCount_ < 30) {
            std::ostringstream oss;
            oss
                << "[CameraCapture] Unsupported or short frame buffer. subtype="
                << VideoSubtypeName(captureSubtype_)
                << " size="
                << srcWidth
                << "x"
                << srcHeight
                << " pitch="
                << srcPitch
                << " bytes="
                << availableBytes;
            Log(oss.str());
            ++frameFailureLogCount_;
        }
        outRgba.clear();
        return false;
    }

    frameFailureLogCount_ = 0;
    outMetadata.sourceTimestamp100ns = sourceTimestamp100ns;
    outMetadata.captureCompletedTimeUs = NowMicroseconds();
    outMetadata.readSampleMs = readSampleMs;
    return true;
}

bool CameraCapture::TryGetNv12Frame(Nv12Frame& outFrame) {
    outFrame = {};

    if (!reader_ || captureSubtype_ != MFVideoFormat_NV12) {
        return false;
    }

    IMFSample* sample = nullptr;
    int64_t sourceTimestamp100ns = 0;
    double readSampleMs = 0.0;
    if (!GetFrame(&sample, &sourceTimestamp100ns, &readSampleMs) ||
        sample == nullptr) {
        return false;
    }

    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    sample->Release();

    if (FAILED(hr) || buffer == nullptr) {
        if (frameFailureLogCount_ < 30) {
            LogHr("ConvertToContiguousBuffer NV12 failed", hr);
            ++frameFailureLogCount_;
        }
        return false;
    }

    BYTE* src = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    hr = buffer->Lock(&src, &maxLength, &currentLength);
    if (FAILED(hr) || src == nullptr) {
        if (frameFailureLogCount_ < 30) {
            LogHr("NV12 MediaBuffer Lock failed", hr);
            ++frameFailureLogCount_;
        }
        buffer->Release();
        return false;
    }

    const UINT32 srcWidth = (std::max<UINT32>)(1, captureWidth_);
    const UINT32 srcHeight = (std::max<UINT32>)(1, captureHeight_);
    const size_t srcPitch =
        captureStride_ != 0
        ? static_cast<size_t>(std::abs(captureStride_))
        : static_cast<size_t>(srcWidth);
    const size_t availableBytes =
        static_cast<size_t>(currentLength != 0 ? currentLength : maxLength);
    const size_t yPlaneBytes = srcPitch * static_cast<size_t>(srcHeight);
    const size_t uvPlaneBytes = srcPitch * static_cast<size_t>(srcHeight / 2u);

    if (srcPitch < srcWidth || availableBytes < yPlaneBytes + uvPlaneBytes) {
        if (frameFailureLogCount_ < 30) {
            std::ostringstream oss;
            oss
                << "[CameraCapture] NV12 buffer too short. size="
                << srcWidth
                << "x"
                << srcHeight
                << " pitch="
                << srcPitch
                << " bytes="
                << availableBytes;
            Log(oss.str());
            ++frameFailureLogCount_;
        }
        buffer->Unlock();
        buffer->Release();
        return false;
    }

    outFrame.width = srcWidth;
    outFrame.height = srcHeight;
    outFrame.yPitch = srcWidth;
    outFrame.uvPitch = srcWidth;
    outFrame.yPlane.resize(
        static_cast<size_t>(srcWidth) * static_cast<size_t>(srcHeight));
    outFrame.uvPlane.resize(
        static_cast<size_t>(srcWidth) * static_cast<size_t>(srcHeight / 2u));

    const uint8_t* yPlane = src;
    const uint8_t* uvPlane = src + yPlaneBytes;
    for (UINT32 y = 0; y < srcHeight; ++y) {
        std::memcpy(
            outFrame.yPlane.data() + static_cast<size_t>(y) * srcWidth,
            yPlane + static_cast<size_t>(y) * srcPitch,
            srcWidth);
    }
    for (UINT32 y = 0; y < srcHeight / 2u; ++y) {
        std::memcpy(
            outFrame.uvPlane.data() + static_cast<size_t>(y) * srcWidth,
            uvPlane + static_cast<size_t>(y) * srcPitch,
            srcWidth);
    }

    buffer->Unlock();
    buffer->Release();

    frameFailureLogCount_ = 0;
    outFrame.metadata.sourceTimestamp100ns = sourceTimestamp100ns;
    outFrame.metadata.captureCompletedTimeUs = NowMicroseconds();
    outFrame.metadata.readSampleMs = readSampleMs;
    return true;
}

void CameraCapture::StartAsyncCapture() {
    if (!reader_ || asyncRunning_.load()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(latestFrameMutex_);
        latestFrame_.clear();
        latestNv12Frame_ = {};
        latestFrameMetadata_ = {};
        latestFrameId_ = 0;
        latestFrameReady_ = false;
        asyncCaptureFps_ = 0.0;
        asyncCapturedFrames_ = 0;
        asyncCapturedFramesAtLastFpsUpdate_ = 0;
        asyncLastFpsUpdate_ = std::chrono::steady_clock::now();
    }

    asyncRunning_.store(true);
    asyncThread_ = std::thread(&CameraCapture::AsyncCaptureLoop, this);
    Log("[CameraCapture] Async capture started.");
}

void CameraCapture::StopAsyncCapture() {
    asyncRunning_.store(false);

    if (asyncThread_.joinable()) {
        asyncThread_.join();
    }
}

bool CameraCapture::TryGetLatestRgbaFrame(std::vector<uint8_t>& outRgba) {
    uint64_t ignoredFrameId = 0;
    return TryGetLatestRgbaFrame(outRgba, ignoredFrameId);
}

bool CameraCapture::TryGetLatestRgbaFrame(
    std::vector<uint8_t>& outRgba,
    uint64_t& outFrameId
) {
    FrameMetadata metadata{};
    const bool ok = TryGetLatestRgbaFrame(outRgba, metadata);
    outFrameId = metadata.frameId;
    return ok;
}

bool CameraCapture::TryGetLatestRgbaFrame(
    std::vector<uint8_t>& outRgba,
    FrameMetadata& outMetadata
) {
    std::lock_guard<std::mutex> lock(latestFrameMutex_);

    outMetadata = {};
    if (!latestFrameReady_ || latestFrame_.empty()) {
        return ConvertLatestNv12ToRgbaLocked(outRgba, outMetadata);
    }

    outRgba = latestFrame_;
    outMetadata = latestFrameMetadata_;
    return true;
}

bool CameraCapture::TryGetLatestNv12Frame(Nv12Frame& outFrame) {
    std::lock_guard<std::mutex> lock(latestFrameMutex_);

    outFrame = {};
    if (!latestFrameReady_ ||
        latestNv12Frame_.yPlane.empty() ||
        latestNv12Frame_.uvPlane.empty()) {
        return false;
    }

    outFrame = latestNv12Frame_;
    return true;
}

bool CameraCapture::ConvertLatestNv12ToRgbaLocked(
    std::vector<uint8_t>& outRgba,
    FrameMetadata& outMetadata) const {
    outRgba.clear();
    outMetadata = {};

    if (!latestFrameReady_ ||
        latestNv12Frame_.yPlane.empty() ||
        latestNv12Frame_.uvPlane.empty() ||
        latestNv12Frame_.width == 0 ||
        latestNv12Frame_.height == 0 ||
        latestNv12Frame_.yPitch < latestNv12Frame_.width ||
        latestNv12Frame_.uvPitch < latestNv12Frame_.width ||
        outputWidth_ == 0 ||
        outputHeight_ == 0) {
        return false;
    }

    outRgba.resize(
        static_cast<size_t>(outputWidth_) *
        static_cast<size_t>(outputHeight_) *
        4u);

    for (UINT32 y = 0; y < outputHeight_; ++y) {
        const UINT32 srcY =
            (std::min<UINT32>)(
                latestNv12Frame_.height - 1u,
                (y * latestNv12Frame_.height) / outputHeight_);
        const uint8_t* yRow =
            latestNv12Frame_.yPlane.data() +
            static_cast<size_t>(srcY) * latestNv12Frame_.yPitch;
        const uint8_t* uvRow =
            latestNv12Frame_.uvPlane.data() +
            static_cast<size_t>(srcY / 2u) * latestNv12Frame_.uvPitch;

        for (UINT32 x = 0; x < outputWidth_; ++x) {
            const UINT32 srcX =
                (std::min<UINT32>)(
                    latestNv12Frame_.width - 1u,
                    (x * latestNv12Frame_.width) / outputWidth_);
            const UINT32 uvX = srcX & ~1u;
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;
            YuvToRgb(yRow[srcX], uvRow[uvX], uvRow[uvX + 1u], r, g, b);

            const size_t dstIndex =
                (static_cast<size_t>(y) *
                    static_cast<size_t>(outputWidth_) +
                    static_cast<size_t>(x)) *
                4u;
            outRgba[dstIndex + 0] = r;
            outRgba[dstIndex + 1] = g;
            outRgba[dstIndex + 2] = b;
            outRgba[dstIndex + 3] = 255;
        }
    }

    outMetadata = latestNv12Frame_.metadata;
    return true;
}

double CameraCapture::GetAsyncCaptureFps() const {
    std::lock_guard<std::mutex> lock(latestFrameMutex_);
    return asyncCaptureFps_;
}

void CameraCapture::AsyncCaptureLoop() {
    constexpr auto kRecoverAfterNoFrame =
        std::chrono::milliseconds(1500);
    constexpr uint32_t kMinFailuresBeforeRecover = 30;

    auto lastFrameTime = std::chrono::steady_clock::now();
    uint32_t consecutiveFailures = 0;

    while (asyncRunning_.load()) {
        std::vector<uint8_t> frame;
        Nv12Frame nv12Frame;
        FrameMetadata metadata{};
        bool capturedNv12 = false;
        bool captured = false;
        if (captureSubtype_ == MFVideoFormat_NV12) {
            capturedNv12 = TryGetNv12Frame(nv12Frame);
            captured = capturedNv12;
            metadata = nv12Frame.metadata;
        }
        if (!captured) {
            captured = TryGetRgbaFrame(frame, metadata);
        }
        const auto now = std::chrono::steady_clock::now();

        if (captured &&
            (capturedNv12 ||
                !frame.empty())) {
            lastFrameTime = now;
            consecutiveFailures = 0;

            std::lock_guard<std::mutex> lock(latestFrameMutex_);
            latestFrameId_++;
            metadata.frameId = latestFrameId_;
            latestFrameMetadata_ = metadata;
            if (capturedNv12) {
                nv12Frame.metadata = metadata;
                latestNv12Frame_ = std::move(nv12Frame);
                latestFrame_.clear();
            }
            else {
                latestFrame_ = std::move(frame);
                latestNv12Frame_ = {};
            }
            latestFrameReady_ = true;
            asyncCapturedFrames_++;

            const double elapsedSec =
                std::chrono::duration<double>(
                    now - asyncLastFpsUpdate_
                ).count();
            if (elapsedSec >= 0.5) {
                const uint64_t capturedDelta =
                    asyncCapturedFrames_ -
                    asyncCapturedFramesAtLastFpsUpdate_;
                asyncCaptureFps_ =
                    static_cast<double>(capturedDelta) / elapsedSec;
                asyncCapturedFramesAtLastFpsUpdate_ =
                    asyncCapturedFrames_;
                asyncLastFpsUpdate_ = now;
            }
        }
        else {
            ++consecutiveFailures;
            const auto noFrameDuration = now - lastFrameTime;
            if (consecutiveFailures >= kMinFailuresBeforeRecover &&
                noFrameDuration >= kRecoverAfterNoFrame) {
                RecoverReader();
                lastFrameTime = std::chrono::steady_clock::now();
                consecutiveFailures = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void CameraCapture::Shutdown() {
    StopAsyncCapture();

    ReleaseReader();

    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
}
