#include "NetworkVideoReceiver.h"

#include "PacketProtocol.h"
#include "UdpReceiver.h"
#include "../externals/DirectXTex/DirectXTex.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <wrl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

namespace net {
namespace {

using Microsoft::WRL::ComPtr;

constexpr double kDefaultFreshnessDropThresholdMs = 120.0;

void SetCodecApiU32(IMFTransform* transform, const GUID& key, ULONG value) {
    if (!transform) {
        return;
    }

    ComPtr<ICodecAPI> codecApi;
    if (FAILED(transform->QueryInterface(IID_PPV_ARGS(&codecApi))) || !codecApi) {
        return;
    }

    VARIANT variant{};
    VariantInit(&variant);
    variant.vt = VT_UI4;
    variant.ulVal = value;
    codecApi->SetValue(&key, &variant);
    VariantClear(&variant);
}

enum class H264DecodeStatus {
    Decoded,
    NeedMoreInput,
    Failed
};

struct H264AccessUnitValidation {
    bool valid = false;
    bool decoderSync = false;
    bool hasIdr = false;
    bool hasSps = false;
    bool hasPps = false;
    uint16_t nalCount = 0;
    std::string reason;
};

const char* ToString(H264DecodeStatus status) {
    switch (status) {
    case H264DecodeStatus::Decoded:
        return "Decoded";
    case H264DecodeStatus::NeedMoreInput:
        return "NeedMoreInput";
    case H264DecodeStatus::Failed:
        return "Failed";
    default:
        return "Unknown";
    }
}

void TraceH264Receive(
    uint32_t frameId,
    uint32_t headerFrameId,
    uint8_t flags,
    bool decoderSync,
    bool decoderWasSynced,
    bool decoderHasSync,
    const char* decodeStatus,
    uint32_t width,
    uint32_t height,
    size_t payloadBytes,
    const char* reason) {
    const bool shouldLog =
        frameId <= 180 ||
        decoderSync ||
        !decoderHasSync ||
        (decodeStatus && std::strcmp(decodeStatus, "Decoded") != 0) ||
        (frameId % 60u) == 0u;
    if (!shouldLog) {
        return;
    }

    {
        std::ostringstream debug;
        debug
            << "[H264RX] frameId=" << frameId
            << " headerFrameId=" << headerFrameId
            << " flags=0x" << std::hex << static_cast<unsigned>(flags) << std::dec
            << " decoderSync=" << (decoderSync ? "true" : "false")
            << " decoderWasSynced=" << (decoderWasSynced ? "true" : "false")
            << " decoderHasSync=" << (decoderHasSync ? "true" : "false")
            << " decodeStatus=" << (decodeStatus ? decodeStatus : "")
            << " size=" << width << "x" << height
            << " payloadBytes=" << payloadBytes
            << " reason=" << (reason ? reason : "")
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
            "logs\\h264_receive_trace_%04u%02u%02u_%02u%02u%02u.csv",
            static_cast<unsigned>(localTime.wYear),
            static_cast<unsigned>(localTime.wMonth),
            static_cast<unsigned>(localTime.wDay),
            static_cast<unsigned>(localTime.wHour),
            static_cast<unsigned>(localTime.wMinute),
            static_cast<unsigned>(localTime.wSecond));

        traceFile.open(path, std::ios::out | std::ios::trunc);
        if (traceFile.is_open()) {
            traceFile
                << "frameId,headerFrameId,flags,decoderSync,"
                << "decoderWasSynced,decoderHasSync,decodeStatus,"
                << "width,height,payloadBytes,reason\n";
        }
    }

    if (!traceFile.is_open()) {
        return;
    }

    traceFile
        << frameId << ','
        << headerFrameId << ','
        << static_cast<unsigned>(flags) << ','
        << (decoderSync ? 1 : 0) << ','
        << (decoderWasSynced ? 1 : 0) << ','
        << (decoderHasSync ? 1 : 0) << ','
        << '"' << (decodeStatus ? decodeStatus : "") << '"' << ','
        << width << ','
        << height << ','
        << payloadBytes << ','
        << '"' << (reason ? reason : "") << '"'
        << '\n';
    traceFile.flush();
}

bool FindH264StartCode(
    const uint8_t* data,
    size_t size,
    size_t from,
    size_t& outOffset,
    size_t& outLength) {
    if (!data || from >= size) {
        return false;
    }

    for (size_t i = from; i + 2 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            outOffset = i;
            outLength = 3;
            return true;
        }
        if (i + 3 < size &&
            data[i] == 0 &&
            data[i + 1] == 0 &&
            data[i + 2] == 0 &&
            data[i + 3] == 1) {
            outOffset = i;
            outLength = 4;
            return true;
        }
    }

    return false;
}

