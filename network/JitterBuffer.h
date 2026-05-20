#pragma once

#include "FrameReassembler.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace net {

    struct JitterBufferResult {
        uint32_t bufferedFrames = 0;
        uint32_t droppedFrames = 0;
        uint32_t targetDelayMs = 0;
    };

    // ============================================================
    // JitterBuffer
    // ------------------------------------------------------------
    // FrameReassemblerで完成したフレームをすぐ表示せず、
    // 一定時間だけ保持してからframeId順に取り出す。
    //
    // 目的:
    // - UDPの到着揺れを吸収する
    // - 少し遅れて来たフレームを並び替える
    // - 古すぎるフレームを捨てる
    // - 表示側に安定したフレーム列を渡す
    // ============================================================
    class JitterBuffer {
    public:
        JitterBuffer();

        JitterBuffer(
            uint32_t targetDelayMs,
            uint32_t maxBufferedFrames
        );

        void SetTargetDelayMs(uint32_t delayMs);
        uint32_t GetTargetDelayMs() const;

        void SetMaxBufferedFrames(uint32_t maxFrames);
        uint32_t GetMaxBufferedFrames() const;

        JitterBufferResult PushFrame(CompletedFrame&& frame);

        bool TryPopReadyFrame(
            uint64_t nowUs,
            CompletedFrame& outFrame
        );

        uint32_t DropExpiredFrames(
            uint64_t nowUs,
            uint64_t maxDisplayLatencyUs
        );

        void Clear();

        uint32_t GetBufferedFrameCount() const;

    private:
        uint64_t MakeFrameKey(
            uint32_t streamId,
            uint32_t frameId
        ) const;

        void TrimOverflowLocked(JitterBufferResult& result);

        bool IsOlderThanLastReleasedLocked(
            const CompletedFrame& frame
        ) const;

        bool IsPastDisplayDeadlineLocked(
            const CompletedFrame& frame,
            uint64_t nowUs,
            uint64_t maxDisplayLatencyUs
        ) const;

    private:
        mutable std::mutex mutex_;

        std::map<uint64_t, CompletedFrame> frames_;

        uint32_t targetDelayMs_ = 30;
        uint32_t maxBufferedFrames_ = 8;

        bool hasLastReleasedFrame_ = false;
        uint32_t lastReleasedStreamId_ = 0;
        uint32_t lastReleasedFrameId_ = 0;
    };

} // namespace net
