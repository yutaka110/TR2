#include "NetworkManager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

NetworkManager::NetworkManager(const std::string& ip, uint16_t port) {
    WSADATA wsa{};
    int result = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (result != 0) {
        std::cerr << "[NetworkManager] WSAStartup failed: " << result << "\n";
        return;
    }

    udpSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket_ == INVALID_SOCKET) {
        std::cerr << "[NetworkManager] Failed to create socket: " << WSAGetLastError() << "\n";
        WSACleanup();
        return;
    }

    udpAddr_.sin_family = AF_INET;
    udpAddr_.sin_port = htons(port);

    if (InetPtonA(AF_INET, ip.c_str(), &udpAddr_.sin_addr) != 1) {
        std::cerr << "[NetworkManager] Invalid IP address: " << ip << "\n";
    }

    // recvfromをStop時に抜けやすくするため、受信タイムアウトを設定
    DWORD timeoutMs = 100;
    setsockopt(
        udpSocket_,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeoutMs),
        sizeof(timeoutMs)
    );
}

NetworkManager::~NetworkManager() {
    StopRNVPControlReceiver();

    if (udpSocket_ != INVALID_SOCKET) {
        closesocket(udpSocket_);
        udpSocket_ = INVALID_SOCKET;
    }

    WSACleanup();
}

// ============================================================
// Legacy / Current Sender
// ============================================================

void NetworkManager::SendUDPFragmented(const std::string& data, uint32_t frameId) {
    std::vector<uint8_t> bytes(data.begin(), data.end());
    SendUDPFragmented(bytes, frameId);
}

void NetworkManager::SendUDPFragmented(const std::vector<uint8_t>& data, uint32_t frameId) {
    if (udpSocket_ == INVALID_SOCKET || data.empty()) {
        return;
    }

    const size_t maxPayload = net::kMaxUdpPayloadSize;
    const size_t totalSize = data.size();

    const size_t chunkCountSizeT = (totalSize + maxPayload - 1) / maxPayload;
    if (chunkCountSizeT == 0 || chunkCountSizeT > (std::numeric_limits<uint16_t>::max)()) {
        std::cerr << "[NetworkManager] SendUDPFragmented: data too large. chunkCount="
            << chunkCountSizeT << "\n";
        return;
    }

    const uint16_t chunkCount = static_cast<uint16_t>(chunkCountSizeT);
    const uint64_t sendTimeUs = NowMicroseconds();

    for (uint16_t i = 0; i < chunkCount; ++i) {
        const size_t offset = static_cast<size_t>(i) * maxPayload;
        const size_t payloadSize = (std::min)(maxPayload, totalSize - offset);

        std::vector<uint8_t> packet(net::kPacketHeaderSize + payloadSize);

        net::PacketHeader header;
        header.frameId = frameId;
        header.chunkIndex = i;
        header.chunkCount = chunkCount;
        header.sendTimeUs = sendTimeUs;
        header.payloadSize = static_cast<uint32_t>(payloadSize);
        header.flags = (i == chunkCount - 1)
            ? net::PacketFlag_LastChunk
            : net::PacketFlag_None;

        net::EncodeHeader(packet.data(), header);
        std::memcpy(packet.data() + net::kPacketHeaderSize, data.data() + offset, payloadSize);

        const int sent = sendto(
            udpSocket_,
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()),
            0,
            reinterpret_cast<sockaddr*>(&udpAddr_),
            sizeof(udpAddr_)
        );

        if (sent == SOCKET_ERROR) {
            std::cerr << "[NetworkManager] SendUDPFragmented sendto failed: "
                << WSAGetLastError() << "\n";
        }
    }
}

void NetworkManager::SendUDPFragmentedParallel(const std::string& data, uint32_t frameId, int threadCount) {
    (void)threadCount;
    SendUDPFragmented(data, frameId);
}

// ============================================================
// RNVP v1 Data Sender
// ============================================================

void NetworkManager::SendRNVPFragmented(
    const std::string& data,
    uint32_t frameId,
    net::CodecType codecType,
    uint32_t streamId
) {
    std::vector<uint8_t> bytes(data.begin(), data.end());
    SendRNVPFragmented(bytes, frameId, codecType, streamId);
}

