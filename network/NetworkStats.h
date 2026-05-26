#pragma once

#include "NetworkConditionSimulator.h"

#include <cstdint>
#include <mutex>
#include <string>

namespace net {

	struct NetworkStatsSnapshot {
		// ============================================================
		// Packet / Byte
		// ============================================================
		uint64_t receivedPackets = 0;
		uint64_t receivedBytes = 0;

		// RNVP sequence欠番から推定した欠損パケット数
		uint64_t missingPackets = 0;

		uint64_t duplicatePackets = 0;
		uint64_t reorderedPackets = 0;

		// ============================================================
		// Frame
		// ============================================================
		uint64_t completedFrames = 0;
		uint64_t droppedFrames = 0;
		uint64_t deadlineDroppedFrames = 0;
		uint64_t outputQueueDroppedFrames = 0;
		uint64_t outputQueueDropEvents = 0;
		uint64_t outputQueueDropBurstEvents = 0;
		uint32_t lastOutputQueueDropFrameCount = 0;
		uint32_t lastOutputQueueDropQueueSize = 0;
		double lastOutputQueueDropOldestAgeMs = 0.0;
		double lastOutputQueueDropNewestAgeMs = 0.0;
		double maxOutputQueueDropOldestAgeMs = 0.0;
		std::string lastOutputQueueDropReason;
		uint64_t decodedFrames = 0;
		uint64_t displayedFrames = 0;

		double receiveFps = 0.0;
		double decodeFps = 0.0;
		double displayFps = 0.0;

		// ============================================================
		// Latency
		// ============================================================
		double currentLatencyMs = 0.0;
		double averageLatencyMs = 0.0;
		double maxLatencyMs = 0.0;

		// ============================================================
		// RTT
		// ============================================================
		double currentRttMs = 0.0;
		double averageRttMs = 0.0;
		double maxRttMs = 0.0;
		uint64_t rttSamples = 0;

		// ============================================================
        // Sender ACK
        // ------------------------------------------------------------
        // 送信側が受け取ったACK情報。
        // NetworkManager側のACK受信結果をUIに渡すために使う。
        // ============================================================
		uint64_t ackCount = 0;
		uint32_t lastAckFrameId = 0;
		uint32_t lastAckReceivedChunks = 0;
		uint32_t lastAckMissingChunks = 0;
		double lastAckMissingRate = 0.0;
		uint64_t ackRetransmittedFrames = 0;
		uint64_t ackRetransmittedChunks = 0;
		uint64_t ackStaleDroppedFrames = 0;
		uint64_t ackKeyFrameRequests = 0;
		bool ackKeyFramePending = false;
		uint64_t deadlineNackSentFrames = 0;
		uint64_t deadlineNackRecoveredFrames = 0;
		uint64_t deadlineNackMissingChunks = 0;

		// ============================================================
        // Adaptive Streaming
        // ------------------------------------------------------------
        // AdaptiveStreamingController の現在の制御目標。
        // UI表示用。実際のJPEG品質/FPS反映は次Stepで行う。
        // ============================================================
		bool adaptiveEnabled = false;

		int adaptiveTargetJpegQuality = 0;
		int adaptiveTargetFps = 0;
		int adaptiveTargetBitrateKbps = 0;
		int adaptiveTargetWidth = 0;
		int adaptiveTargetHeight = 0;
		uint64_t adaptiveRawFrameBytes = 0;
		uint64_t adaptiveEncodedFrameBytes = 0;
		double adaptiveCompressionRatio = 0.0;

		bool adaptiveQualityChanged = false;
		bool adaptiveFpsChanged = false;
		bool adaptiveBitrateChanged = false;
		bool adaptiveResolutionChanged = false;

		double adaptiveLastAckMissingRate = 0.0;
		double adaptiveLastPacketLossRate = 0.0;
		double adaptiveLastRttMs = 0.0;
		double adaptiveLastLatencyMs = 0.0;
		double adaptiveLastJitterMs = 0.0;
		double adaptiveLastReceiveFps = 0.0;
		double adaptiveLastDecodeFps = 0.0;
		double adaptiveLastDisplayFps = 0.0;
		double adaptiveLastQoeScore = 0.0;
		std::string adaptiveDegradationCause;

		// ============================================================
		// Jitter
		// ------------------------------------------------------------
		// 連続するフレーム到着間隔の揺れ。
		// RFC3550風の厳密計算ではなく、作品用に分かりやすい
		// 平均絶対変動として扱う。
		// ============================================================
		double currentJitterMs = 0.0;
		double averageJitterMs = 0.0;
		double maxJitterMs = 0.0;