H264AccessUnitValidation ValidateH264AccessUnit(
    const CompletedFrame& frame,
    const H264AccessUnitPayloadHeader& auHeader) {
    H264AccessUnitValidation result{};

    if (auHeader.headerBytes != kH264AccessUnitPayloadHeaderSize &&
        auHeader.headerBytes != kH264AccessUnitPayloadHeaderV2Size) {
        result.reason = "invalid-header-size";
        return result;
    }

    const size_t expectedPayloadBytes =
        static_cast<size_t>(auHeader.headerBytes) +
        static_cast<size_t>(auHeader.accessUnitBytes);
    if (frame.data.size() != expectedPayloadBytes) {
        result.reason = "payload-size-mismatch";
        return result;
    }

    if (auHeader.frameId != frame.frameId) {
        result.reason = "frame-id-mismatch";
        return result;
    }

    const uint8_t* accessUnit =
        frame.data.data() + auHeader.headerBytes;
    const size_t accessUnitBytes = auHeader.accessUnitBytes;

    if (auHeader.magic == kH264AccessUnitPayloadMagicV2) {
        const uint32_t calculatedCrc =
            ComputeCrc32(accessUnit, accessUnitBytes);
        if (calculatedCrc != auHeader.accessUnitCrc32) {
            result.reason = "crc-mismatch";
            return result;
        }
    }

    size_t offset = 0;
    size_t startCodeOffset = 0;
    size_t startCodeLength = 0;
    bool hasVcl = false;

    while (FindH264StartCode(
        accessUnit,
        accessUnitBytes,
        offset,
        startCodeOffset,
        startCodeLength)) {
        const size_t nalHeaderOffset = startCodeOffset + startCodeLength;
        if (nalHeaderOffset >= accessUnitBytes) {
            result.reason = "empty-nal";
            return result;
        }

        size_t nextStartCodeOffset = 0;
        size_t nextStartCodeLength = 0;
        const bool hasNextStartCode = FindH264StartCode(
            accessUnit,
            accessUnitBytes,
            nalHeaderOffset + 1,
            nextStartCodeOffset,
            nextStartCodeLength);
        const size_t nalEnd =
            hasNextStartCode ? nextStartCodeOffset : accessUnitBytes;
        if (nalEnd <= nalHeaderOffset) {
            result.reason = "zero-sized-nal";
            return result;
        }

        const uint8_t nalHeader = accessUnit[nalHeaderOffset];
        if ((nalHeader & 0x80u) != 0) {
            result.reason = "forbidden-zero-bit";
            return result;
        }

        const uint8_t nalType = nalHeader & 0x1Fu;
        result.nalCount++;
        result.hasIdr = result.hasIdr || nalType == 5;
        result.hasSps = result.hasSps || nalType == 7;
        result.hasPps = result.hasPps || nalType == 8;
        hasVcl = hasVcl || nalType == 1 || nalType == 5;

        offset = nalEnd;
        (void)nextStartCodeLength;
    }

    if (result.nalCount == 0) {
        result.reason = "no-annexb-nals";
        return result;
    }
    if (auHeader.nalUnitCount != 0 &&
        auHeader.nalUnitCount != result.nalCount) {
        result.reason = "nal-count-mismatch";
        return result;
    }
    if (!hasVcl) {
        result.reason = "no-vcl-nal";
        return result;
    }

    const bool headerIdr =
        (auHeader.flags & H264AccessUnitFlag_Idr) != 0;
    const bool headerSpsPps =
        (auHeader.flags & H264AccessUnitFlag_ContainsSpsPps) != 0;
    const bool headerSync =
        (auHeader.flags & H264AccessUnitFlag_DecoderSync) != 0;

    if (headerIdr != result.hasIdr) {
        result.reason = "idr-flag-mismatch";
        return result;
    }
    if (headerSpsPps != (result.hasSps || result.hasPps)) {
        result.reason = "sps-pps-flag-mismatch";
        return result;
    }
    if (headerSync && !result.hasIdr) {
        result.reason = "sync-without-idr";
        return result;
    }
    if (result.hasIdr && (!result.hasSps || !result.hasPps)) {
        result.reason = "idr-without-sps-pps";
        return result;
    }

    result.decoderSync = result.hasIdr && result.hasSps && result.hasPps;
    result.valid = true;
    result.reason = "ok";
    return result;
}

class H264CpuDecoder {
public:
    ~H264CpuDecoder() {
        Shutdown();
    }

    bool Initialize(uint32_t width, uint32_t height) {
        Shutdown();

        if (width == 0 || height == 0) {
            return false;
        }

        width_ = width;
        height_ = height;

        HRESULT hr = CoCreateInstance(
            CLSID_CMSH264DecoderMFT,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&decoder_));
        if (FAILED(hr)) {
            OutputDebugStringA("[NetworkVideoReceiver] H.264 decoder create failed.\n");
            return false;
        }

        ComPtr<IMFAttributes> decoderAttributes;
        if (SUCCEEDED(decoder_->GetAttributes(&decoderAttributes)) &&
            decoderAttributes) {
            decoderAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        SetCodecApiU32(decoder_.Get(), CODECAPI_AVLowLatencyMode, 1);

        ComPtr<IMFMediaType> inputType;
        hr = MFCreateMediaType(&inputType);
        if (FAILED(hr)) {
            return false;
        }
        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, 30, 1);
        inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = decoder_->SetInputType(0, inputType.Get(), 0);
        if (FAILED(hr)) {
            OutputDebugStringA("[NetworkVideoReceiver] H.264 decoder input type failed.\n");
            Shutdown();
            return false;
        }

