#include "UdpReceiver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace net {
namespace {

    constexpr uint64_t kDefaultFreshnessDropThresholdUs = 120000;
    constexpr uint64_t kRecoveryFreshnessSlackUs = 25000;
    constexpr uint64_t kMinimumRecoveryExpireUs = 40000;

    uint64_t FreshnessDropThresholdUs() {
        static const uint64_t thresholdUs = []() {
            char text[64]{};
            const DWORD length = GetEnvironmentVariableA(
                "RNVP_FRESHNESS_DROP_THRESHOLD_MS",
                text,
                static_cast<DWORD>(sizeof(text)));
            if (length == 0 || length >= sizeof(text)) {
                return kDefaultFreshnessDropThresholdUs;
            }

            char* end = nullptr;
            const double valueMs = strtod(text, &end);
            if (end == text || valueMs <= 0.0) {
                return kDefaultFreshnessDropThresholdUs;
            }

            return static_cast<uint64_t>(valueMs * 1000.0);
        }();
        return thresholdUs;
    }

    uint64_t NackRecoveryExpireUs() {
        const uint64_t thresholdUs = FreshnessDropThresholdUs();
        if (thresholdUs <= kRecoveryFreshnessSlackUs + kMinimumRecoveryExpireUs) {
            return (std::max)(kMinimumRecoveryExpireUs, thresholdUs / 2);
        }

        return thresholdUs - kRecoveryFreshnessSlackUs;
    }