void NetworkManager::SendRNVPFragmented(
    const std::vector<uint8_t>& data,
    uint32_t frameId,
    net::CodecType codecType,
    uint32_t streamId
) {
    if (udpSocket_ == INVALID_SOCKET || data.empty()) {
        return;
    }

    const size_t maxPayload = net::kMaxUdpPayloadSize;
    const size_t totalSize = data.size();

    const size_t chunkCountSizeT = (totalSize + maxPayload - 1) / maxPayload;
    if (chunkCountSizeT == 0 || chunkCountSizeT > (std::numeric_limits<uint16_t>::max)()) {
        std::cerr << "[NetworkManager] SendRNVPFragmented: data too large. chunkCount="
            << chunkCountSizeT << "\n";
        return;
    }

    const uint16_t chunkCount = static_cast<uint16_t>(chunkCountSizeT);
    const uint64_t sendTimeUs = NowMicroseconds();

    for (uint16_t i = 0; i < chunkCount; ++i) {
        const size_t offset = static_cast<size_t>(i) * maxPayload;
        const size_t payloadSize = (std::min)(maxPayload, totalSize - offset);

        std::vector<uint8_t> packet(net::kRnvpHeaderV1Size + payloadSize);

        net::RnvpHeaderV1 header;
        header.magic = net::kRnvpMagic;
        header.version = net::kRnvpVersion;
        header.packetType = static_cast<uint8_t>(net::PacketType::Data);
        header.headerSize = static_cast<uint16_t>(net::kRnvpHeaderV1Size);

        header.sequence = NextRNVPSequence();
        header.streamId = streamId;

        header.frameId = frameId;
        header.chunkIndex = i;
        header.chunkCount = chunkCount;

        header.sendTimeUs = sendTimeUs;
        header.payloadSize = static_cast<uint32_t>(payloadSize);
        header.flags = (i == chunkCount - 1)
            ? net::PacketFlag_LastChunk
            : net::PacketFlag_None;

        header.codecType = static_cast<uint8_t>(codecType);

        net::EncodeRnvpHeaderV1(packet.data(), header);

        std::memcpy(
            packet.data() + net::kRnvpHeaderV1Size,
            data.data() + offset,
            payloadSize
        );

        const int sent = sendto(
            udpSocket_,
            reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(packet.size()),
            0,
            reinterpret_cast<sockaddr*>(&udpAddr_),
            sizeof(udpAddr_)
        );

        if (sent == SOCKET_ERROR) {
            std::cerr << "[NetworkManager] SendRNVPFragmented sendto failed: "
                << WSAGetLastError() << "\n";
        }
    }
}

// ============================================================
// RNVP v1 Ping
// ============================================================

void NetworkManager::SendRNVPPing(uint32_t streamId) {
    if (udpSocket_ == INVALID_SOCKET) {
        return;
    }

    constexpr size_t payloadSize = 8;

    std::vector<uint8_t> packet(net::kRnvpHeaderV1Size + payloadSize);

    const uint64_t nowUs = NowMicroseconds();

    net::PingPayload ping{};
    ping.clientTimeUs = nowUs;

    net::RnvpHeaderV1 header{};
    header.magic = net::kRnvpMagic;
    header.version = net::kRnvpVersion;
    header.packetType = static_cast<uint8_t>(net::PacketType::Ping);
    header.headerSize = static_cast<uint16_t>(net::kRnvpHeaderV1Size);

    header.sequence = NextRNVPSequence();
    header.streamId = streamId;

    header.frameId = 0;
    header.chunkIndex = 0;
    header.chunkCount = 0;

    header.sendTimeUs = nowUs;
    header.payloadSize = static_cast<uint32_t>(payloadSize);
    header.flags = net::PacketFlag_None;
    header.codecType = static_cast<uint8_t>(net::CodecType::Unknown);

    net::EncodeRnvpHeaderV1(packet.data(), header);
    net::EncodePingPayload(packet.data() + net::kRnvpHeaderV1Size, ping);

    const int sent = sendto(
        udpSocket_,
        reinterpret_cast<const char*>(packet.data()),
        static_cast<int>(packet.size()),
        0,
        reinterpret_cast<sockaddr*>(&udpAddr_),
        sizeof(udpAddr_)
    );

    if (sent == SOCKET_ERROR) {
        std::cerr << "[NetworkManager] SendRNVPPing sendto failed: "
            << WSAGetLastError() << "\n";
        return;
    }

    std::cout << "[NetworkManager] RNVP Ping sent. streamId="
        << streamId
        << " sequence="
        << header.sequence
        << "\n";
}