        if (!SetOutputType()) {
            Shutdown();
            return false;
        }

        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        synced_ = false;
        discontinuity_ = true;
        return true;
    }

    void Shutdown() {
        if (decoder_) {
            decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            decoder_.Reset();
        }
        width_ = 0;
        height_ = 0;
        outputSubtype_ = GUID_NULL;
        outputStride_ = 0;
        outputSampleBytes_ = 0;
        synced_ = false;
        discontinuity_ = true;
    }

    bool IsInitializedFor(uint32_t width, uint32_t height) const {
        return decoder_ && width_ == width && height_ == height;
    }

    bool HasSync() const {
        return synced_;
    }

    void MarkReferenceBroken() {
        synced_ = false;
        discontinuity_ = true;
        if (decoder_) {
            decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
    }

    H264DecodeStatus Decode(
        const uint8_t* data,
        size_t size,
        uint64_t ptsUs,
        bool decoderSync,
        DecodedVideoFrame& outFrame) {
        outFrame.rgba.clear();
        outFrame.nv12Y.clear();
        outFrame.nv12UV.clear();
        outFrame.nv12YPitch = 0;
        outFrame.nv12UVPitch = 0;
        if (!decoder_ || !data || size == 0) {
            return H264DecodeStatus::Failed;
        }

        if (!synced_ && !decoderSync) {
            return H264DecodeStatus::NeedMoreInput;
        }
        const bool wasSyncedBeforeInput = synced_;

        ComPtr<IMFSample> sample;
        HRESULT hr = MFCreateSample(&sample);
        if (FAILED(hr)) {
            return H264DecodeStatus::Failed;
        }

        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
        if (FAILED(hr)) {
            return H264DecodeStatus::Failed;
        }

        BYTE* dst = nullptr;
        DWORD maxLen = 0;
        hr = buffer->Lock(&dst, &maxLen, nullptr);
        if (FAILED(hr)) {
            return H264DecodeStatus::Failed;
        }
        std::memcpy(dst, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(size));

        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(static_cast<LONGLONG>(ptsUs * 10));
        sample->SetSampleDuration(333333);

        if (decoderSync || discontinuity_) {
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(sample.As(&attrs)) && attrs) {
                attrs->SetUINT32(MFSampleExtension_CleanPoint, decoderSync ? TRUE : FALSE);
                if (discontinuity_) {
                    attrs->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
                }
            }
        }

        hr = decoder_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            DecodedVideoFrame discarded;
            DrainOne(discarded);
            hr = decoder_->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            MarkReferenceBroken();
            return H264DecodeStatus::Failed;
        }

        discontinuity_ = false;
        if (decoderSync) {
            synced_ = true;
        }

        H264DecodeStatus status = DrainOne(outFrame);
        if (status == H264DecodeStatus::Failed) {
            if (decoderSync && !wasSyncedBeforeInput) {
                return H264DecodeStatus::NeedMoreInput;
            }
            MarkReferenceBroken();
        }
        return status;
    }

