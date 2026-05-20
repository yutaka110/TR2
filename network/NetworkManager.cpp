#include "NetworkManager.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

namespace {

    void NetworkDebugLog(const std::string& message) {
        OutputDebugStringA(message.c_str());
        OutputDebugStringA("\n");

        std::cout << message << "\n";
    }

} // namespace

NetworkManager::NetworkManager(const std::string& ip, uint16_t port) {
    WSADATA wsa{};
    int result = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (result != 0) {
        NetworkDebugLog("[NetworkManager] WSAStartup failed: " + std::to_string(result));
        return;
    }

    udpSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket_ == INVALID_SOCKET) {
        NetworkDebugLog("[NetworkManager] Failed to create socket: " + std::to_string(WSAGetLastError()));
        WSACleanup();
        return;
    }

    // ============================================================
    // Important:
    // ------------------------------------------------------------
    // NetworkManager は Ping/Data を送るだけでなく、
    // UdpReceiver から返ってくる Pong/ACK を同じsocketで受け取る。
    //
    // そのため、recvfrom() を開始する前に必ずローカル側をbindする。
    // port 0 は OS に空いている一時ポートを割り当ててもらう指定。
    // ============================================================
    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(0);
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(udpSocket_, reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) == SOCKET_ERROR) {
        NetworkDebugLog("[NetworkManager] bind local ephemeral port failed: " + std::to_string(WSAGetLastError()));
        closesocket(udpSocket_);
        udpSocket_ = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    sockaddr_in boundAddr{};
    int boundLen = sizeof(boundAddr);
    if (getsockname(udpSocket_, reinterpret_cast<sockaddr*>(&boundAddr), &boundLen) == 0) {
        std::ostringstream oss;
        oss << "[NetworkManager] Local UDP port bound: "
            << ntohs(boundAddr.sin_port);
        NetworkDebugLog(oss.str());
    }

    udpAddr_.sin_family = AF_INET;
    udpAddr_.sin_port = htons(port);

    if (InetPtonA(AF_INET, ip.c_str(), &udpAddr_.sin_addr) != 1) {
        NetworkDebugLog("[NetworkManager] Invalid IP address: " + ip);
    }
    else {
        std::ostringstream oss;
        oss << "[NetworkManager] Remote endpoint: "
            << ip << ":" << port;
        NetworkDebugLog(oss.str());
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

        SendPacketWithSimulation(
            std::move(packet),
            "SendUDPFragmented"
        );
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
    uint32_t streamId,
    bool keyFrame
) {
    std::vector<uint8_t> bytes(data.begin(), data.end());
    SendRNVPFragmented(bytes, frameId, codecType, streamId, keyFrame);
}

void NetworkManager::SendRNVPFragmented(
    const std::vector<uint8_t>& data,
    uint32_t frameId,
    net::CodecType codecType,
    uint32_t streamId,
    bool keyFrame
) {
    SendRNVPFragmentedInternal(
        data,
        frameId,
        codecType,
        streamId,
        keyFrame,
        true,
        "SendRNVPFragmented"
    );
}

void NetworkManager::SendRNVPFragmentedInternal(
    const std::vector<uint8_t>& data,
    uint32_t frameId,
    net::CodecType codecType,
    uint32_t streamId,
    bool keyFrame,
    bool trackFrame,
    const char* context
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

    if (!SendRNVPFramePackets(
        data,
        frameId,
        codecType,
        streamId,
        keyFrame,
        sendTimeUs,
        context
    )) {
        return;
    }

    if (trackFrame) {
        TrackSentFrame(
            data,
            frameId,
            codecType,
            streamId,
            keyFrame,
            chunkCount,
            sendTimeUs
        );
    }
}

bool NetworkManager::SendRNVPFramePackets(
    const std::vector<uint8_t>& data,
    uint32_t frameId,
    net::CodecType codecType,
    uint32_t streamId,
    bool keyFrame,
    uint64_t sendTimeUs,
    const char* context
) {
    if (udpSocket_ == INVALID_SOCKET || data.empty()) {
        return false;
    }

    const size_t maxPayload = net::kMaxUdpPayloadSize;
    const size_t totalSize = data.size();

    const size_t chunkCountSizeT = (totalSize + maxPayload - 1) / maxPayload;
    if (chunkCountSizeT == 0 || chunkCountSizeT > (std::numeric_limits<uint16_t>::max)()) {
        return false;
    }

    const uint16_t chunkCount = static_cast<uint16_t>(chunkCountSizeT);

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
        if (keyFrame) {
            header.flags = net::AddPacketFlag(header.flags, net::PacketFlag_KeyFrame);
        }

        header.codecType = static_cast<uint8_t>(codecType);

        net::EncodeRnvpHeaderV1(packet.data(), header);

        std::memcpy(
            packet.data() + net::kRnvpHeaderV1Size,
            data.data() + offset,
            payloadSize
        );

        SendPacketWithSimulation(
            std::move(packet),
            context
        );
    }

    return true;
}

