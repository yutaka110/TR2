#include "NetworkManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

namespace {

    constexpr uint64_t kDefaultFreshnessDropThresholdUs = 120000;
    constexpr uint64_t kRetransmitFreshnessSlackUs = 15000;

    void NetworkDebugLog(const std::string& message) {
        OutputDebugStringA(message.c_str());
        OutputDebugStringA("\n");

        std::cout << message << "\n";
    }

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

    uint64_t RetransmitFreshnessDeadlineUs() {
        const uint64_t thresholdUs = FreshnessDropThresholdUs();
        if (thresholdUs <= kRetransmitFreshnessSlackUs) {
            return thresholdUs;
        }

        return thresholdUs - kRetransmitFreshnessSlackUs;
    }

    bool DefaultRnvpFecEnabled() {
        char text[16]{};
        const DWORD length = GetEnvironmentVariableA(
            "RNVP_FEC_ENABLED",
            text,
            static_cast<DWORD>(sizeof(text)));
        if (length == 0 || length >= sizeof(text)) {
            return true;
        }

        return text[0] != '0';
    }

    uint16_t ClampRnvpFecGroupChunkCount(uint16_t value) {
        return static_cast<uint16_t>(
            (std::max)(2u, (std::min)(32u, static_cast<unsigned>(value)))
        );
    }

    uint16_t DefaultRnvpFecGroupChunkCount() {
        char text[16]{};
        const DWORD length = GetEnvironmentVariableA(
            "RNVP_FEC_GROUP_CHUNKS",
            text,
            static_cast<DWORD>(sizeof(text)));
        if (length == 0 || length >= sizeof(text)) {
            return uint16_t{ 4 };
        }

        char* end = nullptr;
        const long value = std::strtol(text, &end, 10);
        if (end == text) {
            return uint16_t{ 4 };
        }

        return static_cast<uint16_t>(
            (std::max)(2L, (std::min)(32L, value))
        );
    }

} // namespace

NetworkManager::NetworkManager(const std::string& ip, uint16_t port) {
    fecEnabled_.store(DefaultRnvpFecEnabled());
    fecGroupChunkCount_.store(DefaultRnvpFecGroupChunkCount());

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

    packetPacer_.SetTargetBitrateBps(6000000);
    packetPacer_.Start(
        [this](std::vector<uint8_t>&& packet, const char* context) {
            SendPacketWithSimulation(std::move(packet), context);
        });
}

NetworkManager::~NetworkManager() {
    packetPacer_.Stop();
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

        SendPacedPacketWithSimulation(
            std::move(packet),
            context,
            net::PacketPacingPriority::Normal,
            sendTimeUs + 150000ull
        );
    }

    SendRNVPFecParity(
        data,
        frameId,
        codecType,
        streamId,
        keyFrame,
        chunkCount,
        sendTimeUs,
        context
    );

    return true;
}

