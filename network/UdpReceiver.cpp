#include "UdpReceiver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <vector>

namespace net {

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

        if (completedFrames_.empty()) {
            return false;
        }

        outFrame = std::move(completedFrames_.front());
        completedFrames_.pop();

        return true;
    }

    NetworkStatsSnapshot UdpReceiver::GetStats() const {
        return stats_.GetSnapshot();
    }

    void UdpReceiver::ResetStats() {
        stats_.Reset();
        reassembler_.Clear();
        jitterBuffer_.Clear();

        std::lock_guard<std::mutex> lock(frameQueueMutex_);
        while (!completedFrames_.empty()) {
            completedFrames_.pop();
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

        DrainReadyJitterBuffer(nowUs);
    }

    void UdpReceiver::DrainReadyJitterBuffer(uint64_t nowUs) {
        CompletedFrame readyFrame;

        while (jitterBuffer_.TryPopReadyFrame(nowUs, readyFrame)) {
            stats_.OnJitterBufferReleased();

            uint32_t droppedByOutputQueue = 0;

            {
                std::lock_guard<std::mutex> lock(frameQueueMutex_);

                while (completedFrames_.size() >= kMaxQueuedFrames) {
                    completedFrames_.pop();
                    droppedByOutputQueue++;
                }

                completedFrames_.push(std::move(readyFrame));
            }

            if (droppedByOutputQueue > 0) {
                for (uint32_t i = 0; i < droppedByOutputQueue; ++i) {
                    stats_.OnDroppedFrame();
                }
            }

            stats_.OnJitterBufferUpdated(
                jitterBuffer_.GetBufferedFrameCount(),
                jitterBuffer_.GetTargetDelayMs()
            );

            readyFrame = CompletedFrame{};
        }
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
        case PacketType::Data: {
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
                (header.flags & PacketFlag_LastChunk) != 0;

            if (ackInfo.valid && isLastChunk) {
                SendRnvpAck(header, ackInfo, fromAddr);
            }

            if (completed) {
                PushCompletedFrameToJitterBuffer(
                    std::move(*completed),
                    receiveTimeUs
                );
            }

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

        constexpr size_t payloadSize = 16;

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