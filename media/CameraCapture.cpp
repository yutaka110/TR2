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
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#pragma comment(lib, "strmiids.lib")

namespace {
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
    hr = MFCreateAttributes(&readerAttributes, 1);
    if (SUCCEEDED(hr)) {
        readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        readerAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
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

    if (!ConfigureRgb32Output(width, height, true)) {
        Log("[CameraCapture] Requested RGB32 size failed; falling back to camera default size.");
        if (!ConfigureRgb32Output(width, height, false)) {
            Log("[CameraCapture] Configure RGB32 output failed.");
            ReleaseReader();
            return false;
        }
    }

    UpdateCurrentFrameSize();

    std::ostringstream oss;
    oss << "[CameraCapture] Initialized RGB32 capture "
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
    hr = MFGetAttributeSize(currentType, MF_MT_FRAME_SIZE, &width, &height);
    currentType->Release();

    if (SUCCEEDED(hr) && width > 0 && height > 0) {
        captureWidth_ = width;
        captureHeight_ = height;
        std::ostringstream oss;
        oss << "[CameraCapture] Current media size: " << width << "x" << height;
        Log(oss.str());
        return true;
    }

    LogHr("MFGetAttributeSize(MF_MT_FRAME_SIZE) failed", hr);
    return false;
}

bool CameraCapture::GetFrame(IMFSample** outSample) {
    if (!reader_ || !outSample) {
        return false;
    }

    *outSample = nullptr;

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
    outRgba.clear();

    if (!reader_ || outputWidth_ == 0 || outputHeight_ == 0) {
        return false;
    }

    IMFSample* sample = nullptr;
    if (!GetFrame(&sample) || sample == nullptr) {
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

    const UINT32 srcWidth = std::max<UINT32>(1, captureWidth_);
    const UINT32 srcHeight = std::max<UINT32>(1, captureHeight_);
    const size_t requiredSrcBytes =
        static_cast<size_t>(srcWidth) * static_cast<size_t>(srcHeight) * 4u;
    const size_t availableBytes =
        static_cast<size_t>(currentLength != 0 ? currentLength : maxLength);

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

    const size_t safeSrcBytes =
        requiredSrcBytes < availableBytes ? requiredSrcBytes : availableBytes;
    const size_t safePixels = safeSrcBytes / 4u;

    for (UINT32 y = 0; y < outputHeight_; ++y) {
        const UINT32 srcY =
            std::min<UINT32>(srcHeight - 1u, (y * srcHeight) / outputHeight_);

        for (UINT32 x = 0; x < outputWidth_; ++x) {
            const UINT32 srcX =
                std::min<UINT32>(srcWidth - 1u, (x * srcWidth) / outputWidth_);
            const size_t srcPixel =
                static_cast<size_t>(srcY) * static_cast<size_t>(srcWidth) +
                static_cast<size_t>(srcX);
            const size_t dstIndex =
                (static_cast<size_t>(y) * static_cast<size_t>(outputWidth_) +
                    static_cast<size_t>(x)) * 4u;

            if (srcPixel < safePixels) {
                const size_t srcIndex = srcPixel * 4u;
                const uint8_t b = src[srcIndex + 0];
                const uint8_t g = src[srcIndex + 1];
                const uint8_t r = src[srcIndex + 2];

                outRgba[dstIndex + 0] = r;
                outRgba[dstIndex + 1] = g;
                outRgba[dstIndex + 2] = b;
                outRgba[dstIndex + 3] = 255;
            } else {
                outRgba[dstIndex + 0] = 0;
                outRgba[dstIndex + 1] = 0;
                outRgba[dstIndex + 2] = 0;
                outRgba[dstIndex + 3] = 255;
            }
        }
    }

    buffer->Unlock();
    buffer->Release();
    frameFailureLogCount_ = 0;
    return true;
}

void CameraCapture::StartAsyncCapture() {
    if (!reader_ || asyncRunning_.load()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(latestFrameMutex_);
        latestFrame_.clear();
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
    std::lock_guard<std::mutex> lock(latestFrameMutex_);

    if (!latestFrameReady_ || latestFrame_.empty()) {
        return false;
    }

    outRgba = latestFrame_;
    outFrameId = latestFrameId_;
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
        const bool captured = TryGetRgbaFrame(frame);
        const auto now = std::chrono::steady_clock::now();

        if (captured && !frame.empty()) {
            lastFrameTime = now;
            consecutiveFailures = 0;

            std::lock_guard<std::mutex> lock(latestFrameMutex_);
            latestFrame_ = std::move(frame);
            latestFrameId_++;
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