private:
    static const char* FormatName(const GUID& subtype) {
        if (subtype == MFVideoFormat_NV12) {
            return "NV12";
        }
        if (subtype == MFVideoFormat_RGB32) {
            return "RGB32";
        }
        return "unknown";
    }

    bool TrySetOutputSubtype(const GUID& subtype) {
        ComPtr<IMFMediaType> outputType;
        HRESULT hr = MFCreateMediaType(&outputType);
        if (FAILED(hr)) {
            return false;
        }

        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, subtype);
        MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, 30, 1);
        outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = decoder_->SetOutputType(0, outputType.Get(), 0);
        if (FAILED(hr)) {
            return false;
        }

        ComPtr<IMFMediaType> currentType;
        if (SUCCEEDED(decoder_->GetOutputCurrentType(0, &currentType)) &&
            currentType) {
            GUID currentSubtype{};
            if (SUCCEEDED(currentType->GetGUID(MF_MT_SUBTYPE, &currentSubtype))) {
                outputSubtype_ = currentSubtype;
            }
            else {
                outputSubtype_ = subtype;
            }

            UINT32 strideAttribute = 0;
            if (SUCCEEDED(currentType->GetUINT32(
                    MF_MT_DEFAULT_STRIDE,
                    &strideAttribute))) {
                outputStride_ = static_cast<LONG>(strideAttribute);
            }
            else {
                outputStride_ = 0;
            }

            UINT32 sampleSize = 0;
            if (SUCCEEDED(currentType->GetUINT32(MF_MT_SAMPLE_SIZE, &sampleSize))) {
                outputSampleBytes_ = sampleSize;
            }
            else {
                outputSampleBytes_ = 0;
            }
        }
        else {
            outputSubtype_ = subtype;
            outputStride_ = 0;
            outputSampleBytes_ = 0;
        }

        if (outputStride_ == 0) {
            LONG defaultStride = 0;
            if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(
                    outputSubtype_.Data1,
                    width_,
                    &defaultStride))) {
                outputStride_ = defaultStride;
            }
        }

        if (outputSubtype_ == MFVideoFormat_RGB32) {
            if (outputStride_ == 0) {
                outputStride_ = static_cast<LONG>(width_) * 4;
            }
            const size_t strideBytes =
                static_cast<size_t>(std::abs(outputStride_));
            outputSampleBytes_ =
                (std::max)(
                    outputSampleBytes_,
                    strideBytes * static_cast<size_t>(height_));
        }
        else if (outputSubtype_ == MFVideoFormat_NV12) {
            if (outputStride_ == 0) {
                outputStride_ = static_cast<LONG>(width_);
            }
            const size_t strideBytes =
                static_cast<size_t>(std::abs(outputStride_));
            outputSampleBytes_ =
                (std::max)(
                    outputSampleBytes_,
                    strideBytes * static_cast<size_t>(height_) * 3u / 2u);
        }
        else {
            outputStride_ = 0;
            outputSampleBytes_ = 0;
            return false;
        }

        std::ostringstream oss;
        oss
            << "[NetworkVideoReceiver] H.264 decoder output selected: "
            << FormatName(outputSubtype_)
            << " stride=" << outputStride_
            << " bytes=" << outputSampleBytes_
            << "\n";
        OutputDebugStringA(oss.str().c_str());
        return true;
    }

    bool SetOutputType() {
        const GUID kPreferredSubtypes[] = {
            MFVideoFormat_NV12,
            MFVideoFormat_RGB32,
        };

        for (const GUID& preferredSubtype : kPreferredSubtypes) {
            for (DWORD index = 0;; ++index) {
                ComPtr<IMFMediaType> availableType;
                HRESULT hr = decoder_->GetOutputAvailableType(
                    0,
                    index,
                    &availableType);
                if (hr == MF_E_NO_MORE_TYPES) {
                    break;
                }
                if (FAILED(hr) || !availableType) {
                    continue;
                }

                GUID subtype{};
                if (FAILED(availableType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
                    continue;
                }
                if (subtype == preferredSubtype &&
                    TrySetOutputSubtype(preferredSubtype)) {
                    return true;
                }
            }
        }

        if (TrySetOutputSubtype(MFVideoFormat_NV12)) {
            return true;
        }
        if (TrySetOutputSubtype(MFVideoFormat_RGB32)) {
            return true;
        }

        OutputDebugStringA(
            "[NetworkVideoReceiver] H.264 decoder output type negotiation failed.\n");
        return false;
    }

    bool ConvertRgb32ToRgba(
        const uint8_t* src,
        size_t bytes,
        DecodedVideoFrame& outFrame) const {
        const size_t rowBytes = static_cast<size_t>(width_) * 4u;
        const size_t requiredBytes =
            static_cast<size_t>(std::abs(outputStride_)) *
            static_cast<size_t>(height_);
        if (!src || outputStride_ <= 0 || bytes < requiredBytes) {
            return false;
        }

        outFrame.format = DecodedVideoFrameFormat::Rgba8;
        outFrame.rgba.resize(
            static_cast<size_t>(width_) *
            static_cast<size_t>(height_) *
            4u);
        for (uint32_t y = 0; y < height_; ++y) {
            const uint8_t* row =
                src + static_cast<size_t>(y) * static_cast<size_t>(outputStride_);
            uint8_t* dst =
                outFrame.rgba.data() + static_cast<size_t>(y) * rowBytes;
            for (uint32_t x = 0; x < width_; ++x) {
                const size_t i = static_cast<size_t>(x) * 4u;
                dst[i + 0] = row[i + 2];
                dst[i + 1] = row[i + 1];
                dst[i + 2] = row[i + 0];
                dst[i + 3] = row[i + 3];
            }
        }
        return true;
    }

    bool CopyNv12Planes(
        const uint8_t* src,
        size_t bytes,
        DecodedVideoFrame& outFrame) const {
        if (!src || outputStride_ <= 0 || width_ == 0 || height_ == 0) {
            return false;
        }

        const size_t stride = static_cast<size_t>(outputStride_);
        const size_t yPlaneBytes = stride * static_cast<size_t>(height_);
        const size_t uvPlaneBytes = stride * static_cast<size_t>(height_ / 2u);
        if (bytes < yPlaneBytes + uvPlaneBytes || stride < width_) {
            return false;
        }

        const uint8_t* yPlane = src;
        const uint8_t* uvPlane = src + yPlaneBytes;

        outFrame.format = DecodedVideoFrameFormat::Nv12;
        outFrame.nv12YPitch = width_;
        outFrame.nv12UVPitch = width_;
        outFrame.nv12Y.resize(
            static_cast<size_t>(width_) * static_cast<size_t>(height_));
        outFrame.nv12UV.resize(
            static_cast<size_t>(width_) * static_cast<size_t>(height_ / 2u));

        for (uint32_t y = 0; y < height_; ++y) {
            std::memcpy(
                outFrame.nv12Y.data() + static_cast<size_t>(y) * width_,
                yPlane + static_cast<size_t>(y) * stride,
                width_);
        }
        for (uint32_t y = 0; y < height_ / 2u; ++y) {
            std::memcpy(
                outFrame.nv12UV.data() + static_cast<size_t>(y) * width_,
                uvPlane + static_cast<size_t>(y) * stride,
                width_);
        }
        return true;
    }

    H264DecodeStatus DrainOne(DecodedVideoFrame& outFrame) {
        outFrame.rgba.clear();
        outFrame.nv12Y.clear();
        outFrame.nv12UV.clear();
        outFrame.nv12YPitch = 0;
        outFrame.nv12UVPitch = 0;

        MFT_OUTPUT_STREAM_INFO info{};
        HRESULT hr = decoder_->GetOutputStreamInfo(0, &info);
        if (FAILED(hr)) {
            return H264DecodeStatus::Failed;
        }

        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
            hr = MFCreateSample(&sample);
            if (FAILED(hr)) {
                return H264DecodeStatus::Failed;
            }
            const DWORD bufferBytes =
                (std::max<DWORD>)(
                    info.cbSize,
                    static_cast<DWORD>(outputSampleBytes_));
            hr = MFCreateMemoryBuffer(bufferBytes, &buffer);
            if (FAILED(hr)) {
                return H264DecodeStatus::Failed;
            }
            sample->AddBuffer(buffer.Get());
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = sample.Get();
        DWORD status = 0;
        hr = decoder_->ProcessOutput(0, 1, &output, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (output.pSample && output.pSample != sample.Get()) {
                output.pSample->Release();
            }
            return H264DecodeStatus::NeedMoreInput;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (output.pSample && output.pSample != sample.Get()) {
                output.pSample->Release();
            }
            return SetOutputType()
                ? H264DecodeStatus::NeedMoreInput
                : H264DecodeStatus::Failed;
        }
        if (FAILED(hr) || !output.pSample) {
            if (output.pSample && output.pSample != sample.Get()) {
                output.pSample->Release();
            }
            return H264DecodeStatus::Failed;
        }

        ComPtr<IMFMediaBuffer> contiguous;
        hr = output.pSample->ConvertToContiguousBuffer(&contiguous);
        if (FAILED(hr)) {
            if (output.pSample != sample.Get()) {
                output.pSample->Release();
            }
            return H264DecodeStatus::Failed;
        }

        BYTE* src = nullptr;
        DWORD currentLength = 0;
        hr = contiguous->Lock(&src, nullptr, &currentLength);
        if (FAILED(hr)) {
            if (output.pSample != sample.Get()) {
                output.pSample->Release();
            }
            return H264DecodeStatus::Failed;
        }

        if (currentLength < outputSampleBytes_) {
            contiguous->Unlock();
            if (output.pSample != sample.Get()) {
                output.pSample->Release();
            }
            return H264DecodeStatus::Failed;
        }

        bool converted = false;
        if (outputSubtype_ == MFVideoFormat_NV12) {
            converted = CopyNv12Planes(src, currentLength, outFrame);
        }
        else if (outputSubtype_ == MFVideoFormat_RGB32) {
            converted = ConvertRgb32ToRgba(src, currentLength, outFrame);
        }

        contiguous->Unlock();
        if (output.pSample != sample.Get()) {
            output.pSample->Release();
        }
        return converted
            ? H264DecodeStatus::Decoded
            : H264DecodeStatus::Failed;
    }

private:
    ComPtr<IMFTransform> decoder_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    GUID outputSubtype_ = GUID_NULL;
    LONG outputStride_ = 0;
    size_t outputSampleBytes_ = 0;
    bool synced_ = false;
    bool discontinuity_ = true;
};