    uint64_t NackInitialDeadlineUs(uint64_t defaultDeadlineUs) {
        const uint64_t recoveryExpireUs = NackRecoveryExpireUs();
        if (recoveryExpireUs <= kRecoveryFreshnessSlackUs) {
            return recoveryExpireUs / 2;
        }

        return (std::min)(
            defaultDeadlineUs,
            recoveryExpireUs - kRecoveryFreshnessSlackUs);
    }

} // namespace

    UdpReceiver::UdpReceiver()
        : reassembler_(&stats_)
        , jitterBuffer_(30, 8) {
    }

    UdpReceiver::~UdpReceiver() {
        Stop();
    }

    bool UdpReceiver::Start(uint16_t listenPort) {
        if (running_) {
            return true;
        }

        WSADATA wsaData{};
        int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (wsaResult != 0) {
            std::cerr << "[UdpReceiver] WSAStartup failed: " << wsaResult << "\n";
            return false;
        }

        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) {
            std::cerr << "[UdpReceiver] socket failed: " << WSAGetLastError() << "\n";
            WSACleanup();
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(listenPort);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "[UdpReceiver] bind failed: " << WSAGetLastError() << "\n";
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            WSACleanup();
            return false;
        }

        const DWORD receiveTimeoutMs = 10;
        setsockopt(
            socket_,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&receiveTimeoutMs),
            sizeof(receiveTimeoutMs)
        );

        running_ = true;
        receiveThread_ = std::thread(&UdpReceiver::ReceiveLoop, this);

        return true;
    }

    void UdpReceiver::Stop() {
        if (!running_ && socket_ == INVALID_SOCKET) {
            return;
        }

        running_ = false;

        if (socket_ != INVALID_SOCKET) {
            shutdown(socket_, SD_BOTH);
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }

        if (receiveThread_.joinable()) {
            receiveThread_.join();
        }

        WSACleanup();
    }

    bool UdpReceiver::IsRunning() const {
        return running_;
    }

    bool UdpReceiver::TryPopFrame(CompletedFrame& outFrame) {
        DrainReadyJitterBuffer(NowMicroseconds());

        std::lock_guard<std::mutex> lock(frameQueueMutex_);

        uint32_t droppedByDeadline = 0;
        while (!completedFrames_.empty() &&
            IsFramePastReceiverSafetyDeadline(
                completedFrames_.front(),
                NowMicroseconds())) {
            completedFrames_.pop_front();
            droppedByDeadline++;
        }

        if (droppedByDeadline > 0) {
            stats_.OnDeadlineDroppedFrames(droppedByDeadline);
        }

        if (completedFrames_.empty()) {
            return false;
        }

        outFrame = std::move(completedFrames_.front());
        completedFrames_.pop_front();

        return true;
    }

    NetworkStatsSnapshot UdpReceiver::GetStats() const {
        return stats_.GetSnapshot();
    }

    void UdpReceiver::ResetStats() {
        stats_.Reset();
        reassembler_.Clear();
        jitterBuffer_.Clear();
        hasLastRnvpDataAddr_ = false;
        lastRnvpDataAddr_ = sockaddr_in{};
        consecutiveIncompleteFrames_ = 0;
        lastKeyFrameRequestUs_ = 0;
        pendingTransportFeedback_.clear();
        hasLastTransportFeedbackSequence_ = false;
        lastTransportFeedbackSequence_ = 0;
        transportFeedbackSequence_ = 1;
        lastTransportFeedbackSendUs_ = 0;

        std::lock_guard<std::mutex> lock(frameQueueMutex_);
        while (!completedFrames_.empty()) {
            completedFrames_.pop_front();
        }
    }

    void UdpReceiver::SetJitterBufferTargetDelayMs(uint32_t delayMs) {
        jitterBuffer_.SetTargetDelayMs(delayMs);

        stats_.OnJitterBufferUpdated(
            jitterBuffer_.GetBufferedFrameCount(),
            jitterBuffer_.GetTargetDelayMs()
        );
    }

    uint32_t UdpReceiver::GetJitterBufferTargetDelayMs() const {
        return jitterBuffer_.GetTargetDelayMs();
    }

    void UdpReceiver::SetJitterBufferAutoModeEnabled(bool enabled) {
        jitterBufferAutoModeEnabled_.store(enabled);

        if (!enabled) {
            stats_.OnJitterBufferAutoModeUpdated(
                false,
                jitterBuffer_.GetTargetDelayMs()
            );
            return;
        }

        lastJitterAutoUpdateUs_.store(0);

        stats_.OnJitterBufferAutoModeUpdated(
            true,
            jitterBuffer_.GetTargetDelayMs()
        );
    }

    bool UdpReceiver::IsJitterBufferAutoModeEnabled() const {
        return jitterBufferAutoModeEnabled_.load();
    }

    void UdpReceiver::NotifyDecodeFrame() {
        stats_.OnDecodeFrame();
    }

    void UdpReceiver::NotifyDisplayFrame() {
        stats_.OnDisplayFrame();
    }

    void UdpReceiver::PushCompletedFrameToJitterBuffer(
        CompletedFrame&& frame,
        uint64_t nowUs
    ) {
        UpdateJitterBufferAutoMode(nowUs);

        JitterBufferResult result =
            jitterBuffer_.PushFrame(std::move(frame));

        if (result.droppedFrames > 0) {
            stats_.OnJitterBufferDropped(result.droppedFrames);

            for (uint32_t i = 0; i < result.droppedFrames; ++i) {
                stats_.OnDroppedFrame();
            }
        }

        stats_.OnJitterBufferUpdated(
            result.bufferedFrames,
            result.targetDelayMs
        );

        const uint32_t deadlineDrops =
            jitterBuffer_.DropExpiredFrames(nowUs, kReceiverSafetyExpireUs);

        if (deadlineDrops > 0) {
            stats_.OnDeadlineDroppedFrames(deadlineDrops);
        }

        DrainReadyJitterBuffer(nowUs);
    }

    void UdpReceiver::DrainReadyJitterBuffer(uint64_t nowUs) {
        CompletedFrame readyFrame;
        uint32_t releasedFramesThisDrain = 0;

        const uint32_t deadlineDrops =
            jitterBuffer_.DropExpiredFrames(nowUs, kReceiverSafetyExpireUs);

        if (deadlineDrops > 0) {
            stats_.OnDeadlineDroppedFrames(deadlineDrops);
        }

        while (jitterBuffer_.TryPopReadyFrame(nowUs, readyFrame)) {
            stats_.OnJitterBufferReleased();

            if (IsFramePastReceiverSafetyDeadline(readyFrame, nowUs)) {
                stats_.OnDeadlineDroppedFrames(1);
                readyFrame = CompletedFrame{};
                continue;
            }

            uint32_t droppedByOutputQueue = 0;
            uint32_t queueSizeBeforeDrop = 0;
            double oldestDroppedAgeMs = 0.0;
            const double newestFrameAgeMs =
                CalculateFrameAgeMs(readyFrame, nowUs);
            const char* outputDropReason =
                releasedFramesThisDrain > 0
                ? "jitter-burst-release"
                : "renderer-lag";

            {
                std::lock_guard<std::mutex> lock(frameQueueMutex_);

                queueSizeBeforeDrop =
                    static_cast<uint32_t>(completedFrames_.size());

                while (!completedFrames_.empty()) {
                    oldestDroppedAgeMs =
                        (std::max)(
                            oldestDroppedAgeMs,
                            CalculateFrameAgeMs(completedFrames_.front(), nowUs)
                        );
                    completedFrames_.pop_front();
                    droppedByOutputQueue++;
                }

                while (completedFrames_.size() >= kMaxQueuedFrames) {
                    oldestDroppedAgeMs =
                        (std::max)(
                            oldestDroppedAgeMs,
                            CalculateFrameAgeMs(completedFrames_.front(), nowUs)
                        );
                    completedFrames_.pop_front();
                    droppedByOutputQueue++;
                }

                completedFrames_.push_back(std::move(readyFrame));
            }

            if (droppedByOutputQueue > 0) {
                stats_.OnOutputQueueDropEvent(
                    droppedByOutputQueue,
                    queueSizeBeforeDrop,
                    oldestDroppedAgeMs,
                    newestFrameAgeMs,
                    outputDropReason
                );
            }

            releasedFramesThisDrain++;

            stats_.OnJitterBufferUpdated(
                jitterBuffer_.GetBufferedFrameCount(),
                jitterBuffer_.GetTargetDelayMs()
            );

            readyFrame = CompletedFrame{};
        }
    }

    bool UdpReceiver::IsFramePastReceiverSafetyDeadline(
        const CompletedFrame& frame,
        uint64_t nowUs
    ) const {
        uint64_t baseTimeUs = frame.sendTimeUs;

        if (baseTimeUs == 0 || nowUs < baseTimeUs) {
            baseTimeUs = frame.receiveTimeUs;
        }

        return nowUs > baseTimeUs &&
            nowUs - baseTimeUs > kReceiverSafetyExpireUs;
    }

    double UdpReceiver::CalculateFrameAgeMs(
        const CompletedFrame& frame,
        uint64_t nowUs
    ) const {
        uint64_t baseTimeUs = frame.sendTimeUs;

        if (baseTimeUs == 0 || nowUs < baseTimeUs) {
            baseTimeUs = frame.receiveTimeUs;
        }

        if (nowUs <= baseTimeUs) {
            return 0.0;
        }

        return static_cast<double>(nowUs - baseTimeUs) / 1000.0;
    }

    void UdpReceiver::UpdateJitterBufferAutoMode(uint64_t nowUs) {
        if (!jitterBufferAutoModeEnabled_.load()) {
            return;
        }

        constexpr uint64_t kAutoUpdateIntervalUs = 500000; // 0.5秒ごと
        constexpr uint32_t kMinAutoDelayMs = 5;
        constexpr uint32_t kMaxAutoDelayMs = 80;
        constexpr uint32_t kMaxStepUpMs = 5;
        constexpr uint32_t kMaxStepDownMs = 3;

        const uint64_t lastUpdateUs = lastJitterAutoUpdateUs_.load();

        if (lastUpdateUs != 0 &&
            nowUs > lastUpdateUs &&
            nowUs - lastUpdateUs < kAutoUpdateIntervalUs) {
            return;
        }

        lastJitterAutoUpdateUs_.store(nowUs);

        const NetworkStatsSnapshot snapshot = stats_.GetSnapshot();

        const double currentJitterMs = snapshot.currentJitterMs;
        const double averageJitterMs = snapshot.averageJitterMs;
        const double maxJitterMs = snapshot.maxJitterMs;

        // ============================================================
        // Auto Delay Calculation
        // ------------------------------------------------------------
        // averageJitterを基準にしつつ、
        // currentJitterが跳ねた場合とmaxJitterが大きい場合は余裕を持たせる。
        //
        // 狙い:
        // - 平常時は低遅延
        // - 揺れたときだけ少しバッファを増やす
        // - 急激な変化で表示リズムを壊さない
        // ============================================================
        double calculatedDelayMs = 5.0;

        calculatedDelayMs += averageJitterMs * 3.0;

        if (currentJitterMs > averageJitterMs * 2.0) {
            calculatedDelayMs += 5.0;
        }

        if (maxJitterMs >= 25.0) {
            calculatedDelayMs = (std::max)(
                calculatedDelayMs,
                averageJitterMs * 2.0 + 15.0
                );
        }

        uint32_t targetDelayMs = static_cast<uint32_t>(
            std::ceil(calculatedDelayMs)
            );

        targetDelayMs = (std::max)(targetDelayMs, kMinAutoDelayMs);
        targetDelayMs = (std::min)(targetDelayMs, kMaxAutoDelayMs);

        const uint32_t currentDelayMs = jitterBuffer_.GetTargetDelayMs();

        uint32_t smoothedDelayMs = currentDelayMs;

        if (targetDelayMs > currentDelayMs) {
            const uint32_t diff = targetDelayMs - currentDelayMs;
            smoothedDelayMs = currentDelayMs + (std::min)(diff, kMaxStepUpMs);
        }
        else if (targetDelayMs < currentDelayMs) {
            const uint32_t diff = currentDelayMs - targetDelayMs;
            smoothedDelayMs = currentDelayMs - (std::min)(diff, kMaxStepDownMs);
        }

        jitterBuffer_.SetTargetDelayMs(smoothedDelayMs);

        stats_.OnJitterBufferUpdated(
            jitterBuffer_.GetBufferedFrameCount(),
            jitterBuffer_.GetTargetDelayMs()
        );

        stats_.OnJitterBufferAutoModeUpdated(
            true,
            smoothedDelayMs
        );
    }

    void UdpReceiver::ReceiveLoop() {
        std::vector<uint8_t> buffer(kReceiveBufferSize);

        while (running_) {
            sockaddr_in fromAddr{};
            int fromLen = sizeof(fromAddr);

            int received = recvfrom(
                socket_,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()),
                0,
                reinterpret_cast<sockaddr*>(&fromAddr),
                &fromLen
            );

            if (!running_) {
                break;
            }

            if (received <= 0) {
                const int error = WSAGetLastError();
                if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                    SendDeadlineNacks(NowMicroseconds());
                    if (hasLastRnvpDataAddr_) {
                        SendTransportFeedback(lastRnvpDataAddr_, false);
                    }
                }
                continue;
            }

            const uint64_t receiveTimeUs = NowMicroseconds();

            if (static_cast<size_t>(received) < sizeof(uint32_t)) {
                continue;
            }

            const uint32_t magic = ReadU32BE(buffer.data());

            // ========================================================
            // RNVP v1 packet
            // ========================================================
            if (magic == kRnvpMagic) {
                HandleRnvpPacket(
                    buffer.data(),
                    static_cast<size_t>(received),
                    receiveTimeUs,
                    fromAddr
                );

                SendDeadlineNacks(receiveTimeUs);
                continue;
            }

            // ========================================================
            // Legacy packet
            // ========================================================
            if (magic == kPacketMagic) {
                auto completed = reassembler_.PushPacket(
                    buffer.data(),
                    static_cast<size_t>(received),
                    receiveTimeUs
                );

                if (completed) {
                    PushCompletedFrameToJitterBuffer(
                        std::move(*completed),
                        receiveTimeUs
                    );
                }

                continue;
            }

            // Unknown packet
        }
    }

    void UdpReceiver::HandleRnvpPacket(
        const uint8_t* packetData,
        size_t packetSize,
        uint64_t receiveTimeUs,
        const sockaddr_in& fromAddr
    ) {
        RnvpHeaderV1 header;
        if (!DecodeRnvpHeaderV1(packetData, packetSize, header)) {
            return;
        }

        const PacketType packetType = static_cast<PacketType>(header.packetType);

        const uint8_t* payload = packetData + header.headerSize;
        const size_t payloadSize = header.payloadSize;

        switch (packetType) {
        case PacketType::Data:
        case PacketType::Fec: {
            lastRnvpDataAddr_ = fromAddr;
            hasLastRnvpDataAddr_ = true;
            TrackTransportFeedback(header, receiveTimeUs);

            FrameAckInfo ackInfo{};

            auto completed = reassembler_.PushPacketWithAckInfo(
                packetData,
                packetSize,
                receiveTimeUs,
                &ackInfo
            );

            // ============================================================
            // ACK policy
            // ------------------------------------------------------------
            // 以前はData packetを1つ受け取るたびにACKを返していた。
            // それだとACK数が多すぎて逆方向の通信負荷が増える。
            //
            // まずは低遅延映像向けに、
            // 「LastChunkを受け取ったときだけACKを返す」方式にする。
            // ============================================================
            const bool isLastChunk =
                packetType == PacketType::Data &&
                (header.flags & PacketFlag_LastChunk) != 0;

            if (ackInfo.valid && isLastChunk) {
                SendRnvpAck(header, ackInfo, fromAddr);

                if (ackInfo.missingChunkCount > 0) {
                    consecutiveIncompleteFrames_++;

                    const bool cooldownElapsed =
                        lastKeyFrameRequestUs_ == 0 ||
                        receiveTimeUs > lastKeyFrameRequestUs_ + kKeyFrameRequestCooldownUs;

                    const bool shouldRequestKeyFrame =
                        cooldownElapsed &&
                        (ackInfo.missingChunkCount >= 2 ||
                            consecutiveIncompleteFrames_ >= 2);

                    if (shouldRequestKeyFrame) {
                        SendRnvpControl(
                            header,
                            ControlCommand::RequestKeyFrame,
                            ackInfo.frameId,
                            fromAddr
                        );

                        lastKeyFrameRequestUs_ = receiveTimeUs;
                    }
                }
            }

            if (completed) {
                consecutiveIncompleteFrames_ = 0;

                PushCompletedFrameToJitterBuffer(
                    std::move(*completed),
                    receiveTimeUs
                );
            }

            SendTransportFeedback(fromAddr, false);

            break;
        }

        case PacketType::Ping:
            HandleRnvpPing(header, payload, payloadSize, fromAddr);
            break;

        case PacketType::Pong:
            HandleRnvpPong(header, payload, payloadSize);
            break;

        case PacketType::Ack:
            HandleRnvpAck(header, payload, payloadSize);
            break;

        case PacketType::Control:
            HandleRnvpControl(header, payload, payloadSize);
            break;

        default:
            break;
        }
    }

    void UdpReceiver::HandleRnvpPing(
        const RnvpHeaderV1& header,
        const uint8_t* payload,
        size_t payloadSize,
        const sockaddr_in& fromAddr
    ) {
        PingPayload ping{};
        if (!DecodePingPayload(payload, payloadSize, ping)) {
            return;
        }

        SendRnvpPong(header, ping, fromAddr);
    }

    void UdpReceiver::HandleRnvpPong(
        const RnvpHeaderV1& header,
        const uint8_t* payload,
        size_t payloadSize
    ) {
        (void)header;

        PongPayload pong{};
        if (!DecodePongPayload(payload, payloadSize, pong)) {
            return;
        }

        const uint64_t nowUs = NowMicroseconds();

        if (nowUs >= pong.clientTimeUs) {
            const double rttMs =
                static_cast<double>(nowUs - pong.clientTimeUs) / 1000.0;

            std::cout << "[RNVP] Pong received. RTT = "
                << rttMs << " ms\n";
        }
    }

    void UdpReceiver::HandleRnvpAck(
        const RnvpHeaderV1& header,
        const uint8_t* payload,
        size_t payloadSize
    ) {
        (void)header;

        AckPayload ack{};
        if (!DecodeAckPayload(payload, payloadSize, ack)) {
            return;
        }

        std::cout << "[RNVP] Ack received. frameId="
            << ack.frameId
            << " receivedChunks="
            << ack.receivedChunkCount
            << " missingChunks="
            << ack.missingChunkCount
            << " missingList="
            << ack.missingChunkIndices.size()
            << " latestSequence="
            << ack.latestSequence
            << "\n";
    }

    void UdpReceiver::HandleRnvpControl(
        const RnvpHeaderV1& header,
        const uint8_t* payload,
        size_t payloadSize
    ) {
        (void)header;

        ControlPayload control{};
        if (!DecodeControlPayload(payload, payloadSize, control)) {
            return;
        }

        const ControlCommand command =
            static_cast<ControlCommand>(control.command);

        switch (command) {
        case ControlCommand::SetJpegQuality:
            std::cout << "[RNVP] Control: SetJpegQuality = "
                << control.value << "\n";
            break;

        case ControlCommand::SetTargetFps:
            std::cout << "[RNVP] Control: SetTargetFps = "
                << control.value << "\n";
            break;

        case ControlCommand::SetBitrateKbps:
            std::cout << "[RNVP] Control: SetBitrateKbps = "
                << control.value << "\n";
            break;

        case ControlCommand::RequestKeyFrame:
            std::cout << "[RNVP] Control: RequestKeyFrame\n";
            break;

        case ControlCommand::ResetStats:
            std::cout << "[RNVP] Control: ResetStats\n";
            ResetStats();
            break;

        default:
            std::cout << "[RNVP] Control: Unknown command = "
                << static_cast<int>(control.command)
                << " value="
                << control.value
                << "\n";
            break;
        }
    }

    void UdpReceiver::SendRnvpPong(
        const RnvpHeaderV1& pingHeader,
        const PingPayload& pingPayload,
        const sockaddr_in& toAddr
    ) {
        if (socket_ == INVALID_SOCKET) {
            return;
        }

        PongPayload pong{};
        pong.clientTimeUs = pingPayload.clientTimeUs;
        pong.serverTimeUs = NowMicroseconds();

        constexpr size_t payloadSize = 16;

        std::vector<uint8_t> packet(kRnvpHeaderV1Size + payloadSize);

        RnvpHeaderV1 header{};
        header.magic = kRnvpMagic;
        header.version = kRnvpVersion;
        header.packetType = static_cast<uint8_t>(PacketType::Pong);
        header.headerSize = static_cast<uint16_t>(kRnvpHeaderV1Size);

        header.sequence = NextRNVPSequence();
        header.streamId = pingHeader.streamId;

        header.frameId = 0;
        header.chunkIndex = 0;
        header.chunkCount = 0;

        header.sendTimeUs = NowMicroseconds();
        header.payloadSize = static_cast<uint32_t>(payloadSize);
        header.flags = PacketFlag_None;
        header.codecType = static_cast<uint8_t>(CodecType::Unknown);

        EncodeRnvpHeaderV1(packet.data(), header);
        EncodePongPayload(packet.data() + kRnvpHeaderV1Size, pong);

        const int sent = sendto(
            socket_,
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()),
            0,
            reinterpret_cast<const sockaddr*>(&toAddr),
            sizeof(toAddr)
        );

        if (sent == SOCKET_ERROR) {
            std::cerr << "[UdpReceiver] SendRnvpPong failed: "
                << WSAGetLastError() << "\n";
        }
    }

    void UdpReceiver::SendRnvpAck(
        const RnvpHeaderV1& dataHeader,
        const FrameAckInfo& ackInfo,
        const sockaddr_in& toAddr
    ) {
        if (socket_ == INVALID_SOCKET) {
            return;
        }

        AckPayload ack{};
        ack.frameId = ackInfo.frameId;
        ack.receivedChunkCount = ackInfo.receivedChunkCount;
        ack.missingChunkCount = ackInfo.missingChunkCount;
        ack.latestSequence = ackInfo.latestSequence;
        ack.missingChunkIndices = ackInfo.missingChunkIndices;

        const size_t payloadSize = CalculateAckPayloadSize(ack);

        std::vector<uint8_t> packet(kRnvpHeaderV1Size + payloadSize);

        RnvpHeaderV1 header{};
        header.magic = kRnvpMagic;
        header.version = kRnvpVersion;
        header.packetType = static_cast<uint8_t>(PacketType::Ack);
        header.headerSize = static_cast<uint16_t>(kRnvpHeaderV1Size);

        header.sequence = NextRNVPSequence();
        header.streamId = dataHeader.streamId;

        header.frameId = ackInfo.frameId;
        header.chunkIndex = 0;
        header.chunkCount = 0;

        header.sendTimeUs = NowMicroseconds();
        header.payloadSize = static_cast<uint32_t>(payloadSize);
        header.flags = PacketFlag_None;
        header.codecType = static_cast<uint8_t>(CodecType::Unknown);

        EncodeRnvpHeaderV1(packet.data(), header);
        EncodeAckPayload(packet.data() + kRnvpHeaderV1Size, ack);

        const int sent = sendto(
            socket_,
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()),
            0,
            reinterpret_cast<const sockaddr*>(&toAddr),
            sizeof(toAddr)
        );

        if (sent == SOCKET_ERROR) {
            std::cerr << "[UdpReceiver] SendRnvpAck failed: "
                << WSAGetLastError() << "\n";
        }
    }

    void UdpReceiver::SendDeadlineNacks(uint64_t nowUs) {
        if (!hasLastRnvpDataAddr_) {
            return;
        }

        const uint64_t recoveryExpireUs = NackRecoveryExpireUs();
        const uint64_t nackDeadlineUs =
            NackInitialDeadlineUs(kFrameNackDeadlineUs);

        FrameRecoveryActions recoveryActions =
            reassembler_.CollectRecoveryActions(
                nowUs,
                nackDeadlineUs,
                kFrameNackIntervalUs,
                recoveryExpireUs,
                kFrameNackMinRecoverySlackUs,
                kMaxDeadlineNacksPerFrame
            );

        if (recoveryActions.expiredFrameCount > 0) {
            consecutiveIncompleteFrames_ +=
                recoveryActions.expiredFrameCount;

            const bool cooldownElapsed =
                lastKeyFrameRequestUs_ == 0 ||
                nowUs > lastKeyFrameRequestUs_ + kKeyFrameRequestCooldownUs;

            const bool shouldRequestKeyFrame =
                cooldownElapsed &&
                (recoveryActions.expiredAfterNackCount > 0 ||
                    recoveryActions.expiredFrameCount >= 2 ||
                    consecutiveIncompleteFrames_ >= 3);

            if (shouldRequestKeyFrame) {
                RnvpHeaderV1 syntheticHeader{};
                syntheticHeader.streamId =
                    recoveryActions.lastExpiredStreamId;
                syntheticHeader.frameId =
                    recoveryActions.lastExpiredFrameId;

                SendRnvpControl(
                    syntheticHeader,
                    ControlCommand::RequestKeyFrame,
                    recoveryActions.lastExpiredFrameId,
                    lastRnvpDataAddr_
                );

                lastKeyFrameRequestUs_ = nowUs;
            }
        }

        for (const FrameAckInfo& ackInfo : recoveryActions.nackAckInfos) {
            RnvpHeaderV1 syntheticHeader{};
            syntheticHeader.streamId = ackInfo.streamId;
            syntheticHeader.frameId = ackInfo.frameId;

            SendRnvpAck(
                syntheticHeader,
                ackInfo,
                lastRnvpDataAddr_
            );

            stats_.OnDeadlineNackSent(ackInfo.missingChunkCount);

            consecutiveIncompleteFrames_++;

            const bool cooldownElapsed =
                lastKeyFrameRequestUs_ == 0 ||
                nowUs > lastKeyFrameRequestUs_ + kKeyFrameRequestCooldownUs;

            const bool shouldRequestKeyFrame =
                cooldownElapsed &&
                (ackInfo.missingChunkCount >= 2 ||
                    consecutiveIncompleteFrames_ >= 3);

            if (shouldRequestKeyFrame) {
                SendRnvpControl(
                    syntheticHeader,
                    ControlCommand::RequestKeyFrame,
                    ackInfo.frameId,
                    lastRnvpDataAddr_
                );

                lastKeyFrameRequestUs_ = nowUs;
            }
        }
    }

    void UdpReceiver::TrackTransportFeedback(
        const RnvpHeaderV1& dataHeader,
        uint64_t receiveTimeUs
    ) {
        if (!hasLastTransportFeedbackSequence_) {
            hasLastTransportFeedbackSequence_ = true;
            lastTransportFeedbackSequence_ = dataHeader.sequence;
        }
        else {
            const uint32_t expectedNext = lastTransportFeedbackSequence_ + 1;
            if (dataHeader.sequence > expectedNext) {
                for (uint32_t missingSequence = expectedNext;
                    missingSequence < dataHeader.sequence;
                    ++missingSequence) {
                    if (pendingTransportFeedback_.size() >=
                        kMaxTransportFeedbackEntries) {
                        break;
                    }

                    PendingTransportFeedback missing{};
                    missing.sequence = missingSequence;
                    missing.received = false;
                    pendingTransportFeedback_.push_back(missing);
                }
            }

            if (dataHeader.sequence > lastTransportFeedbackSequence_) {
                lastTransportFeedbackSequence_ = dataHeader.sequence;
            }
        }

        PendingTransportFeedback received{};
        received.sequence = dataHeader.sequence;
        received.received = true;
        received.receiveTimeUs = receiveTimeUs;
        pendingTransportFeedback_.push_back(received);

        while (pendingTransportFeedback_.size() >
            kMaxTransportFeedbackEntries) {
            pendingTransportFeedback_.pop_front();
        }
    }

    void UdpReceiver::SendTransportFeedback(
        const sockaddr_in& toAddr,
        bool force
    ) {
        if (socket_ == INVALID_SOCKET ||
            pendingTransportFeedback_.empty()) {
            return;
        }

        const uint64_t nowUs = NowMicroseconds();
        const bool intervalElapsed =
            lastTransportFeedbackSendUs_ == 0 ||
            nowUs > lastTransportFeedbackSendUs_ + kTransportFeedbackIntervalUs;
        if (!force &&
            pendingTransportFeedback_.size() < kTransportFeedbackBatchSize &&
            !intervalElapsed) {
            return;
        }

        const size_t entryCount = (std::min)(
            pendingTransportFeedback_.size(),
            kMaxTransportFeedbackEntries
        );
        if (entryCount == 0) {
            return;
        }

        const uint32_t baseSequence =
            pendingTransportFeedback_.front().sequence;

        uint64_t referenceReceiveTimeUs = 0;
        for (size_t i = 0; i < entryCount; ++i) {
            const PendingTransportFeedback& pending =
                pendingTransportFeedback_[i];
            if (pending.received) {
                referenceReceiveTimeUs = pending.receiveTimeUs;
                break;
            }
        }
        if (referenceReceiveTimeUs == 0) {
            referenceReceiveTimeUs = nowUs;
        }

        TransportFeedbackPayload feedback{};
        feedback.baseSequence = baseSequence;
        feedback.feedbackSequence = transportFeedbackSequence_++;
        feedback.referenceReceiveTimeUs = referenceReceiveTimeUs;
        feedback.entries.reserve(entryCount);

        for (size_t i = 0; i < entryCount; ++i) {
            const PendingTransportFeedback& pending =
                pendingTransportFeedback_.front();

            TransportFeedbackEntry entry{};
            const uint32_t sequenceDelta =
                pending.sequence >= baseSequence
                ? pending.sequence - baseSequence
                : 0;
            entry.sequenceDelta = static_cast<uint16_t>(
                (std::min)(sequenceDelta, 0xFFFFu)
            );

            if (pending.received) {
                entry.flags = TransportFeedbackFlag_Received;
                entry.receiveDeltaUs = pending.receiveTimeUs >=
                    referenceReceiveTimeUs
                    ? static_cast<uint32_t>(
                        (std::min)(
                            pending.receiveTimeUs - referenceReceiveTimeUs,
                            static_cast<uint64_t>(
                                (std::numeric_limits<uint32_t>::max)())
                        ))
                    : 0;
            }
            else {
                entry.flags = TransportFeedbackFlag_Missing;
                entry.receiveDeltaUs = 0;
            }

            feedback.entries.push_back(entry);
            pendingTransportFeedback_.pop_front();
        }

        feedback.packetStatusCount =
            static_cast<uint16_t>(feedback.entries.size());

        const size_t payloadSize =
            CalculateTransportFeedbackPayloadSize(feedback);
        std::vector<uint8_t> packet(kRnvpHeaderV1Size + payloadSize);

        RnvpHeaderV1 header{};
        header.magic = kRnvpMagic;
        header.version = kRnvpVersion;
        header.packetType =
            static_cast<uint8_t>(PacketType::TransportFeedback);
        header.headerSize = static_cast<uint16_t>(kRnvpHeaderV1Size);
        header.sequence = NextRNVPSequence();
        header.streamId = 1;
        header.frameId = 0;
        header.chunkIndex = 0;
        header.chunkCount = 0;
        header.sendTimeUs = nowUs;
        header.payloadSize = static_cast<uint32_t>(payloadSize);
        header.flags = PacketFlag_Control;
        header.codecType = static_cast<uint8_t>(CodecType::Unknown);

        EncodeRnvpHeaderV1(packet.data(), header);
        EncodeTransportFeedbackPayload(
            packet.data() + kRnvpHeaderV1Size,
            feedback
        );

        const int sent = sendto(
            socket_,
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()),
            0,
            reinterpret_cast<const sockaddr*>(&toAddr),
            sizeof(toAddr)
        );

        if (sent == SOCKET_ERROR) {
            std::cerr << "[UdpReceiver] SendTransportFeedback failed: "
                << WSAGetLastError() << "\n";
            return;
        }

        lastTransportFeedbackSendUs_ = nowUs;
    }

    void UdpReceiver::SendRnvpControl(
        const RnvpHeaderV1& dataHeader,
        ControlCommand command,
        uint32_t value,
        const sockaddr_in& toAddr
    ) {
        if (socket_ == INVALID_SOCKET) {
            return;
        }

        ControlPayload control{};
        control.command = static_cast<uint8_t>(command);
        control.value = value;

        constexpr size_t payloadSize = 8;

        std::vector<uint8_t> packet(kRnvpHeaderV1Size + payloadSize);

        RnvpHeaderV1 header{};
        header.magic = kRnvpMagic;
        header.version = kRnvpVersion;
        header.packetType = static_cast<uint8_t>(PacketType::Control);
        header.headerSize = static_cast<uint16_t>(kRnvpHeaderV1Size);

        header.sequence = NextRNVPSequence();
        header.streamId = dataHeader.streamId;

        header.frameId = dataHeader.frameId;
        header.chunkIndex = 0;
        header.chunkCount = 0;

        header.sendTimeUs = NowMicroseconds();
        header.payloadSize = static_cast<uint32_t>(payloadSize);
        header.flags = PacketFlag_Control;
        header.codecType = static_cast<uint8_t>(CodecType::Unknown);

        EncodeRnvpHeaderV1(packet.data(), header);
        EncodeControlPayload(packet.data() + kRnvpHeaderV1Size, control);

        const int sent = sendto(
            socket_,
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()),
            0,
            reinterpret_cast<const sockaddr*>(&toAddr),
            sizeof(toAddr)
        );

        if (sent == SOCKET_ERROR) {
            std::cerr << "[UdpReceiver] SendRnvpControl failed: "
                << WSAGetLastError() << "\n";
            return;
        }

        if (command == ControlCommand::RequestKeyFrame) {
            std::cout << "[UdpReceiver] RequestKeyFrame sent. frameId="
                << value << "\n";
        }
    }

    uint64_t UdpReceiver::NowMicroseconds() const {
        using namespace std::chrono;

        return static_cast<uint64_t>(
            duration_cast<microseconds>(
                steady_clock::now().time_since_epoch()
            ).count()
            );
    }

    uint32_t UdpReceiver::NextRNVPSequence() {
        return rnvpSequence_.fetch_add(1, std::memory_order_relaxed);
    }

} // namespace net