bool NetworkManager::SendRNVPFecParity(
    const std::vector<uint8_t>& data,
    uint32_t frameId,
    net::CodecType codecType,
    uint32_t streamId,
    bool keyFrame,
    uint16_t chunkCount,
    uint64_t sendTimeUs,
    const char* context
) {
    if (!IsFecEnabled() ||
        udpSocket_ == INVALID_SOCKET ||
        data.empty() ||
        chunkCount <= 1) {
        return false;
    }

    const size_t maxPayload = net::kMaxUdpPayloadSize;
    const size_t totalSize = data.size();
    if (totalSize >
        static_cast<size_t>((std::numeric_limits<uint32_t>::max)())) {
        return false;
    }

    const uint16_t groupChunkCount = GetFecGroupChunkCount();
    bool sentAnyParity = false;

    for (uint32_t groupStartValue = 0;
        groupStartValue < chunkCount;
        groupStartValue += groupChunkCount) {
        const uint16_t groupStart =
            static_cast<uint16_t>(groupStartValue);
        const uint16_t chunksInGroup = static_cast<uint16_t>(
            (std::min)(
                static_cast<uint32_t>(groupChunkCount),
                static_cast<uint32_t>(chunkCount - groupStartValue)
            )
        );

        if (chunksInGroup <= 1) {
            continue;
        }

        size_t parityPayloadSize = 0;
        for (uint16_t groupOffset = 0;
            groupOffset < chunksInGroup;
            ++groupOffset) {
            const uint16_t chunkIndex =
                static_cast<uint16_t>(groupStart + groupOffset);
            const size_t offset =
                static_cast<size_t>(chunkIndex) * maxPayload;
            if (offset >= totalSize) {
                break;
            }

            const size_t chunkSize =
                (std::min)(maxPayload, totalSize - offset);
            parityPayloadSize =
                (std::max)(parityPayloadSize, chunkSize);
        }

        if (parityPayloadSize == 0 ||
            parityPayloadSize > maxPayload) {
            continue;
        }

        std::vector<uint8_t> parity(parityPayloadSize, 0);
        for (uint16_t groupOffset = 0;
            groupOffset < chunksInGroup;
            ++groupOffset) {
            const uint16_t chunkIndex =
                static_cast<uint16_t>(groupStart + groupOffset);
            const size_t offset =
                static_cast<size_t>(chunkIndex) * maxPayload;
            if (offset >= totalSize) {
                break;
            }

            const size_t chunkSize =
                (std::min)(maxPayload, totalSize - offset);
            for (size_t i = 0; i < chunkSize; ++i) {
                parity[i] ^= data[offset + i];
            }
        }

        const size_t payloadSize =
            net::kFecPayloadHeaderSize + parityPayloadSize;
        std::vector<uint8_t> packet(net::kRnvpHeaderV1Size + payloadSize);

        net::RnvpHeaderV1 header{};
        header.magic = net::kRnvpMagic;
        header.version = net::kRnvpVersion;
        header.packetType = static_cast<uint8_t>(net::PacketType::Fec);
        header.headerSize = static_cast<uint16_t>(net::kRnvpHeaderV1Size);
        header.sequence = NextRNVPSequence();
        header.streamId = streamId;
        header.frameId = frameId;
        header.chunkIndex = groupStart;
        header.chunkCount = chunkCount;
        header.sendTimeUs = sendTimeUs;
        header.payloadSize = static_cast<uint32_t>(payloadSize);
        header.flags = net::PacketFlag_DroppedAllowed;
        if (keyFrame) {
            header.flags = net::AddPacketFlag(
                header.flags,
                net::PacketFlag_KeyFrame
            );
        }
        header.codecType = static_cast<uint8_t>(codecType);

        net::FecPayloadHeader fecHeader{};
        fecHeader.framePayloadBytes = static_cast<uint32_t>(totalSize);
        fecHeader.parityPayloadBytes =
            static_cast<uint16_t>(parityPayloadSize);
        fecHeader.protectedChunkCount = chunksInGroup;

        net::EncodeRnvpHeaderV1(packet.data(), header);
        net::EncodeFecPayloadHeader(
            packet.data() + net::kRnvpHeaderV1Size,
            fecHeader
        );
        std::memcpy(
            packet.data() + net::kRnvpHeaderV1Size + net::kFecPayloadHeaderSize,
            parity.data(),
            parity.size()
        );

        SendPacedPacketWithSimulation(
            std::move(packet),
            context,
            net::PacketPacingPriority::Normal,
            sendTimeUs + 150000ull
        );
        sentAnyParity = true;
    }

    return sentAnyParity;
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

        SendPacedPacketWithSimulation(
            std::move(packet),
            context,
            net::PacketPacingPriority::High,
            sendTimeUs + 150000ull
        );
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

    case net::PacketType::TransportFeedback:
        HandleRnvpTransportFeedback(header, payload, payloadSize);
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
        maxRttMs_ = (std::max)(maxRttMs_, rttMs);
        rttSampleCount_++;

        if (rttSampleCount_ == 1) {
            averageRttMs_ = rttMs;
        }
        else {
            averageRttMs_ += (rttMs - averageRttMs_) / static_cast<double>(rttSampleCount_);
        }
    }

    bandwidthEstimator_.OnRttSample(rttMs);

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
    const net::NetworkCondition condition = networkSimulator_.GetCondition();
    const uint64_t simulatorOneWayDelayUs =
        condition.enabled
        ? static_cast<uint64_t>(condition.maxDelayMs) * 1000ull
        : 0ull;
    const double averageRttMs = GetAverageRttMs();
    const uint64_t rttOneWayDelayUs =
        averageRttMs > 0.0
        ? static_cast<uint64_t>((averageRttMs * 1000.0) * 0.5)
        : 0ull;
    const uint64_t estimatedRetransmitDeliveryUs =
        (std::max)(simulatorOneWayDelayUs, rttOneWayDelayUs);

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

            const uint64_t retransmitDeadlineUs =
                (std::min)(
                    kMaxRetransmitAgeUs,
                    RetransmitFreshnessDeadlineUs());

            const bool staleByAge =
                nowUs > record->sendTimeUs &&
                nowUs - record->sendTimeUs +
                    estimatedRetransmitDeliveryUs > retransmitDeadlineUs;

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