// ============================================================
// RNVP v1 Control Receiver
// ============================================================

bool NetworkManager::StartRNVPControlReceiver() {
    if (udpSocket_ == INVALID_SOCKET) {
        return false;
    }

    if (controlReceiverRunning_) {
        return true;
    }

    controlReceiverRunning_ = true;
    controlReceiveThread_ = std::thread(&NetworkManager::RNVPControlReceiveLoop, this);

    return true;
}

void NetworkManager::StopRNVPControlReceiver() {
    if (!controlReceiverRunning_) {
        return;
    }

    controlReceiverRunning_ = false;

    if (controlReceiveThread_.joinable()) {
        controlReceiveThread_.join();
    }
}

void NetworkManager::RNVPControlReceiveLoop() {
    std::vector<uint8_t> buffer(kControlReceiveBufferSize);

    while (controlReceiverRunning_) {
        sockaddr_in fromAddr{};
        int fromLen = sizeof(fromAddr);

        const int received = recvfrom(
            udpSocket_,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&fromAddr),
            &fromLen
        );

        if (!controlReceiverRunning_) {
            break;
        }

        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();

            // タイムアウトは正常。Stop待ちのために定期的に抜ける。
            if (error == WSAETIMEDOUT) {
                continue;
            }

            std::cerr << "[NetworkManager] RNVPControlReceiveLoop recvfrom failed: "
                << error << "\n";
            continue;
        }

        if (received <= 0) {
            continue;
        }

        HandleRnvpControlPacket(
            buffer.data(),
            static_cast<size_t>(received)
        );
    }
}

void NetworkManager::HandleRnvpControlPacket(
    const uint8_t* packetData,
    size_t packetSize
) {
    if (!packetData || packetSize < net::kRnvpHeaderV1Size) {
        return;
    }

    const uint32_t magic = net::ReadU32BE(packetData);
    if (magic != net::kRnvpMagic) {
        return;
    }

    net::RnvpHeaderV1 header{};
    if (!net::DecodeRnvpHeaderV1(packetData, packetSize, header)) {
        return;
    }

    const net::PacketType packetType =
        static_cast<net::PacketType>(header.packetType);

    const uint8_t* payload = packetData + header.headerSize;
    const size_t payloadSize = header.payloadSize;

    switch (packetType) {
    case net::PacketType::Pong:
        HandleRnvpPong(header, payload, payloadSize);
        break;

    case net::PacketType::Ack:
        HandleRnvpAck(header, payload, payloadSize);
        break;

    case net::PacketType::Control:
        HandleRnvpControl(header, payload, payloadSize);
        break;

    default:
        // NetworkManager側では基本的にDataやPingは処理しない
        break;
    }
}

void NetworkManager::HandleRnvpPong(
    const net::RnvpHeaderV1& header,
    const uint8_t* payload,
    size_t payloadSize
) {
    (void)header;

    net::PongPayload pong{};
    if (!net::DecodePongPayload(payload, payloadSize, pong)) {
        return;
    }

    const uint64_t nowUs = NowMicroseconds();

    if (nowUs < pong.clientTimeUs) {
        return;
    }

    const double rttMs =
        static_cast<double>(nowUs - pong.clientTimeUs) / 1000.0;

    {
        std::lock_guard<std::mutex> lock(rttMutex_);

        lastRttMs_ = rttMs;
        rttSampleCount_++;

        if (rttSampleCount_ == 1) {
            averageRttMs_ = rttMs;
        }
        else {
            averageRttMs_ += (rttMs - averageRttMs_) / static_cast<double>(rttSampleCount_);
        }
    }

    std::cout << "[NetworkManager] RNVP Pong received. RTT = "
        << rttMs << " ms\n";
}

