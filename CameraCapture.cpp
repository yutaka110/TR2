#include "CameraCapture.h"
#include <mfobjects.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <comdef.h>
#include <cstdio>

bool CameraCapture::Initialize(UINT32 width, UINT32 height) {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] MFStartup failed\n");
        return false;
    }

    IMFAttributes* attr = nullptr;
    hr = MFCreateAttributes(&attr, 1);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] MFCreateAttributes failed\n");
        return false;
    }

    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attr, &devices, &count);
    attr->Release();

    if (FAILED(hr) || count == 0) {
        OutputDebugStringA("[ERROR] No camera found\n");
        return false;
    }

    IMFMediaSource* mediaSource = nullptr;
    hr = devices[0]->ActivateObject(__uuidof(IMFMediaSource), (void**)&mediaSource);
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);

    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] ActivateObject failed\n");
        return false;
    }

    hr = MFCreateSourceReaderFromMediaSource(mediaSource, nullptr, &reader_);
    mediaSource->Release();
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] MFCreateSourceReaderFromMediaSource failed\n");
        return false;
    }

    // 設定：NV12
    IMFMediaType* mediaType = nullptr;
    hr = MFCreateMediaType(&mediaType);
    if (FAILED(hr)) return false;

    mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);

    // 解像度を明示的に設定
    UINT64 frameSize = ((UINT64)width << 32) | height;
    hr = mediaType->SetUINT64(MF_MT_FRAME_SIZE, frameSize);
    if (FAILED(hr)) OutputDebugStringA("[WARN] SetUINT64(MF_MT_FRAME_SIZE) failed\n");

    // フレームレートも設定（任意）
    hr = MFSetAttributeRatio(mediaType, MF_MT_FRAME_RATE, 30, 1);
    if (FAILED(hr)) OutputDebugStringA("[WARN] MF_MT_FRAME_RATE failed\n");

    hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mediaType);
    mediaType->Release();

    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] SetCurrentMediaType(NV12) failed\n");
        return false;
    }

    // ストリーム設定
    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    OutputDebugStringA("[INFO] CameraCapture initialized with NV12\n");
    return true;
}

bool CameraCapture::GetFrame(IMFSample** outSample) {
    DWORD streamIndex, flags;
    LONGLONG timestamp;
    *outSample = nullptr;
    HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &timestamp, outSample);
    return SUCCEEDED(hr) && *outSample != nullptr;
}

void CameraCapture::Shutdown() {
    if (reader_) {
        reader_->Release();
        reader_ = nullptr;
    }
    MFShutdown();
}
