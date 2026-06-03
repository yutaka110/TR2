#include "NetworkVideoReceiver.h"

#include "PacketProtocol.h"
#include "UdpReceiver.h"
#include "../externals/DirectXTex/DirectXTex.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace net {
namespace {

constexpr double kDefaultFreshnessDropThresholdMs = 120.0;

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

uint64_t NetworkVideoReceiver::NowMicroseconds() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()).count());
}

} // namespace net