void NetworkManager::HandleRnvpAck(
    const net::RnvpHeaderV1& header,
    const uint8_t* payload,
    size_t payloadSize
) {
    (void)header;

    net::AckPayload ack{};
    if (!net::DecodeAckPayload(payload, payloadSize, ack)) {
        return;
    }

    const uint32_t totalChunks =
        ack.receivedChunkCount + ack.missingChunkCount;

    double missingRate = 0.0;
    if (totalChunks > 0) {
        missingRate =
            static_cast<double>(ack.missingChunkCount) /
            static_cast<double>(totalChunks);
    }

    {
        std::lock_guard<std::mutex> lock(ackMutex_);

        lastAckFrameId_ = ack.frameId;
        lastAckReceivedChunks_ = ack.receivedChunkCount;
        lastAckMissingChunks_ = ack.missingChunkCount;
        lastAckMissingRate_ = missingRate;
        ackCount_++;
    }

    std::cout << "[NetworkManager] RNVP Ack received. frameId="
        << ack.frameId
        << " receivedChunks="
        << ack.receivedChunkCount
        << " missingChunks="
        << ack.missingChunkCount
        << " missingRate="
        << missingRate * 100.0
        << " latestSequence="
        << ack.latestSequence
        << "\n";
}

void NetworkManager::HandleRnvpControl(
    const net::RnvpHeaderV1& header,
    const uint8_t* payload,
    size_t payloadSize
) {
    (void)header;

    net::ControlPayload control{};
    if (!net::DecodeControlPayload(payload, payloadSize, control)) {
        return;
    }

    const net::ControlCommand command =
        static_cast<net::ControlCommand>(control.command);

    switch (command) {
    case net::ControlCommand::SetJpegQuality:
        std::cout << "[NetworkManager] Control: SetJpegQuality = "
            << control.value << "\n";
        break;

    case net::ControlCommand::SetTargetFps:
        std::cout << "[NetworkManager] Control: SetTargetFps = "
            << control.value << "\n";
        break;

    case net::ControlCommand::SetBitrateKbps:
        std::cout << "[NetworkManager] Control: SetBitrateKbps = "
            << control.value << "\n";
        break;

    case net::ControlCommand::RequestKeyFrame:
        std::cout << "[NetworkManager] Control: RequestKeyFrame\n";
        break;

    case net::ControlCommand::ResetStats:
        std::cout << "[NetworkManager] Control: ResetStats\n";
        break;

    default:
        std::cout << "[NetworkManager] Control: Unknown command = "
            << static_cast<int>(control.command)
            << " value="
            << control.value
            << "\n";
        break;
    }
}

// ============================================================
// RTT Accessors
// ============================================================

double NetworkManager::GetLastRttMs() const {
    std::lock_guard<std::mutex> lock(rttMutex_);
    return lastRttMs_;
}

double NetworkManager::GetAverageRttMs() const {
    std::lock_guard<std::mutex> lock(rttMutex_);
    return averageRttMs_;
}

uint64_t NetworkManager::GetRttSampleCount() const {
    std::lock_guard<std::mutex> lock(rttMutex_);
    return rttSampleCount_;
}

uint32_t NetworkManager::GetLastAckFrameId() const {
    std::lock_guard<std::mutex> lock(ackMutex_);
    return lastAckFrameId_;
}

uint32_t NetworkManager::GetLastAckReceivedChunks() const {
    std::lock_guard<std::mutex> lock(ackMutex_);
    return lastAckReceivedChunks_;
}

uint32_t NetworkManager::GetLastAckMissingChunks() const {
    std::lock_guard<std::mutex> lock(ackMutex_);
    return lastAckMissingChunks_;
}

double NetworkManager::GetLastAckMissingRate() const {
    std::lock_guard<std::mutex> lock(ackMutex_);
    return lastAckMissingRate_;
}

uint64_t NetworkManager::GetAckCount() const {
    std::lock_guard<std::mutex> lock(ackMutex_);
    return ackCount_;
}

// ============================================================
// Utility
// ============================================================

uint64_t NetworkManager::NowMicroseconds() const {
    using namespace std::chrono;

    return static_cast<uint64_t>(
        duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()
        ).count()
        );
}

uint32_t NetworkManager::NextRNVPSequence() {
    return rnvpSequence_.fetch_add(1, std::memory_order_relaxed);
}