double FreshnessDropThresholdMs() {
    static const double thresholdMs = []() {
        char buffer[64]{};
        const DWORD length = GetEnvironmentVariableA(
            "RNVP_FRESHNESS_DROP_THRESHOLD_MS",
            buffer,
            static_cast<DWORD>(sizeof(buffer)));
        if (length == 0 || length >= sizeof(buffer)) {
            return kDefaultFreshnessDropThresholdMs;
        }

        char* end = nullptr;
        const double value = std::strtod(buffer, &end);
        if (end == buffer || value <= 0.0) {
            return kDefaultFreshnessDropThresholdMs;
        }

        return value;
    }();
    return thresholdMs;
}

bool DecodeJpegFrameToRgba(
    const std::vector<uint8_t>& jpeg,
    std::vector<uint8_t>& outRgba,
    uint32_t& outWidth,
    uint32_t& outHeight) {
    if (jpeg.empty()) {
        return false;
    }

    DirectX::TexMetadata metadata{};
    DirectX::ScratchImage decoded;
    HRESULT hr = DirectX::LoadFromWICMemory(
        jpeg.data(),
        jpeg.size(),
        DirectX::WIC_FLAGS_FORCE_RGB,
        &metadata,
        decoded);

    if (FAILED(hr)) {
        static uint32_t decodeFailLogCount = 0;
        if (decodeFailLogCount < 10) {
            OutputDebugStringA("[NetworkVideoReceiver] JPEG decode failed.\n");
            decodeFailLogCount++;
        }
        return false;
    }

    const DirectX::Image* image = decoded.GetImage(0, 0, 0);
    if (image == nullptr || image->pixels == nullptr ||
        image->width == 0 || image->height == 0) {
        return false;
    }

    DirectX::ScratchImage converted;
    if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        hr = DirectX::Convert(
            *image,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DirectX::TEX_FILTER_DEFAULT,
            0.0f,
            converted);

        if (FAILED(hr)) {
            return false;
        }

        image = converted.GetImage(0, 0, 0);
        if (image == nullptr || image->pixels == nullptr) {
            return false;
        }
    }

    outWidth = static_cast<uint32_t>(image->width);
    outHeight = static_cast<uint32_t>(image->height);
    const size_t rowBytes = static_cast<size_t>(outWidth) * 4u;
    outRgba.resize(rowBytes * static_cast<size_t>(outHeight));

    for (uint32_t y = 0; y < outHeight; ++y) {
        std::memcpy(
            outRgba.data() + static_cast<size_t>(y) * rowBytes,
            image->pixels + static_cast<size_t>(y) * image->rowPitch,
            rowBytes);
    }

    return true;
}

double ElapsedMs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

double AgeMs(uint64_t nowUs, uint64_t thenUs) {
    if (thenUs == 0 || nowUs < thenUs) {
        return 0.0;
    }

    return static_cast<double>(nowUs - thenUs) / 1000.0;
}

uint64_t NowMicroseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool DecodeRawFrameToRgba(
    const CompletedFrame& frame,
    DecodedVideoFrame& outFrame) {
    RawFramePayloadHeader rawHeader{};
    if (!DecodeRawFramePayloadHeader(
            frame.data.data(),
            frame.data.size(),
            rawHeader)) {
        return false;
    }

    const size_t requiredBytes =
        static_cast<size_t>(rawHeader.width) *
        static_cast<size_t>(rawHeader.height) *
        4u;
    if (rawHeader.payloadBytes < requiredBytes ||
        kRawFramePayloadHeaderSize + requiredBytes > frame.data.size()) {
        return false;
    }

    const uint8_t* rgba =
        frame.data.data() + kRawFramePayloadHeaderSize;
    outFrame.frameId = frame.frameId;
    outFrame.streamId = frame.streamId;
    outFrame.width = rawHeader.width;
    outFrame.height = rawHeader.height;
    outFrame.sendTimeUs = frame.sendTimeUs;
    outFrame.receiveTimeUs = frame.receiveTimeUs;
    outFrame.decodedTimeUs = NowMicroseconds();
    outFrame.rgba.assign(rgba, rgba + requiredBytes);
    return true;
}

double FrameFreshnessAgeMs(uint64_t nowUs, uint64_t sendTimeUs, uint64_t receiveTimeUs) {
    if (sendTimeUs != 0 && nowUs >= sendTimeUs) {
        return AgeMs(nowUs, sendTimeUs);
    }

    return AgeMs(nowUs, receiveTimeUs);
}

double FrameFreshnessAgeMs(uint64_t nowUs, const CompletedFrame& frame) {
    return FrameFreshnessAgeMs(nowUs, frame.sendTimeUs, frame.receiveTimeUs);
}

double FrameFreshnessAgeMs(uint64_t nowUs, const DecodedVideoFrame& frame) {
    return FrameFreshnessAgeMs(nowUs, frame.sendTimeUs, frame.receiveTimeUs);
}