void NetworkManager::HandleRnvpTransportFeedback(
    const net::RnvpHeaderV1& header,
    const uint8_t* payload,
    size_t payloadSize
) {
    (void)header;

    net::TransportFeedbackPayload feedback{};
    if (!net::DecodeTransportFeedbackPayload(
        payload,
        payloadSize,
        feedback)) {
        return;
    }

    uint64_t receivedCount = 0;
    uint64_t missingCount = 0;
    double jitterSumMs = 0.0;
    uint32_t jitterSamples = 0;
    double queueTrendSumMs = 0.0;
    uint32_t queueTrendSamples = 0;
    std::vector<net::BandwidthFeedbackPacket> bandwidthFeedbackPackets;
    bandwidthFeedbackPackets.reserve(feedback.entries.size());

    bool hasPreviousReceived = false;
    uint64_t previousSendTimeUs = 0;
    uint64_t previousReceiveTimeUs = 0;

    {
        std::lock_guard<std::mutex> sentLock(sentPacketsMutex_);

        for (const net::TransportFeedbackEntry& entry : feedback.entries) {
            const uint32_t sequence =
                feedback.baseSequence + entry.sequenceDelta;
            const bool received =
                (entry.flags & net::TransportFeedbackFlag_Received) != 0;
            const bool missing =
                (entry.flags & net::TransportFeedbackFlag_Missing) != 0;

            if (missing && !received) {
                missingCount++;
                net::BandwidthFeedbackPacket bandwidthPacket{};
                bandwidthPacket.sequence = sequence;
                bandwidthPacket.received = false;
                bandwidthFeedbackPackets.push_back(bandwidthPacket);
                continue;
            }

            if (!received) {
                continue;
            }

            receivedCount++;

            const auto sentIt = std::find_if(
                sentPackets_.begin(),
                sentPackets_.end(),
                [sequence](const SentPacketRecord& record) {
                    return record.sequence == sequence;
                });

            const uint64_t receiveTimeUs =
                feedback.referenceReceiveTimeUs + entry.receiveDeltaUs;

            if (sentIt == sentPackets_.end()) {
                net::BandwidthFeedbackPacket bandwidthPacket{};
                bandwidthPacket.sequence = sequence;
                bandwidthPacket.receiveTimeUs = receiveTimeUs;
                bandwidthPacket.received = true;
                bandwidthFeedbackPackets.push_back(bandwidthPacket);
                hasPreviousReceived = false;
                continue;
            }

            net::BandwidthFeedbackPacket bandwidthPacket{};
            bandwidthPacket.sequence = sequence;
            bandwidthPacket.sendTimeUs = sentIt->sendTimeUs;
            bandwidthPacket.receiveTimeUs = receiveTimeUs;
            bandwidthPacket.packetBytes = sentIt->packetBytes;
            bandwidthPacket.received = true;
            bandwidthFeedbackPackets.push_back(bandwidthPacket);

            if (hasPreviousReceived &&
                sentIt->sendTimeUs >= previousSendTimeUs &&
                receiveTimeUs >= previousReceiveTimeUs) {
                const double sendIntervalMs =
                    static_cast<double>(
                        sentIt->sendTimeUs - previousSendTimeUs) / 1000.0;
                const double receiveIntervalMs =
                    static_cast<double>(
                        receiveTimeUs - previousReceiveTimeUs) / 1000.0;
                const double intervalDeltaMs =
                    receiveIntervalMs - sendIntervalMs;

                jitterSumMs += std::abs(intervalDeltaMs);
                jitterSamples++;
                if (intervalDeltaMs > 0.0) {
                    queueTrendSumMs += intervalDeltaMs;
                    queueTrendSamples++;
                }
            }

            previousSendTimeUs = sentIt->sendTimeUs;
            previousReceiveTimeUs = receiveTimeUs;
            hasPreviousReceived = true;
        }
    }

    const uint64_t statusCount = receivedCount + missingCount;
    const double lossRate = statusCount > 0
        ? static_cast<double>(missingCount) / static_cast<double>(statusCount)
        : 0.0;
    const double arrivalJitterMs = jitterSamples > 0
        ? jitterSumMs / static_cast<double>(jitterSamples)
        : 0.0;
    const double queueDelayTrendMs = queueTrendSamples > 0
        ? queueTrendSumMs / static_cast<double>(queueTrendSamples)
        : 0.0;

    {
        std::lock_guard<std::mutex> lock(transportFeedbackMutex_);
        transportFeedbackStats_.feedbackPackets++;
        transportFeedbackStats_.feedbackPacketStatuses += statusCount;
        transportFeedbackStats_.feedbackReceivedPackets += receivedCount;
        transportFeedbackStats_.feedbackMissingPackets += missingCount;
        transportFeedbackStats_.feedbackLossRate = lossRate;
        transportFeedbackStats_.feedbackArrivalJitterMs = arrivalJitterMs;
        transportFeedbackStats_.feedbackQueueDelayTrendMs = queueDelayTrendMs;
        transportFeedbackStats_.lastFeedbackSequence =
            feedback.feedbackSequence;
    }

    bandwidthEstimator_.OnTransportFeedback(bandwidthFeedbackPackets);
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

double NetworkManager::GetMaxRttMs() const {
    std::lock_guard<std::mutex> lock(rttMutex_);
    return maxRttMs_;
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

void NetworkManager::SetPacingEnabled(bool enabled) {
    packetPacer_.SetEnabled(enabled);
}

bool NetworkManager::IsPacingEnabled() const {
    return packetPacer_.IsEnabled();
}

void NetworkManager::SetPacingTargetBitrateKbps(uint32_t bitrateKbps) {
    const uint32_t bitrateBps =
        (std::max)(uint32_t{ 1 }, bitrateKbps) * 1000u;
    packetPacer_.SetTargetBitrateBps(bitrateBps);
}

net::PacketPacerStats NetworkManager::GetPacingStats() const {
    return packetPacer_.GetStats();
}

void NetworkManager::SetFecEnabled(bool enabled) {
    fecEnabled_.store(enabled, std::memory_order_relaxed);
}

bool NetworkManager::IsFecEnabled() const {
    return fecEnabled_.load(std::memory_order_relaxed);
}

void NetworkManager::SetAdaptiveFecEnabled(bool enabled) {
    const bool previous =
        adaptiveFecEnabled_.exchange(enabled, std::memory_order_relaxed);
    if (previous == enabled) {
        return;
    }

    std::lock_guard<std::mutex> lock(adaptiveFecMutex_);
    adaptiveFecLastDeadlineNackSentFrames_ = 0;
    adaptiveFecLastDeadlineNackExpiredDroppedFrames_ = 0;
    adaptiveFecLastParityPackets_ = 0;
    adaptiveFecLastRecoveredFrames_ = 0;
    adaptiveFecLossPressureEma_ = 0.0;
    adaptiveFecStableSamples_ = 0;
    adaptiveFecHoldSamples_ = 0;
    adaptiveFecHoldGroupChunkCount_ = 8;
    adaptiveFecHoldUntilUs_ = 0;
    adaptiveFecG2UntilUs_ = 0;
    adaptiveFecG2CooldownUntilUs_ = 0;
    adaptiveFecIneffectiveOffUntilUs_ = 0;
    adaptiveFecIneffectiveOffStartExpiredFrames_ = 0;
    adaptiveFecIneffectiveOffRearmGroupChunkCount_ = 8;
    adaptiveFecPostOffRearmSamples_ = 0;
    adaptiveFecPostOffRearmGroupChunkCount_ = 8;
    adaptiveFecWasteSamples_ = 0;
    adaptiveFecIneffectiveSamples_ = 0;
    adaptiveFecUncoveredDeadlineSamples_ = 0;
    adaptiveFecCoveredRecoverySamples_ = 0;
}

bool NetworkManager::IsAdaptiveFecEnabled() const {
    return adaptiveFecEnabled_.load(std::memory_order_relaxed);
}

void NetworkManager::SetFecGroupChunkCount(uint16_t groupChunkCount) {
    fecGroupChunkCount_.store(
        ClampRnvpFecGroupChunkCount(groupChunkCount),
        std::memory_order_relaxed
    );
}

uint16_t NetworkManager::GetFecGroupChunkCount() const {
    return fecGroupChunkCount_.load(std::memory_order_relaxed);
}

void NetworkManager::UpdateAdaptiveFec(
    double packetLossRate,
    double ackMissingRate,
    uint64_t deadlineNackSentFrames,
    uint64_t deadlineNackExpiredDroppedFrames,
    uint64_t fecParityPackets,
    uint64_t fecRecoveredFrames,
    uint32_t estimatedBandwidthBps,
    uint32_t targetBitrateKbps,
    double queueDelayMs
) {
    if (!IsAdaptiveFecEnabled()) {
        return;
    }

    const uint64_t nowUs = NowMicroseconds();
    const auto sanitizeRate = [](double value) {
        if (!std::isfinite(value)) {
            return 0.0;
        }
        return (std::max)(0.0, (std::min)(1.0, value));
    };
    const auto subtractCounter =
        [](uint64_t current, uint64_t previous) -> uint64_t {
        return current >= previous
            ? current - previous
            : current;
    };

    const double instantLossPressure =
        (std::max)(
            sanitizeRate(packetLossRate),
            sanitizeRate(ackMissingRate));
    uint64_t deadlineNackDelta = 0;
    uint64_t deadlineExpiredDelta = 0;
    uint64_t fecParityDelta = 0;
    uint64_t fecRecoveredDelta = 0;
    double lossPressure = instantLossPressure;

    {
        std::lock_guard<std::mutex> lock(adaptiveFecMutex_);
        deadlineNackDelta = subtractCounter(
            deadlineNackSentFrames,
            adaptiveFecLastDeadlineNackSentFrames_);
        deadlineExpiredDelta = subtractCounter(
            deadlineNackExpiredDroppedFrames,
            adaptiveFecLastDeadlineNackExpiredDroppedFrames_);
        fecParityDelta = subtractCounter(
            fecParityPackets,
            adaptiveFecLastParityPackets_);
        fecRecoveredDelta = subtractCounter(
            fecRecoveredFrames,
            adaptiveFecLastRecoveredFrames_);
        adaptiveFecLastDeadlineNackSentFrames_ =
            deadlineNackSentFrames;
        adaptiveFecLastDeadlineNackExpiredDroppedFrames_ =
            deadlineNackExpiredDroppedFrames;
        adaptiveFecLastParityPackets_ = fecParityPackets;
        adaptiveFecLastRecoveredFrames_ = fecRecoveredFrames;

        adaptiveFecLossPressureEma_ =
            adaptiveFecLossPressureEma_ <= 0.0
            ? instantLossPressure
            : adaptiveFecLossPressureEma_ * 0.70 +
                instantLossPressure * 0.30;
        lossPressure =
            (std::max)(instantLossPressure, adaptiveFecLossPressureEma_);
    }

    const bool burstMissingSignal = ackMissingRate >= 0.20;
    const bool recoveryDeadlinePressure =
        deadlineExpiredDelta > 0 ||
        deadlineNackDelta >= 2;
    const bool fecWasUseful = fecRecoveredDelta > 0;
    const bool fecCoveredDeadlineMiss =
        fecWasUseful &&
        (deadlineExpiredDelta == 0 ||
            fecRecoveredDelta >= deadlineExpiredDelta);
    const bool nackNeedsFecFallback =
        deadlineNackDelta > 0 &&
        !fecCoveredDeadlineMiss;
    const bool recoveryDeadlineUncovered =
        deadlineExpiredDelta > 0 &&
        !fecCoveredDeadlineMiss;
    const bool deadlineExpiryStillRising = deadlineExpiredDelta > 0;
    const bool severeDeadlineExpired = deadlineExpiredDelta >= 4;
    const bool deadlineRiskForStrongerFec =
        severeDeadlineExpired ||
        recoveryDeadlineUncovered ||
        deadlineExpiryStillRising;
    const bool fecWasWasteful =
        fecParityDelta > 0 &&
        fecRecoveredDelta == 0 &&
        !recoveryDeadlinePressure;
    const bool fecWasIneffective =
        fecParityDelta > 0 &&
        fecRecoveredDelta == 0 &&
        deadlineExpiredDelta == 0 &&
        !nackNeedsFecFallback;
    const bool fecWasDeadlineIneffective =
        fecParityDelta > 0 &&
        deadlineExpiredDelta > 0 &&
        fecRecoveredDelta < deadlineExpiredDelta;
    const bool bandwidthTight =
        estimatedBandwidthBps > 0 &&
        targetBitrateKbps > 0 &&
        estimatedBandwidthBps <
        static_cast<uint32_t>(
            static_cast<uint64_t>(targetBitrateKbps) * 1100ull);
    const bool pacingBacklog = queueDelayMs >= 35.0;
    const bool highBitrateMode = targetBitrateKbps >= 4500;
    constexpr uint32_t kEmergencyFecUncoveredDeadlineSamples = 2;
    constexpr uint64_t kAdaptiveFecG2EmergencyWindowUs = 600000;
    constexpr uint64_t kAdaptiveFecG2CooldownUs = 2000000;
    constexpr uint64_t kAdaptiveFecIneffectiveOffWindowUs = 1200000;
    const bool strongFecBudgetAvailable =
        targetBitrateKbps == 0 ||
        targetBitrateKbps <= 3200 ||
        (estimatedBandwidthBps > 0 &&
            estimatedBandwidthBps >
            static_cast<uint32_t>(
                static_cast<uint64_t>(targetBitrateKbps) * 1800ull));
    const bool highPressure =
        severeDeadlineExpired ||
        (recoveryDeadlineUncovered && deadlineExpiredDelta >= 2) ||
        (deadlineExpiredDelta > 0 && burstMissingSignal);
    const bool mediumPressure =
        deadlineRiskForStrongerFec ||
        (nackNeedsFecFallback && deadlineExpiredDelta > 0);
    const bool lowPressure =
        recoveryDeadlinePressure ||
        burstMissingSignal ||
        fecWasUseful;

    bool enableFec = false;
    uint16_t groupChunkCount = 8;

    if (highPressure) {
        groupChunkCount = 4;
        enableFec = true;
    }
    else if (mediumPressure) {
        groupChunkCount = 4;
        enableFec = true;
    }
    else if (lowPressure) {
        groupChunkCount = 8;
        enableFec = true;
    }

    if ((bandwidthTight || pacingBacklog) && !highPressure) {
        if (deadlineRiskForStrongerFec) {
            groupChunkCount = 4;
        }
        else if (recoveryDeadlinePressure || burstMissingSignal) {
            groupChunkCount = 8;
        }
        else {
            enableFec = false;
            groupChunkCount = 8;
        }
    }
    else if (groupChunkCount == 2 && !strongFecBudgetAvailable) {
        groupChunkCount = 4;
    }
    else if ((bandwidthTight || pacingBacklog) &&
        groupChunkCount == 2 &&
        deadlineExpiredDelta == 0 &&
        instantLossPressure < 0.14) {
        groupChunkCount = 4;
    }

    if (groupChunkCount == 4 &&
        highBitrateMode &&
        !deadlineRiskForStrongerFec &&
        (instantLossPressure >= 0.18 || lossPressure >= 0.14)) {
        groupChunkCount = 8;
    }

    {
        std::lock_guard<std::mutex> lock(adaptiveFecMutex_);
        const auto holdRecoveryRole =
            [&](double seconds, uint16_t group) {
            const uint64_t holdUs =
                nowUs + static_cast<uint64_t>(seconds * 1000000.0);
            adaptiveFecHoldUntilUs_ =
                (std::max)(adaptiveFecHoldUntilUs_, holdUs);
            adaptiveFecHoldGroupChunkCount_ =
                (std::min)(adaptiveFecHoldGroupChunkCount_, group);
        };
        const auto beginIneffectiveOff =
            [&](bool deadlineRisk) {
            adaptiveFecIneffectiveOffUntilUs_ =
                nowUs + kAdaptiveFecIneffectiveOffWindowUs;
            adaptiveFecIneffectiveOffStartExpiredFrames_ =
                deadlineNackExpiredDroppedFrames;
            adaptiveFecIneffectiveOffRearmGroupChunkCount_ =
                deadlineRisk ? uint16_t{ 4 } : uint16_t{ 8 };
        };

        if (recoveryDeadlineUncovered ||
            deadlineExpiryStillRising ||
            severeDeadlineExpired) {
            adaptiveFecUncoveredDeadlineSamples_++;
            adaptiveFecCoveredRecoverySamples_ = 0;
        }
        else if (fecCoveredDeadlineMiss) {
            adaptiveFecCoveredRecoverySamples_++;
            adaptiveFecUncoveredDeadlineSamples_ = 0;
            if (adaptiveFecCoveredRecoverySamples_ >= 2 &&
                adaptiveFecHoldGroupChunkCount_ == 4) {
                adaptiveFecHoldGroupChunkCount_ = 8;
                adaptiveFecHoldSamples_ =
                    (std::min)(adaptiveFecHoldSamples_, uint32_t{ 1 });
                adaptiveFecHoldUntilUs_ =
                    (std::min)(adaptiveFecHoldUntilUs_, nowUs + 600000ull);
            }
        }
        else if (!recoveryDeadlinePressure && !burstMissingSignal) {
            adaptiveFecUncoveredDeadlineSamples_ = 0;
            adaptiveFecCoveredRecoverySamples_ = 0;
        }

        if (adaptiveFecG2UntilUs_ != 0 &&
            adaptiveFecG2UntilUs_ <= nowUs) {
            adaptiveFecG2UntilUs_ = 0;
            adaptiveFecG2CooldownUntilUs_ =
                (std::max)(
                    adaptiveFecG2CooldownUntilUs_,
                    nowUs + kAdaptiveFecG2CooldownUs);
            if (adaptiveFecHoldGroupChunkCount_ == 2) {
                adaptiveFecHoldGroupChunkCount_ = 4;
            }
        }

        const bool g2CooldownActive = adaptiveFecG2CooldownUntilUs_ > nowUs;
        const bool ineffectiveOffExpired =
            adaptiveFecIneffectiveOffUntilUs_ != 0 &&
            adaptiveFecIneffectiveOffUntilUs_ <= nowUs;
        if (ineffectiveOffExpired) {
            const uint64_t nackExpiredDuringOffDelta =
                subtractCounter(
                    deadlineNackExpiredDroppedFrames,
                    adaptiveFecIneffectiveOffStartExpiredFrames_);
            const bool nackExpiredIncreasedAfterOff =
                deadlineExpiredDelta > 0;
            const bool rearmAsRecoveryDefense =
                nackExpiredDuringOffDelta >= 2 ||
                deadlineExpiredDelta >= 2 ||
                (nackExpiredIncreasedAfterOff &&
                    adaptiveFecIneffectiveOffRearmGroupChunkCount_ <= 4);
            adaptiveFecPostOffRearmSamples_ = 2u;
            adaptiveFecPostOffRearmGroupChunkCount_ =
                rearmAsRecoveryDefense ? uint16_t{ 4 } : uint16_t{ 8 };
            adaptiveFecIneffectiveOffUntilUs_ = 0;
            adaptiveFecIneffectiveOffStartExpiredFrames_ = 0;
            adaptiveFecIneffectiveOffRearmGroupChunkCount_ = 8;
            adaptiveFecHoldSamples_ =
                (std::max)(
                    adaptiveFecHoldSamples_,
                    adaptiveFecPostOffRearmSamples_);
            adaptiveFecHoldGroupChunkCount_ =
                (std::min)(
                    adaptiveFecHoldGroupChunkCount_,
                    adaptiveFecPostOffRearmGroupChunkCount_);
            const uint64_t rearmHoldUs =
                nowUs +
                (rearmAsRecoveryDefense ? 1000000ull : 800000ull);
            adaptiveFecHoldUntilUs_ =
                (std::max)(adaptiveFecHoldUntilUs_, rearmHoldUs);
        }

        if (recoveryDeadlineUncovered ||
            deadlineExpiryStillRising ||
            severeDeadlineExpired) {
            adaptiveFecHoldSamples_ =
                (std::max)(adaptiveFecHoldSamples_, uint32_t{ 5 });
            const bool emergencyFecTrigger =
                adaptiveFecUncoveredDeadlineSamples_ >=
                kEmergencyFecUncoveredDeadlineSamples &&
                deadlineExpiredDelta > 0 &&
                !g2CooldownActive;
            if (emergencyFecTrigger && adaptiveFecG2UntilUs_ == 0) {
                adaptiveFecG2UntilUs_ =
                    nowUs + kAdaptiveFecG2EmergencyWindowUs;
            }
            const bool g2EmergencyActive =
                adaptiveFecG2UntilUs_ > nowUs;
            groupChunkCount =
                g2EmergencyActive
                ? uint16_t{ 2 }
                : uint16_t{ 4 };
            holdRecoveryRole(
                g2EmergencyActive ? 0.6 : 1.2,
                groupChunkCount);
        }
        else if (nackNeedsFecFallback ||
            burstMissingSignal ||
            deadlineNackDelta >= 2) {
            const bool nackOrBurstDeadlineRisk =
                deadlineRiskForStrongerFec ||
                fecWasDeadlineIneffective;
            const uint16_t fallbackGroup =
                nackOrBurstDeadlineRisk ? uint16_t{ 4 } : uint16_t{ 8 };
            adaptiveFecHoldSamples_ =
                (std::max)(
                    adaptiveFecHoldSamples_,
                    nackOrBurstDeadlineRisk ? uint32_t{ 4 } : uint32_t{ 2 });
            holdRecoveryRole(
                nackOrBurstDeadlineRisk ? 1.2 : 0.8,
                fallbackGroup);
        }
        else if (fecCoveredDeadlineMiss || fecWasUseful) {
            adaptiveFecHoldSamples_ =
                (std::max)(
                    adaptiveFecHoldSamples_,
                    fecCoveredDeadlineMiss ? uint32_t{ 3 } : uint32_t{ 2 });
            holdRecoveryRole(
                fecCoveredDeadlineMiss ? 0.8 : 1.0,
                groupChunkCount);
        }
        else if (adaptiveFecPostOffRearmSamples_ > 0) {
            enableFec = true;
            groupChunkCount = adaptiveFecPostOffRearmGroupChunkCount_;
            adaptiveFecPostOffRearmSamples_--;
            if (adaptiveFecPostOffRearmSamples_ == 0) {
                adaptiveFecPostOffRearmGroupChunkCount_ = 8;
            }
        }
        else if (adaptiveFecHoldSamples_ > 0) {
            adaptiveFecHoldSamples_--;
            if (adaptiveFecHoldSamples_ == 0 &&
                adaptiveFecHoldUntilUs_ <= nowUs) {
                adaptiveFecHoldGroupChunkCount_ = 8;
            }
        }

        if (fecWasWasteful) {
            adaptiveFecWasteSamples_++;
        }
        else {
            adaptiveFecWasteSamples_ = 0;
        }
        if (fecWasIneffective || fecWasDeadlineIneffective) {
            adaptiveFecIneffectiveSamples_++;
        }
        else if (fecCoveredDeadlineMiss || !recoveryDeadlinePressure) {
            adaptiveFecIneffectiveSamples_ = 0;
        }

        const bool g2EmergencyActiveNow = adaptiveFecG2UntilUs_ > nowUs;
        if (!g2EmergencyActiveNow &&
            adaptiveFecHoldGroupChunkCount_ == 2) {
            adaptiveFecHoldGroupChunkCount_ = 4;
        }

        const bool recoveryHoldActive =
            adaptiveFecHoldSamples_ > 0 ||
            adaptiveFecHoldUntilUs_ > nowUs;
        if (recoveryHoldActive) {
            enableFec = true;
            groupChunkCount =
                (std::min)(groupChunkCount, adaptiveFecHoldGroupChunkCount_);
        }
        else {
            adaptiveFecHoldGroupChunkCount_ = 8;
        }
        if (adaptiveFecIneffectiveSamples_ >= 2 &&
            (!recoveryDeadlinePressure || fecWasDeadlineIneffective) &&
            (!burstMissingSignal || fecWasDeadlineIneffective)) {
            enableFec = false;
            groupChunkCount = 8;
            adaptiveFecHoldSamples_ = 0;
            adaptiveFecHoldGroupChunkCount_ = 8;
            adaptiveFecHoldUntilUs_ = 0;
            adaptiveFecPostOffRearmSamples_ = 0;
            adaptiveFecPostOffRearmGroupChunkCount_ = 8;
            beginIneffectiveOff(
                fecWasDeadlineIneffective ||
                recoveryDeadlineUncovered ||
                nackNeedsFecFallback ||
                deadlineExpiredDelta > 0);
            if (adaptiveFecG2UntilUs_ > nowUs) {
                adaptiveFecG2CooldownUntilUs_ =
                    (std::max)(
                        adaptiveFecG2CooldownUntilUs_,
                        nowUs + kAdaptiveFecG2CooldownUs);
            }
            adaptiveFecG2UntilUs_ = 0;
            adaptiveFecIneffectiveSamples_ = 0;
        }
        if (adaptiveFecWasteSamples_ >= 2 &&
            !recoveryDeadlinePressure &&
            !burstMissingSignal) {
            enableFec = false;
            groupChunkCount = 8;
            adaptiveFecHoldSamples_ = 0;
            adaptiveFecHoldGroupChunkCount_ = 8;
            adaptiveFecHoldUntilUs_ = 0;
            adaptiveFecPostOffRearmSamples_ = 0;
            adaptiveFecPostOffRearmGroupChunkCount_ = 8;
            beginIneffectiveOff(false);
            if (adaptiveFecG2UntilUs_ > nowUs) {
                adaptiveFecG2CooldownUntilUs_ =
                    (std::max)(
                        adaptiveFecG2CooldownUntilUs_,
                        nowUs + kAdaptiveFecG2CooldownUs);
            }
            adaptiveFecG2UntilUs_ = 0;
            adaptiveFecIneffectiveSamples_ = 0;
            adaptiveFecUncoveredDeadlineSamples_ = 0;
        }

        if (adaptiveFecIneffectiveOffUntilUs_ > nowUs &&
            !severeDeadlineExpired) {
            enableFec = false;
            groupChunkCount = 8;
            adaptiveFecHoldSamples_ = 0;
            adaptiveFecHoldGroupChunkCount_ = 8;
            adaptiveFecHoldUntilUs_ = 0;
            adaptiveFecPostOffRearmSamples_ = 0;
            adaptiveFecPostOffRearmGroupChunkCount_ = 8;
        }

        if (groupChunkCount == 2 && adaptiveFecG2UntilUs_ <= nowUs) {
            groupChunkCount = 4;
        }

        if (!enableFec) {
            adaptiveFecStableSamples_++;
        }
        else {
            adaptiveFecStableSamples_ = 0;
        }
    }

    SetFecEnabled(enableFec);
    SetFecGroupChunkCount(groupChunkCount);
}

NetworkManager::TransportFeedbackStats
NetworkManager::GetTransportFeedbackStats() const {
    std::lock_guard<std::mutex> lock(transportFeedbackMutex_);
    return transportFeedbackStats_;
}

net::BandwidthEstimatorStats
NetworkManager::GetBandwidthEstimatorStats() const {
    return bandwidthEstimator_.GetStats();
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
        maxRttMs_ = 0.0;
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
        std::lock_guard<std::mutex> lock(adaptiveFecMutex_);
        adaptiveFecLastDeadlineNackSentFrames_ = 0;
        adaptiveFecLastDeadlineNackExpiredDroppedFrames_ = 0;
        adaptiveFecLastParityPackets_ = 0;
        adaptiveFecLastRecoveredFrames_ = 0;
        adaptiveFecLossPressureEma_ = 0.0;
        adaptiveFecStableSamples_ = 0;
        adaptiveFecHoldSamples_ = 0;
        adaptiveFecHoldGroupChunkCount_ = 8;
        adaptiveFecHoldUntilUs_ = 0;
        adaptiveFecG2UntilUs_ = 0;
        adaptiveFecG2CooldownUntilUs_ = 0;
        adaptiveFecIneffectiveOffUntilUs_ = 0;
        adaptiveFecIneffectiveOffStartExpiredFrames_ = 0;
        adaptiveFecIneffectiveOffRearmGroupChunkCount_ = 8;
        adaptiveFecPostOffRearmSamples_ = 0;
        adaptiveFecPostOffRearmGroupChunkCount_ = 8;
        adaptiveFecWasteSamples_ = 0;
        adaptiveFecIneffectiveSamples_ = 0;
        adaptiveFecUncoveredDeadlineSamples_ = 0;
        adaptiveFecCoveredRecoverySamples_ = 0;
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

    {
        std::lock_guard<std::mutex> lock(sentPacketsMutex_);
        sentPackets_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(transportFeedbackMutex_);
        transportFeedbackStats_ = TransportFeedbackStats{};
    }

    bandwidthEstimator_.Reset();

    ResetNetworkSimulationStats();
}

void NetworkManager::ResetNetworkSimulationStats() {
    networkSimulator_.Reset();
    packetPacer_.ResetStats();
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

void NetworkManager::SendPacedPacketWithSimulation(
    std::vector<uint8_t>&& packet,
    const char* context,
    net::PacketPacingPriority priority,
    uint64_t deadlineUs
) {
    if (!packetPacer_.IsEnabled()) {
        SendPacketWithSimulation(std::move(packet), context);
        return;
    }

    if (!packetPacer_.EnqueuePacket(
        std::move(packet),
        context,
        priority,
        deadlineUs)) {
        SendPacketWithSimulation(std::move(packet), context);
    }
}

void NetworkManager::SendPacketWithSimulation(
    std::vector<uint8_t>&& packet,
    const char* context
) {
    const uint64_t nowUs = NowMicroseconds();
    TrackSentRnvpDataPacket(packet.data(), packet.size(), nowUs);

    if (!networkSimulator_.IsEnabled()) {
        SendPacketRaw(packet.data(), packet.size(), context);
        return;
    }

    networkSimulator_.SubmitPacket(
        std::move(packet),
        nowUs
    );

    FlushNetworkSimulator();
}

void NetworkManager::TrackSentRnvpDataPacket(
    const uint8_t* packetData,
    size_t packetSize,
    uint64_t sendTimeUs
) {
    if (!packetData || packetSize < net::kRnvpHeaderV1Size) {
        return;
    }

    if (net::ReadU32BE(packetData) != net::kRnvpMagic) {
        return;
    }

    net::RnvpHeaderV1 header{};
    if (!net::DecodeRnvpHeaderV1(packetData, packetSize, header)) {
        return;
    }

    if (!net::IsMediaPacket(
        static_cast<net::PacketType>(header.packetType))) {
        return;
    }

    std::lock_guard<std::mutex> lock(sentPacketsMutex_);

    SentPacketRecord record{};
    record.sequence = header.sequence;
    record.sendTimeUs = sendTimeUs;
    record.packetBytes = static_cast<uint32_t>(
        (std::min)(
            packetSize,
            static_cast<size_t>((std::numeric_limits<uint32_t>::max)())
        )
    );
    sentPackets_.push_back(record);

    while (sentPackets_.size() > kSentPacketHistoryLimit) {
        sentPackets_.pop_front();
    }
}