uint32_t NetworkManager::SendRNVPSelectedChunks(
    const std::vector<uint8_t>& data,
    uint32_t frameId,
    net::CodecType codecType,
    uint32_t streamId,
    bool keyFrame,
    uint64_t sendTimeUs,
    const std::vector<uint16_t>& chunkIndices,
    const char* context
) {
    if (udpSocket_ == INVALID_SOCKET || data.empty() || chunkIndices.empty()) {
        return 0;
    }

    const size_t maxPayload = net::kMaxUdpPayloadSize;
    const size_t totalSize = data.size();

    const size_t chunkCountSizeT = (totalSize + maxPayload - 1) / maxPayload;
    if (chunkCountSizeT == 0 || chunkCountSizeT > (std::numeric_limits<uint16_t>::max)()) {
        return 0;
    }

    const uint16_t chunkCount = static_cast<uint16_t>(chunkCountSizeT);
    uint32_t sentChunkCount = 0;

    for (uint16_t chunkIndex : chunkIndices) {
        if (chunkIndex >= chunkCount) {
            continue;
        }

        const size_t offset = static_cast<size_t>(chunkIndex) * maxPayload;
        const size_t payloadSize = (std::min)(maxPayload, totalSize - offset);

        std::vector<uint8_t> packet(net::kRnvpHeaderV1Size + payloadSize);

        net::RnvpHeaderV1 header{};
        header.magic = net::kRnvpMagic;
        header.version = net::kRnvpVersion;
        header.packetType = static_cast<uint8_t>(net::PacketType::Data);
        header.headerSize = static_cast<uint16_t>(net::kRnvpHeaderV1Size);

        header.sequence = NextRNVPSequence();
        header.streamId = streamId;

        header.frameId = frameId;
        header.chunkIndex = chunkIndex;
        header.chunkCount = chunkCount;

        header.sendTimeUs = sendTimeUs;
        header.payloadSize = static_cast<uint32_t>(payloadSize);
        header.flags = (chunkIndex == chunkCount - 1)
            ? net::PacketFlag_LastChunk
            : net::PacketFlag_None;
        if (keyFrame) {
            header.flags = net::AddPacketFlag(header.flags, net::PacketFlag_KeyFrame);
        }

        header.codecType = static_cast<uint8_t>(codecType);

        net::EncodeRnvpHeaderV1(packet.data(), header);

        std::memcpy(
            packet.data() + net::kRnvpHeaderV1Size,
            data.data() + offset,
            payloadSize
        );

        SendPacketWithSimulation(std::move(packet), context);
        sentChunkCount++;
    }

    return sentChunkCount;
}