bool HasDecodedPayload(const DecodedVideoFrame& frame) {
    if (frame.format == DecodedVideoFrameFormat::Nv12) {
        return !frame.nv12Y.empty() &&
            !frame.nv12UV.empty() &&
            frame.nv12YPitch >= frame.width &&
            frame.nv12UVPitch >= frame.width;
    }

    return !frame.rgba.empty();
}

bool IsStaleFrame(double freshnessAgeMs) {
    return freshnessAgeMs > FreshnessDropThresholdMs();
}

} // namespace

NetworkVideoReceiver::~NetworkVideoReceiver() {
    Stop();
}

bool NetworkVideoReceiver::Start(
    UdpReceiver* receiver,
    std::function<bool()> enabledProvider) {
    Stop();

    if (receiver == nullptr) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        receiver_ = receiver;
        enabledProvider_ = std::move(enabledProvider);
        latestDecodedFrame_ = {};
        hasLatestDecodedFrame_ = false;
        stats_ = {};
        stats_.freshnessDropThresholdMs = FreshnessDropThresholdMs();
        hasJpegDecodeMs_ = false;
        hasInputFrameAgeMs_ = false;
        latestDecodedFrameTimeUs_ = 0;
        lastFpsUpdateTimeUs_ = NowMicroseconds();
        decodedFramesAtLastFpsUpdate_ = 0;
    }

    running_.store(true);
    workerThread_ = std::thread(&NetworkVideoReceiver::DecodeLoop, this);
    return true;
}

void NetworkVideoReceiver::Stop() {
    running_.store(false);
    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    receiver_ = nullptr;
    enabledProvider_ = {};
    latestDecodedFrame_ = {};
    hasLatestDecodedFrame_ = false;
    latestDecodedFrameTimeUs_ = 0;
}

bool NetworkVideoReceiver::IsRunning() const {
    return running_.load();
}

bool NetworkVideoReceiver::TryGetLatestFrame(DecodedVideoFrame& outFrame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasLatestDecodedFrame_) {
        return false;
    }

    if (IsStaleFrame(FrameFreshnessAgeMs(
            NowMicroseconds(),
            latestDecodedFrame_))) {
        latestDecodedFrame_ = {};
        hasLatestDecodedFrame_ = false;
        latestDecodedFrameTimeUs_ = 0;
        stats_.freshnessDroppedFrames++;
        RecordDropLocked("stale-before-upload", true);
        return false;
    }

    outFrame = std::move(latestDecodedFrame_);
    latestDecodedFrame_ = {};
    hasLatestDecodedFrame_ = false;
    latestDecodedFrameTimeUs_ = 0;
    return true;
}

NetworkVideoReceiverStats NetworkVideoReceiver::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    NetworkVideoReceiverStats stats = stats_;
    stats.latestDecodedFrameAgeMs =
        hasLatestDecodedFrame_
        ? AgeMs(NowMicroseconds(), latestDecodedFrameTimeUs_)
        : 0.0;
    stats.freshnessDropThresholdMs = FreshnessDropThresholdMs();
    return stats;
}

