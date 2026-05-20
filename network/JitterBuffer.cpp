#include "JitterBuffer.h"

#include <algorithm>

namespace net {

    JitterBuffer::JitterBuffer() {
    }

    JitterBuffer::JitterBuffer(
        uint32_t targetDelayMs,
        uint32_t maxBufferedFrames
    )
        : targetDelayMs_(targetDelayMs)
        , maxBufferedFrames_(maxBufferedFrames) {
    }

    void JitterBuffer::SetTargetDelayMs(uint32_t delayMs) {
        std::lock_guard<std::mutex> lock(mutex_);

        // 低遅延映像用途なので、まずは過剰に大きくしない。
        targetDelayMs_ = (std::min)(delayMs, 200u);
    }

    uint32_t JitterBuffer::GetTargetDelayMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return targetDelayMs_;
    }

    void JitterBuffer::SetMaxBufferedFrames(uint32_t maxFrames) {
        std::lock_guard<std::mutex> lock(mutex_);

        maxBufferedFrames_ = (std::max)(maxFrames, 1u);

        JitterBufferResult dummy{};
        TrimOverflowLocked(dummy);
    }

    uint32_t JitterBuffer::GetMaxBufferedFrames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return maxBufferedFrames_;
    }

    JitterBufferResult JitterBuffer::PushFrame(CompletedFrame&& frame) {
        std::lock_guard<std::mutex> lock(mutex_);

        JitterBufferResult result{};
        result.targetDelayMs = targetDelayMs_;

        if (IsOlderThanLastReleasedLocked(frame)) {
            result.droppedFrames++;
            result.bufferedFrames = static_cast<uint32_t>(frames_.size());
            return result;
        }

        const uint64_t key = MakeFrameKey(frame.streamId, frame.frameId);

        // 同じframeIdがすでにある場合は重複として捨てる。
        if (frames_.find(key) != frames_.end()) {
            result.droppedFrames++;
            result.bufferedFrames = static_cast<uint32_t>(frames_.size());
            return result;
        }

        frames_.emplace(key, std::move(frame));

        TrimOverflowLocked(result);

        result.bufferedFrames = static_cast<uint32_t>(frames_.size());
        result.targetDelayMs = targetDelayMs_;

        return result;
    }

    bool JitterBuffer::TryPopReadyFrame(
        uint64_t nowUs,
        CompletedFrame& outFrame
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (frames_.empty()) {
            return false;
        }

        auto it = frames_.begin();
        CompletedFrame& frame = it->second;

        if (nowUs < frame.receiveTimeUs) {
            return false;
        }

        const uint64_t ageUs = nowUs - frame.receiveTimeUs;
        const uint64_t targetDelayUs =
            static_cast<uint64_t>(targetDelayMs_) * 1000ull;

        // まだジッター吸収時間に達していない。
        if (ageUs < targetDelayUs) {
            return false;
        }

        outFrame = std::move(frame);

        hasLastReleasedFrame_ = true;
        lastReleasedStreamId_ = outFrame.streamId;
        lastReleasedFrameId_ = outFrame.frameId;

        frames_.erase(it);

        return true;
    }

    uint32_t JitterBuffer::DropExpiredFrames(
        uint64_t nowUs,
        uint64_t maxDisplayLatencyUs
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        uint32_t droppedFrames = 0;

        for (auto it = frames_.begin(); it != frames_.end();) {
            if (!IsPastDisplayDeadlineLocked(it->second, nowUs, maxDisplayLatencyUs)) {
                ++it;
                continue;
            }

            it = frames_.erase(it);
            droppedFrames++;
        }

        return droppedFrames;
    }

    void JitterBuffer::Clear() {
        std::lock_guard<std::mutex> lock(mutex_);

        frames_.clear();

        hasLastReleasedFrame_ = false;
        lastReleasedStreamId_ = 0;
        lastReleasedFrameId_ = 0;
    }

    uint32_t JitterBuffer::GetBufferedFrameCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<uint32_t>(frames_.size());
    }

    uint64_t JitterBuffer::MakeFrameKey(
        uint32_t streamId,
        uint32_t frameId
    ) const {
        return (static_cast<uint64_t>(streamId) << 32) |
            static_cast<uint64_t>(frameId);
    }

    void JitterBuffer::TrimOverflowLocked(JitterBufferResult& result) {
        while (frames_.size() > maxBufferedFrames_) {
            // 一番古いframeIdを捨てる。
            // 低遅延映像では、古いフレームを残すより捨てた方がよい。
            frames_.erase(frames_.begin());
            result.droppedFrames++;
        }
    }

    bool JitterBuffer::IsOlderThanLastReleasedLocked(
        const CompletedFrame& frame
    ) const {
        if (!hasLastReleasedFrame_) {
            return false;
        }

        if (frame.streamId != lastReleasedStreamId_) {
            return false;
        }

        return frame.frameId <= lastReleasedFrameId_;
    }

    bool JitterBuffer::IsPastDisplayDeadlineLocked(
        const CompletedFrame& frame,
        uint64_t nowUs,
        uint64_t maxDisplayLatencyUs
    ) const {
        if (maxDisplayLatencyUs == 0) {
            return false;
        }

        uint64_t baseTimeUs = frame.sendTimeUs;

        if (baseTimeUs == 0 || nowUs < baseTimeUs) {
            baseTimeUs = frame.receiveTimeUs;
        }

        return nowUs > baseTimeUs &&
            nowUs - baseTimeUs > maxDisplayLatencyUs;
    }

} // namespace net
