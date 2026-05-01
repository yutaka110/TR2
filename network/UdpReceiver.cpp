#include "UdpReceiver.h"

#include <chrono>
#include <iostream>
#include <vector>

namespace net {

    UdpReceiver::UdpReceiver()
        : reassembler_(&stats_) {
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

        std::lock_guard<std::mutex> lock(frameQueueMutex_);
        while (!completedFrames_.empty()) {
            completedFrames_.pop();
        }
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
                    std::lock_guard<std::mutex> lock(frameQueueMutex_);

                    while (completedFrames_.size() >= kMaxQueuedFrames) {
                        completedFrames_.pop();
                    }

                    completedFrames_.push(std::move(*completed));
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

            if (ackInfo.valid) {
                SendRnvpAck(header, ackInfo, fromAddr);
            }

            if (completed) {
                std::lock_guard<std::mutex> lock(frameQueueMutex_);

                while (completedFrames_.size() >= kMaxQueuedFrames) {
                    completedFrames_.pop();
                }

                completedFrames_.push(std::move(*completed));
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