void NetworkVideoReceiver::DecodeLoop() {
    H264CpuDecoder h264Decoder;

    while (running_.load()) {
        UdpReceiver* receiver = nullptr;
        bool enabled = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            receiver = receiver_;
            if (enabledProvider_) {
                enabled = enabledProvider_();
            }
        }

        if (receiver == nullptr || !enabled) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (hasLatestDecodedFrame_) {
                    RecordDropLocked("receiver-disabled", true);
                }
                hasLatestDecodedFrame_ = false;
                latestDecodedFrameTimeUs_ = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        CompletedFrame frame{};
        if (!receiver->TryPopFrame(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const uint64_t inputCheckUs = NowMicroseconds();
        const double inputFrameAgeMs =
            FrameFreshnessAgeMs(inputCheckUs, frame);
        UpdateInputFrameAge(inputFrameAgeMs);
        if (IsStaleFrame(inputFrameAgeMs)) {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.freshnessDroppedFrames++;
            RecordDropLocked("stale-before-decode", true);
            continue;
        }

        if (frame.codecType == CodecType::MJPEG) {
            std::vector<uint8_t> rgba;
            uint32_t width = 0;
            uint32_t height = 0;
            const auto decodeStart = std::chrono::steady_clock::now();
            if (!DecodeJpegFrameToRgba(frame.data, rgba, width, height)) {
                const double decodeMs = ElapsedMs(decodeStart);
                std::lock_guard<std::mutex> lock(mutex_);
                UpdateDecodeMs(decodeMs);
                stats_.decodeFailures++;
                RecordDropLocked("decode-failure", false);
                continue;
            }

            const double decodeMs = ElapsedMs(decodeStart);
            DecodedVideoFrame decodedFrame{};
            decodedFrame.frameId = frame.frameId;
            decodedFrame.streamId = frame.streamId;
            decodedFrame.width = width;
            decodedFrame.height = height;
            decodedFrame.sendTimeUs = frame.sendTimeUs;
            decodedFrame.receiveTimeUs = frame.receiveTimeUs;
            decodedFrame.decodedTimeUs = NowMicroseconds();
            decodedFrame.rgba = std::move(rgba);

            if (IsStaleFrame(FrameFreshnessAgeMs(
                    decodedFrame.decodedTimeUs,
                    decodedFrame))) {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.freshnessDroppedFrames++;
                RecordDropLocked("stale-after-decode", true);
                continue;
            }

            StoreDecodedFrame(std::move(decodedFrame), decodeMs);
            receiver->NotifyDecodeFrame();
            continue;
        }

        if (frame.codecType == CodecType::H264) {
            H264AccessUnitPayloadHeader auHeader{};
            if (!DecodeH264AccessUnitPayloadHeader(
                    frame.data.data(),
                    frame.data.size(),
                    auHeader)) {
                TraceH264Receive(
                    frame.frameId,
                    0,
                    0,
                    false,
                    h264Decoder.HasSync(),
                    h264Decoder.HasSync(),
                    "HeaderFailure",
                    0,
                    0,
                    frame.data.size(),
                    "payload-header-failure");
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.decodeFailures++;
                RecordH264AuInvalidLocked("payload-header-failure");
                RecordDropLocked("h264-payload-header-failure", false);
                receiver->RequestKeyFrame(frame.frameId);
                continue;
            }

            const H264AccessUnitValidation auValidation =
                ValidateH264AccessUnit(frame, auHeader);
            if (!auValidation.valid) {
                TraceH264Receive(
                    frame.frameId,
                    auHeader.frameId,
                    auHeader.flags,
                    false,
                    h264Decoder.HasSync(),
                    h264Decoder.HasSync(),
                    "AuInvalid",
                    auHeader.width,
                    auHeader.height,
                    auHeader.accessUnitBytes,
                    auValidation.reason.c_str());
                h264Decoder.MarkReferenceBroken();
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.decodeFailures++;
                RecordH264AuInvalidLocked(auValidation.reason.c_str());
                RecordDropLocked("h264-au-invalid", false);
                receiver->RequestKeyFrame(frame.frameId);
                continue;
            }

            const uint8_t* accessUnit =
                frame.data.data() + auHeader.headerBytes;
            const size_t accessUnitBytes = auHeader.accessUnitBytes;
            const bool decoderSync = auValidation.decoderSync;
            const bool decoderWasSyncedBeforeInit = h264Decoder.HasSync();

            if (!h264Decoder.IsInitializedFor(
                    auHeader.width,
                    auHeader.height)) {
                if (!decoderSync ||
                    !h264Decoder.Initialize(
                        auHeader.width,
                        auHeader.height)) {
                    TraceH264Receive(
                        frame.frameId,
                        auHeader.frameId,
                        auHeader.flags,
                        decoderSync,
                        decoderWasSyncedBeforeInit,
                        h264Decoder.HasSync(),
                        !decoderSync ? "InitWaitIdr" : "InitFailed",
                        auHeader.width,
                        auHeader.height,
                        accessUnitBytes,
                        !decoderSync ? "init-wait-idr" : "init-failed");
                    std::lock_guard<std::mutex> lock(mutex_);
                    stats_.decodeQueueDroppedFrames++;
                    RecordDropLocked("h264-waiting-for-idr", true);
                    receiver->RequestKeyFrame(frame.frameId);
                    continue;
                }
            }

            DecodedVideoFrame decodedFrame{};
            decodedFrame.frameId = frame.frameId;
            decodedFrame.streamId = frame.streamId;
            decodedFrame.width = auHeader.width;
            decodedFrame.height = auHeader.height;
            decodedFrame.sendTimeUs = frame.sendTimeUs;
            decodedFrame.receiveTimeUs = frame.receiveTimeUs;
            const auto decodeStart = std::chrono::steady_clock::now();
            const bool decoderWasSynced = h264Decoder.HasSync();
            const H264DecodeStatus decodeStatus = h264Decoder.Decode(
                accessUnit,
                accessUnitBytes,
                auHeader.ptsUs,
                decoderSync,
                decodedFrame);
            const bool decoderHasSyncAfterDecode = h264Decoder.HasSync();

            if (decodeStatus == H264DecodeStatus::NeedMoreInput) {
                std::lock_guard<std::mutex> lock(mutex_);
                const bool shouldRequestKeyFrame =
                    !decoderSync &&
                    !decoderWasSynced &&
                    !decoderHasSyncAfterDecode;
                const bool initialSyncBuffered =
                    decoderSync &&
                    !decoderWasSynced &&
                    decoderHasSyncAfterDecode;
                TraceH264Receive(
                    frame.frameId,
                    auHeader.frameId,
                    auHeader.flags,
                    decoderSync,
                    decoderWasSynced,
                    decoderHasSyncAfterDecode,
                    ToString(decodeStatus),
                    auHeader.width,
                    auHeader.height,
                    accessUnitBytes,
                    initialSyncBuffered
                        ? "initial-sync-buffering"
                        : shouldRequestKeyFrame
                        ? "waiting-for-idr"
                        : "decoder-buffering");
                if (!initialSyncBuffered) {
                    RecordDropLocked(
                        shouldRequestKeyFrame
                            ? "h264-waiting-for-idr"
                            : "h264-decoder-buffering",
                        true);
                }
                if (shouldRequestKeyFrame) {
                    receiver->RequestKeyFrame(frame.frameId);
                }
                continue;
            }

            const double decodeMs = ElapsedMs(decodeStart);
            if (decodeStatus != H264DecodeStatus::Decoded ||
                !HasDecodedPayload(decodedFrame)) {
                TraceH264Receive(
                    frame.frameId,
                    auHeader.frameId,
                    auHeader.flags,
                    decoderSync,
                    decoderWasSynced,
                    decoderHasSyncAfterDecode,
                    !HasDecodedPayload(decodedFrame) ? "DecodedEmpty" : ToString(decodeStatus),
                    auHeader.width,
                    auHeader.height,
                    accessUnitBytes,
                    "decode-failure");
                std::lock_guard<std::mutex> lock(mutex_);
                UpdateDecodeMs(decodeMs);
                stats_.decodeFailures++;
                RecordDropLocked("h264-decode-failure", false);
                receiver->RequestKeyFrame(frame.frameId);
                continue;
            }

            TraceH264Receive(
                frame.frameId,
                auHeader.frameId,
                auHeader.flags,
                decoderSync,
                decoderWasSynced,
                decoderHasSyncAfterDecode,
                ToString(decodeStatus),
                auHeader.width,
                auHeader.height,
                accessUnitBytes,
                "decoded");

            decodedFrame.decodedTimeUs = NowMicroseconds();

            if (IsStaleFrame(FrameFreshnessAgeMs(
                    decodedFrame.decodedTimeUs,
                    decodedFrame))) {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.freshnessDroppedFrames++;
                RecordDropLocked("stale-after-h264-decode", true);
                receiver->RequestKeyFrame(frame.frameId);
                continue;
            }

            StoreDecodedFrame(std::move(decodedFrame), decodeMs);
            receiver->NotifyDecodeFrame();
            continue;
        }

        if (frame.codecType == CodecType::Raw) {
            DecodedVideoFrame decodedFrame{};
            if (!DecodeRawFrameToRgba(frame, decodedFrame)) {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.decodeFailures++;
                RecordDropLocked("raw-decode-failure", false);
                continue;
            }

            if (IsStaleFrame(FrameFreshnessAgeMs(
                    decodedFrame.decodedTimeUs,
                    decodedFrame))) {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.freshnessDroppedFrames++;
                RecordDropLocked("stale-after-raw-decode", true);
                continue;
            }

            StoreDecodedFrame(std::move(decodedFrame), 0.0);
            receiver->NotifyDecodeFrame();
        }
    }
}

void NetworkVideoReceiver::StoreDecodedFrame(
    DecodedVideoFrame frame,
    double jpegDecodeMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    UpdateDecodeMs(jpegDecodeMs);
    if (hasLatestDecodedFrame_) {
        stats_.overwrittenFrames++;
        stats_.decodeRenderOverwriteFrames++;
        RecordDropLocked("render-overwrite", true);
    }

    latestDecodedFrame_ = std::move(frame);
    hasLatestDecodedFrame_ = true;
    latestDecodedFrameTimeUs_ = NowMicroseconds();
    stats_.decodedFrames++;
    UpdateDecodeWorkerFpsLocked(latestDecodedFrameTimeUs_);
}

void NetworkVideoReceiver::UpdateDecodeMs(double sampleMs) {
    constexpr double kAlpha = 0.20;
    if (!hasJpegDecodeMs_) {
        stats_.jpegDecodeMs = sampleMs;
        hasJpegDecodeMs_ = true;
        return;
    }

    stats_.jpegDecodeMs =
        stats_.jpegDecodeMs * (1.0 - kAlpha) + sampleMs * kAlpha;
}

void NetworkVideoReceiver::UpdateInputFrameAge(double sampleMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr double kAlpha = 0.20;
    if (!hasInputFrameAgeMs_) {
        stats_.decodeInputFrameAgeMs = sampleMs;
        hasInputFrameAgeMs_ = true;
        return;
    }

    stats_.decodeInputFrameAgeMs =
        stats_.decodeInputFrameAgeMs * (1.0 - kAlpha) + sampleMs * kAlpha;
}

void NetworkVideoReceiver::UpdateDecodeWorkerFpsLocked(uint64_t nowUs) {
    if (lastFpsUpdateTimeUs_ == 0) {
        lastFpsUpdateTimeUs_ = nowUs;
        decodedFramesAtLastFpsUpdate_ = stats_.decodedFrames;
        return;
    }

    const uint64_t elapsedUs = nowUs - lastFpsUpdateTimeUs_;
    if (elapsedUs < 500000) {
        return;
    }

    const uint64_t frameDelta =
        stats_.decodedFrames - decodedFramesAtLastFpsUpdate_;
    const double elapsedSec = static_cast<double>(elapsedUs) / 1000000.0;
    if (elapsedSec > 0.0) {
        stats_.decodeWorkerFps =
            static_cast<double>(frameDelta) / elapsedSec;
    }

    decodedFramesAtLastFpsUpdate_ = stats_.decodedFrames;
    lastFpsUpdateTimeUs_ = nowUs;
}

void NetworkVideoReceiver::RecordDropLocked(const char* reason, bool queueDrop) {
    if (queueDrop) {
        stats_.decodeQueueDroppedFrames++;
    }
    stats_.lastDropReason = reason != nullptr ? reason : "unknown";
}

void NetworkVideoReceiver::RecordH264AuInvalidLocked(const char* reason) {
    const std::string reasonText =
        reason != nullptr ? reason : "unknown";

    stats_.h264AuInvalidFrames++;
    stats_.h264AuLastInvalidReason = reasonText;

    if (reasonText == "crc-mismatch") {
        stats_.h264AuCrcMismatches++;
    }
    else if (reasonText == "payload-size-mismatch") {
        stats_.h264AuPayloadSizeMismatches++;
    }
    else if (reasonText == "nal-count-mismatch") {
        stats_.h264AuNalCountMismatches++;
    }
    else if (reasonText == "no-annexb-nals") {
        stats_.h264AuNoAnnexBNals++;
    }
    else if (reasonText == "idr-flag-mismatch") {
        stats_.h264AuIdrFlagMismatches++;
    }
    else if (reasonText == "sps-pps-flag-mismatch") {
        stats_.h264AuSpsPpsFlagMismatches++;
    }
    else if (reasonText == "sync-without-idr") {
        stats_.h264AuSyncWithoutIdr++;
    }
    else if (reasonText == "idr-without-sps-pps") {
        stats_.h264AuIdrWithoutSpsPps++;
    }
    else if (reasonText == "forbidden-zero-bit") {
        stats_.h264AuForbiddenZeroBit++;
    }
}

uint64_t NetworkVideoReceiver::NowMicroseconds() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()).count());
}

} // namespace net