		uint32_t jitterBufferTargetDelayMs = 0;
		uint32_t jitterBufferBufferedFrames = 0;
		uint64_t jitterBufferReleasedFrames = 0;
		uint64_t jitterBufferDroppedFrames = 0;

		bool jitterBufferAutoModeEnabled = false;
		uint32_t jitterBufferAutoCalculatedDelayMs = 0;

		// ============================================================
		// Network Condition Simulator
		// ============================================================
		NetworkCondition networkCondition{};
		NetworkSimulationStats networkSimulation{};

		// ============================================================
		// Bandwidth
		// ============================================================
		double bitrateMbps = 0.0;     // 受信ペイロード/フレームベースの推定bitrate
		double throughputMbps = 0.0;  // 実際に受信したUDP packet byteベース

		// ============================================================
		// Loss
		// ============================================================
		double packetLossRate = 0.0;
		double frameDropRate = 0.0;

		// ============================================================
		// Debug / Time
		// ============================================================
		uint32_t latestFrameId = 0;
		uint64_t lastUpdateTimeUs = 0;
	};

	class NetworkStats {
	public:
		NetworkStats();

		void Reset();

		// UDP packetを1つ受け取ったときに呼ぶ
		void OnPacketReceived(uint32_t packetBytes);

		// フレーム再構成が完了したときに呼ぶ
		void OnFrameCompleted(
			uint32_t frameId,
			uint64_t frameBytes,
			uint64_t sendTimeUs,
			uint64_t receiveTimeUs
		);

		// Ping/PongでRTTが取れたときに呼ぶ
		void OnRttSample(double rttMs);

		// 欠損や破棄を観測したときに呼ぶ
		void OnDroppedFrame();
		void OnDeadlineDroppedFrames(uint32_t droppedFrames);
		void OnOutputQueueDroppedFrames(uint32_t droppedFrames);
		void OnOutputQueueDropEvent(
			uint32_t droppedFrames,
			uint32_t queueSizeBeforeDrop,
			double oldestDroppedAgeMs,
			double newestFrameAgeMs,
			const char* reason
		);
		void OnDeadlineNackSent(uint32_t missingChunkCount);
		void OnDeadlineNackRecoveredFrame();

		// 重複packetを観測したときに呼ぶ
		void OnDuplicatePacket();

		// sequenceの逆転など、順序入れ替えを観測したときに呼ぶ
		void OnReorderedPacket();

		// デコード成功時に呼ぶ。MJPEG/H.264対応時に使う
		void OnDecodeFrame();

		// 画面表示成功時に呼ぶ。DX12表示側で使う
		void OnDisplayFrame();

		// RNVP sequence の欠番を検出したときに呼ぶ
		void OnMissingPackets(uint64_t missingCount);

		// JitterBufferの状態更新
		void OnJitterBufferUpdated(
			uint32_t bufferedFrames,
			uint32_t targetDelayMs
		);

		void OnJitterBufferReleased();

		void OnJitterBufferDropped(uint32_t droppedFrames);

		void OnJitterBufferAutoModeUpdated(
			bool enabled,
			uint32_t calculatedDelayMs
		);

		NetworkStatsSnapshot GetSnapshot() const;

	private:
		uint64_t NowMicroseconds() const;

		void UpdateReceiveFps(uint64_t nowUs);
		void UpdateDecodeFps(uint64_t nowUs);
		void UpdateDisplayFps(uint64_t nowUs);

		void UpdateThroughput(uint64_t nowUs);
		void UpdateBitrate(uint64_t nowUs);

	private:
		mutable std::mutex mutex_;
		NetworkStatsSnapshot snapshot_;

		// ============================================================
		// Internal counters
		// ============================================================
		uint64_t startTimeUs_ = 0;

		uint64_t lastFpsUpdateTimeUs_ = 0;
		uint64_t lastDecodeFpsUpdateTimeUs_ = 0;
		uint64_t lastDisplayFpsUpdateTimeUs_ = 0;

		uint64_t framesAtLastFpsUpdate_ = 0;
		uint64_t decodedFramesAtLastFpsUpdate_ = 0;
		uint64_t displayedFramesAtLastFpsUpdate_ = 0;

		uint64_t bytesAtLastThroughputUpdate_ = 0;
		uint64_t frameBytesAtLastBitrateUpdate_ = 0;

		uint64_t totalFrameBytes_ = 0;

		// latency average
		double latencySumMs_ = 0.0;

		// jitter
		bool hasPreviousFrameArrival_ = false;
		uint64_t previousFrameReceiveTimeUs_ = 0;
		double jitterSumMs_ = 0.0;
		uint64_t jitterSamples_ = 0;

		// rtt
		double rttSumMs_ = 0.0;


	};

} // namespace net