void NetworkManager::TrackSentFrame(
    const std::vector<uint8_t>& data,
    uint32_t frameId,
    net::CodecType codecType,
    uint32_t streamId,
    bool keyFrame,
    uint16_t chunkCount,
    uint64_t sendTimeUs
) {
    SentFrameRecord record{};
    record.frameId = frameId;
    record.streamId = streamId;
    record.codecType = codecType;
    record.chunkCount = chunkCount;
    record.sendTimeUs = sendTimeUs;
    record.keyFrame = keyFrame;
    record.payload = data;

    std::lock_guard<std::mutex> lock(sentFramesMutex_);

    latestSentFrameId_ = (std::max)(latestSentFrameId_, frameId);

    auto existing = std::find_if(
        sentFrames_.begin(),
        sentFrames_.end(),
        [frameId, streamId](const SentFrameRecord& candidate) {
            return candidate.frameId == frameId && candidate.streamId == streamId;
        }
    );

    if (existing != sentFrames_.end()) {
        const uint32_t retransmitCount = existing->retransmitCount;
        *existing = std::move(record);
        existing->retransmitCount = retransmitCount;
    }
    else {
        sentFrames_.push_back(std::move(record));
    }

    while (sentFrames_.size() > kSentFrameHistoryLimit) {
        sentFrames_.pop_front();
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

    SendPacketWithSimulation(
        std::move(packet),
        "SendRNVPPing"
    );

    if (udpSocket_ == INVALID_SOCKET) {
        return;
    }

    {
        std::ostringstream oss;
        oss << "[NetworkManager] RNVP Ping sent. streamId="
            << streamId
            << " sequence="
            << header.sequence;
        NetworkDebugLog(oss.str());
    }
}

// ============================================================
// RNVP v1 Control Receiver
// ============================================================

bool NetworkManager::StartRNVPControlReceiver() {
    if (udpSocket_ == INVALID_SOCKET) {
        NetworkDebugLog("[NetworkManager] StartRNVPControlReceiver failed: invalid socket");
        return false;
    }

    if (controlReceiverRunning_) {
        NetworkDebugLog("[NetworkManager] RNVP control receiver already running");
        return true;
    }

    controlReceiverRunning_ = true;
    controlReceiveThread_ = std::thread(&NetworkManager::RNVPControlReceiveLoop, this);

    NetworkDebugLog("[NetworkManager] RNVP control receiver started");

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

        {
            std::ostringstream oss;
            oss << "[NetworkManager] RNVP control packet received. bytes="
                << received;
            NetworkDebugLog(oss.str());
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

    {
        std::ostringstream oss;
        oss << "[NetworkManager] RNVP Pong received. RTT = "
            << rttMs << " ms";
        NetworkDebugLog(oss.str());
    }
}

void NetworkManager::HandleRnvpAck(
    const net::RnvpHeaderV1& header,
    const uint8_t* payload,
    size_t payloadSize
) {
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

    HandleAckControl(header.streamId, ack, missingRate);

    std::cout << "[NetworkManager] RNVP Ack received. frameId="
        << ack.frameId
        << " receivedChunks="
        << ack.receivedChunkCount
        << " missingChunks="
        << ack.missingChunkCount
        << " missingList="
        << ack.missingChunkIndices.size()
        << " missingRate="
        << missingRate * 100.0
        << " latestSequence="
        << ack.latestSequence
        << "\n";
}

void NetworkManager::HandleAckControl(
    uint32_t streamId,
    const net::AckPayload& ack,
    double missingRate
) {
    SentFrameRecord resendRecord{};
    std::vector<uint16_t> resendChunkIndices;
    bool shouldRetransmit = false;
    bool shouldRequestKeyFrame = false;
    bool shouldCountStaleDrop = false;

    const uint64_t nowUs = NowMicroseconds();

    {
        std::lock_guard<std::mutex> lock(sentFramesMutex_);

        auto record = std::find_if(
            sentFrames_.begin(),
            sentFrames_.end(),
            [streamId, frameId = ack.frameId](const SentFrameRecord& candidate) {
                return candidate.streamId == streamId && candidate.frameId == frameId;
            }
        );

        if (ack.missingChunkCount == 0) {
            if (record != sentFrames_.end()) {
                record->acked = true;
            }

            while (!sentFrames_.empty() &&
                sentFrames_.front().acked &&
                latestSentFrameId_ > sentFrames_.front().frameId + kMaxRetransmitFrameLag) {
                sentFrames_.pop_front();
            }

            return;
        }

        if (record == sentFrames_.end()) {
            shouldCountStaleDrop = true;
            shouldRequestKeyFrame = true;
        }
        else {
            const bool staleByFrameLag =
                latestSentFrameId_ > record->frameId + kMaxRetransmitFrameLag;

            const bool staleByAge =
                nowUs > record->sendTimeUs &&
                nowUs - record->sendTimeUs > kMaxRetransmitAgeUs;

            const bool retransmitBudgetExhausted =
                record->retransmitCount >= kMaxRetransmitsPerFrame;

            if (staleByFrameLag || staleByAge || retransmitBudgetExhausted) {
                shouldCountStaleDrop = true;
                shouldRequestKeyFrame = true;
            }
            else {
                record->retransmitCount++;
                resendRecord = *record;
                resendChunkIndices = ack.missingChunkIndices;
                shouldRetransmit = true;
                ackRetransmittedFrameCount_++;
                ackRetransmittedChunkCount_ += resendChunkIndices.empty()
                    ? static_cast<uint64_t>(record->chunkCount)
                    : static_cast<uint64_t>(resendChunkIndices.size());

                if (missingRate >= 0.25) {
                    shouldRequestKeyFrame = true;
                }
            }
        }

        if (shouldCountStaleDrop) {
            ackStaleDroppedFrameCount_++;
        }

        if (shouldRequestKeyFrame) {
            forceNextKeyFrame_.store(true, std::memory_order_relaxed);
            ackKeyFrameRequestCount_++;
        }
    }

    if (shouldRetransmit) {
        const bool retransmitAsKeyFrame =
            resendRecord.keyFrame || shouldRequestKeyFrame;

        uint32_t retransmittedChunks = 0;

        if (!resendChunkIndices.empty()) {
            retransmittedChunks = SendRNVPSelectedChunks(
                resendRecord.payload,
                resendRecord.frameId,
                resendRecord.codecType,
                resendRecord.streamId,
                retransmitAsKeyFrame,
                NowMicroseconds(),
                resendChunkIndices,
                "RNVP ACK Selective Retransmit"
            );
        }
        else {
            SendRNVPFragmentedInternal(
                resendRecord.payload,
                resendRecord.frameId,
                resendRecord.codecType,
                resendRecord.streamId,
                retransmitAsKeyFrame,
                false,
                "RNVP ACK Full Retransmit"
            );

            retransmittedChunks = resendRecord.chunkCount;
        }

        std::ostringstream oss;
        oss << "[NetworkManager] ACK control retransmit. frameId="
            << resendRecord.frameId
            << " missingChunks="
            << ack.missingChunkCount
            << " missingList="
            << resendChunkIndices.size()
            << " retransmittedChunks="
            << retransmittedChunks
            << " missingRate="
            << missingRate * 100.0
            << "%";
        NetworkDebugLog(oss.str());
    }
    else if (shouldCountStaleDrop) {
        std::ostringstream oss;
        oss << "[NetworkManager] ACK control dropped stale frame. frameId="
            << ack.frameId
            << " missingChunks="
            << ack.missingChunkCount;
        NetworkDebugLog(oss.str());
    }
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
        {
            std::lock_guard<std::mutex> lock(sentFramesMutex_);
            forceNextKeyFrame_.store(true, std::memory_order_relaxed);
            ackKeyFrameRequestCount_++;
        }
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

uint64_t NetworkManager::GetAckRetransmittedFrameCount() const {
    std::lock_guard<std::mutex> lock(sentFramesMutex_);
    return ackRetransmittedFrameCount_;
}

uint64_t NetworkManager::GetAckRetransmittedChunkCount() const {
    std::lock_guard<std::mutex> lock(sentFramesMutex_);
    return ackRetransmittedChunkCount_;
}

uint64_t NetworkManager::GetAckStaleDroppedFrameCount() const {
    std::lock_guard<std::mutex> lock(sentFramesMutex_);
    return ackStaleDroppedFrameCount_;
}

uint64_t NetworkManager::GetAckKeyFrameRequestCount() const {
    std::lock_guard<std::mutex> lock(sentFramesMutex_);
    return ackKeyFrameRequestCount_;
}

bool NetworkManager::IsKeyFrameRequestPending() const {
    return forceNextKeyFrame_.load(std::memory_order_relaxed);
}

bool NetworkManager::ConsumeKeyFrameRequest() {
    bool expected = true;
    return forceNextKeyFrame_.compare_exchange_strong(
        expected,
        false,
        std::memory_order_acq_rel
    );
}

// ============================================================
// Network Condition Simulator
// ============================================================

void NetworkManager::SetNetworkCondition(
    const net::NetworkCondition& condition
) {
    networkSimulator_.SetCondition(condition);
}

net::NetworkCondition NetworkManager::GetNetworkCondition() const {
    return networkSimulator_.GetCondition();
}

net::NetworkSimulationStats NetworkManager::GetNetworkSimulationStats() const {
    return networkSimulator_.GetStats();
}

void NetworkManager::ResetStats() {
    {
        std::lock_guard<std::mutex> lock(rttMutex_);
        lastRttMs_ = 0.0;
        averageRttMs_ = 0.0;
        rttSampleCount_ = 0;
    }

    {
        std::lock_guard<std::mutex> lock(ackMutex_);
        lastAckFrameId_ = 0;
        lastAckReceivedChunks_ = 0;
        lastAckMissingChunks_ = 0;
        lastAckMissingRate_ = 0.0;
        ackCount_ = 0;
    }

    {
        std::lock_guard<std::mutex> lock(sentFramesMutex_);
        sentFrames_.clear();
        latestSentFrameId_ = 0;
        ackRetransmittedFrameCount_ = 0;
        ackRetransmittedChunkCount_ = 0;
        ackStaleDroppedFrameCount_ = 0;
        ackKeyFrameRequestCount_ = 0;
        forceNextKeyFrame_.store(false, std::memory_order_relaxed);
    }

    ResetNetworkSimulationStats();
}

void NetworkManager::ResetNetworkSimulationStats() {
    networkSimulator_.Reset();
}

void NetworkManager::FlushNetworkSimulator() {
    std::vector<std::vector<uint8_t>> readyPackets;
    networkSimulator_.PopReadyPackets(NowMicroseconds(), readyPackets);

    for (const std::vector<uint8_t>& packet : readyPackets) {
        SendPacketRaw(
            packet.data(),
            packet.size(),
            "NetworkConditionSimulator"
        );
    }
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

bool NetworkManager::SendPacketRaw(
    const uint8_t* packetData,
    size_t packetSize,
    const char* context
) {
    if (udpSocket_ == INVALID_SOCKET ||
        packetData == nullptr ||
        packetSize == 0 ||
        packetSize > static_cast<size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }

    std::lock_guard<std::mutex> lock(udpSendMutex_);

    const int sent = sendto(
        udpSocket_,
        reinterpret_cast<const char*>(packetData),
        static_cast<int>(packetSize),
        0,
        reinterpret_cast<sockaddr*>(&udpAddr_),
        sizeof(udpAddr_)
    );

    if (sent == SOCKET_ERROR) {
        std::cerr << "[NetworkManager] "
            << context
            << " sendto failed: "
            << WSAGetLastError()
            << "\n";
        return false;
    }

    return true;
}

void NetworkManager::SendPacketWithSimulation(
    std::vector<uint8_t>&& packet,
    const char* context
) {
    if (!networkSimulator_.IsEnabled()) {
        SendPacketRaw(packet.data(), packet.size(), context);
        return;
    }

    networkSimulator_.SubmitPacket(
        std::move(packet),
        NowMicroseconds()
    );

    FlushNetworkSimulator();
}
