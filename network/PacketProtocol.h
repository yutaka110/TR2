#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace net {

    // ============================================================
    // Legacy / Current Data Packet
    // ------------------------------------------------------------
    // 現在の NetworkManager / UdpReceiver / FrameReassembler が使っている
    // 既存28byteヘッダー。
    //
    // 
    // ============================================================

    static constexpr uint32_t kPacketMagic = 0x52545631; // "RTV1"

    // 既存コード互換のため 28byte のまま維持
    static constexpr size_t kPacketHeaderSize = 28;

    // UDP 1パケットに載せる最大ペイロードサイズ。
    // MTU 1500 を意識して、IP/UDPヘッダーや将来拡張の余裕を残す。
    static constexpr size_t kMaxUdpPayloadSize = 1200;

    enum PacketFlags : uint32_t {
        PacketFlag_None = 0,
        PacketFlag_KeyFrame = 1 << 0,
        PacketFlag_LastChunk = 1 << 1,

        // 将来拡張用
        PacketFlag_DroppedAllowed = 1 << 2, // 欠損時に破棄してもよいフレーム
        PacketFlag_Control = 1 << 3, // 制御系パケットを示す予備フラグ
    };

    // 現在の実通信で使う既存ヘッダー。
    // 
    // この構造体のメモリを直接送信せず、EncodeHeader() で必ずBE変換して送る。
    struct PacketHeader {
        uint32_t magic = kPacketMagic;
        uint32_t frameId = 0;
        uint16_t chunkIndex = 0;
        uint16_t chunkCount = 0;
        uint64_t sendTimeUs = 0;
        uint32_t payloadSize = 0;
        uint32_t flags = 0;
    };

    // ============================================================
    // RNVP v1 Protocol Definitions
    // ------------------------------------------------------------
    // Realtime Network Video Protocol v1
    //
    //「独自プロトコル設計」の核。
    // ただし、既存コードを壊さないため、まだ既存PacketHeaderとは
    // 完全置換しない。
    //
    // 現在の NetworkManager / UdpReceiver は RNVP v1 を主経路として使用する。
    // ============================================================

    static constexpr uint32_t kRnvpMagic =
        (static_cast<uint32_t>('R') << 24) |
        (static_cast<uint32_t>('N') << 16) |
        (static_cast<uint32_t>('V') << 8) |
        (static_cast<uint32_t>('P'));

    static constexpr uint8_t kRnvpVersion = 1;

    enum class PacketType : uint8_t {
        Data = 0, // 映像・ダミーフレーム本体
        Ack = 1, // 受信確認
        Ping = 2, // RTT計測要求
        Pong = 3, // RTT計測応答
        Control = 4, // 品質/FPS/bitrate調整指示
    };

    enum class CodecType : uint8_t {
        Unknown = 0,
        Raw = 1,
        MJPEG = 2,
        H264 = 3,
    };

    static constexpr uint32_t kRawFramePayloadMagic =
        (static_cast<uint32_t>('R') << 24) |
        (static_cast<uint32_t>('V') << 16) |
        (static_cast<uint32_t>('F') << 8) |
        static_cast<uint32_t>('1');

    static constexpr size_t kRawFramePayloadHeaderSize = 16;

    enum class RawFrameFormat : uint8_t {
        Rgba8 = 1,
    };

    struct RawFramePayloadHeader {
        uint32_t magic = kRawFramePayloadMagic;
        uint16_t width = 0;
        uint16_t height = 0;
        uint8_t format = static_cast<uint8_t>(RawFrameFormat::Rgba8);
        uint8_t reserved0 = 0;
        uint16_t reserved1 = 0;
        uint32_t payloadBytes = 0;
    };

    enum class ControlCommand : uint8_t {
        None = 0,

        // 送信品質制御
        SetJpegQuality = 1,
        SetTargetFps = 2,
        SetBitrateKbps = 3,

        // 通信制御
        RequestKeyFrame = 10,
        ResetStats = 11,
    };

    // 将来的に正式移行する RNVP v1 ヘッダー。
    //
    // 既存 PacketHeader よりプロトコル情報を明示する：
    // - version
    // - packetType
    // - headerSize
    // - sequence
    // - streamId
    // - codecType
    //
    // この構造体も直接送信せず、EncodeRnvpHeaderV1() を使う想定。
    struct RnvpHeaderV1 {
        uint32_t magic = kRnvpMagic;

        uint8_t version = kRnvpVersion;
        uint8_t packetType = static_cast<uint8_t>(PacketType::Data);
        uint16_t headerSize = 0;

        uint32_t sequence = 0;
        uint32_t streamId = 0;

        uint32_t frameId = 0;
        uint16_t chunkIndex = 0;
        uint16_t chunkCount = 0;

        uint64_t sendTimeUs = 0;

        uint32_t payloadSize = 0;
        uint32_t flags = 0;

        uint8_t codecType = static_cast<uint8_t>(CodecType::Unknown);
        uint8_t reserved0 = 0;
        uint16_t reserved1 = 0;
    };

    // RNVP v1 ヘッダーをバイト列化したときのサイズ。
    //
    // layout:
    //  0  magic        u32
    //  4  version      u8
    //  5  packetType   u8
    //  6  headerSize   u16
    //  8  sequence     u32
    // 12  streamId     u32
    // 16  frameId      u32
    // 20  chunkIndex   u16
    // 22  chunkCount   u16
    // 24  sendTimeUs   u64
    // 32  payloadSize  u32
    // 36  flags        u32
    // 40  codecType    u8
    // 41  reserved0    u8
    // 42  reserved1    u16
    static constexpr size_t kRnvpHeaderV1Size = 44;

    struct AckPayload {
        uint32_t frameId = 0;
        uint32_t receivedChunkCount = 0;
        uint32_t missingChunkCount = 0;
        uint32_t latestSequence = 0;
        std::vector<uint16_t> missingChunkIndices;
    };

    static constexpr size_t kAckPayloadBaseSize = 16;
    static constexpr size_t kAckMissingChunkIndexSize = 2;
    static constexpr size_t kMaxAckMissingChunkIndices = 512;

    struct PingPayload {
        uint64_t clientTimeUs = 0;
    };

    struct PongPayload {
        uint64_t clientTimeUs = 0;
        uint64_t serverTimeUs = 0;
    };

    struct ControlPayload {
        uint8_t command = static_cast<uint8_t>(ControlCommand::None);
        uint8_t reserved0 = 0;
        uint16_t reserved1 = 0;
        uint32_t value = 0;
    };

    // ============================================================
    // Big Endian Utility
    // ------------------------------------------------------------
    // ネットワーク上ではエンディアン差を避けるため、明示的にBEで書く。
    // ============================================================

    inline void WriteU16BE(uint8_t* dst, uint16_t value) {
        dst[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
        dst[1] = static_cast<uint8_t>(value & 0xFF);
    }

    inline void WriteU32BE(uint8_t* dst, uint32_t value) {
        dst[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
        dst[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
        dst[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        dst[3] = static_cast<uint8_t>(value & 0xFF);
    }

    inline void WriteU64BE(uint8_t* dst, uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            dst[i] = static_cast<uint8_t>((value >> (56 - i * 8)) & 0xFF);
        }
    }

    inline uint16_t ReadU16BE(const uint8_t* src) {
        return static_cast<uint16_t>(
            (static_cast<uint16_t>(src[0]) << 8) |
            static_cast<uint16_t>(src[1])
            );
    }

    inline uint32_t ReadU32BE(const uint8_t* src) {
        return (static_cast<uint32_t>(src[0]) << 24) |
            (static_cast<uint32_t>(src[1]) << 16) |
            (static_cast<uint32_t>(src[2]) << 8) |
            static_cast<uint32_t>(src[3]);
    }

    inline uint64_t ReadU64BE(const uint8_t* src) {
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value = (value << 8) | src[i];
        }
        return value;
    }

    inline void EncodeRawFramePayloadHeader(
        uint8_t* dst,
        const RawFramePayloadHeader& header
    ) {
        WriteU32BE(dst + 0, header.magic);
        WriteU16BE(dst + 4, header.width);
        WriteU16BE(dst + 6, header.height);
        dst[8] = header.format;
        dst[9] = header.reserved0;
        WriteU16BE(dst + 10, header.reserved1);
        WriteU32BE(dst + 12, header.payloadBytes);
    }

    inline bool DecodeRawFramePayloadHeader(
        const uint8_t* src,
        size_t size,
        RawFramePayloadHeader& outHeader
    ) {
        if (!src || size < kRawFramePayloadHeaderSize) {
            return false;
        }

        outHeader.magic = ReadU32BE(src + 0);
        outHeader.width = ReadU16BE(src + 4);
        outHeader.height = ReadU16BE(src + 6);
        outHeader.format = src[8];
        outHeader.reserved0 = src[9];
        outHeader.reserved1 = ReadU16BE(src + 10);
        outHeader.payloadBytes = ReadU32BE(src + 12);

        if (outHeader.magic != kRawFramePayloadMagic ||
            outHeader.width == 0 ||
            outHeader.height == 0 ||
            outHeader.format != static_cast<uint8_t>(RawFrameFormat::Rgba8)) {
            return false;
        }

        return kRawFramePayloadHeaderSize + outHeader.payloadBytes <= size;
    }

    // ============================================================
    // Current PacketHeader Encode / Decode
    // ------------------------------------------------------------
    // 既存コード用。
    // NetworkManager.cpp / FrameReassembler.cpp はこれを使う。
    // ============================================================

    inline void EncodeHeader(uint8_t* dst, const PacketHeader& header) {
        WriteU32BE(dst + 0, header.magic);
        WriteU32BE(dst + 4, header.frameId);
        WriteU16BE(dst + 8, header.chunkIndex);
        WriteU16BE(dst + 10, header.chunkCount);
        WriteU64BE(dst + 12, header.sendTimeUs);
        WriteU32BE(dst + 20, header.payloadSize);
        WriteU32BE(dst + 24, header.flags);
    }

    inline bool DecodeHeader(const uint8_t* src, size_t size, PacketHeader& outHeader) {
        if (!src) {
            return false;
        }

        if (size < kPacketHeaderSize) {
            return false;
        }

        outHeader.magic = ReadU32BE(src + 0);
        outHeader.frameId = ReadU32BE(src + 4);
        outHeader.chunkIndex = ReadU16BE(src + 8);
        outHeader.chunkCount = ReadU16BE(src + 10);
        outHeader.sendTimeUs = ReadU64BE(src + 12);
        outHeader.payloadSize = ReadU32BE(src + 20);
        outHeader.flags = ReadU32BE(src + 24);

        if (outHeader.magic != kPacketMagic) {
            return false;
        }

        if (outHeader.chunkCount == 0) {
            return false;
        }

        if (outHeader.chunkIndex >= outHeader.chunkCount) {
            return false;
        }

        if (kPacketHeaderSize + outHeader.payloadSize > size) {
            return false;
        }

        return true;
    }

    // ============================================================
    // RNVP v1 Encode / Decode
    // ------------------------------------------------------------
    // 次フェーズ以降で NetworkManager / UdpReceiver を移行するための関数。
    // 今回追加しても、既存コードには影響しない。
    // ============================================================

    inline void EncodeRnvpHeaderV1(uint8_t* dst, const RnvpHeaderV1& header) {
        WriteU32BE(dst + 0, header.magic);

        dst[4] = header.version;
        dst[5] = header.packetType;
        WriteU16BE(dst + 6, kRnvpHeaderV1Size);

        WriteU32BE(dst + 8, header.sequence);
        WriteU32BE(dst + 12, header.streamId);

        WriteU32BE(dst + 16, header.frameId);
        WriteU16BE(dst + 20, header.chunkIndex);
        WriteU16BE(dst + 22, header.chunkCount);

        WriteU64BE(dst + 24, header.sendTimeUs);

        WriteU32BE(dst + 32, header.payloadSize);
        WriteU32BE(dst + 36, header.flags);

        dst[40] = header.codecType;
        dst[41] = header.reserved0;
        WriteU16BE(dst + 42, header.reserved1);
    }

    inline bool DecodeRnvpHeaderV1(const uint8_t* src, size_t size, RnvpHeaderV1& outHeader) {
        if (!src) {
            return false;
        }

        if (size < kRnvpHeaderV1Size) {
            return false;
        }

        outHeader.magic = ReadU32BE(src + 0);
        outHeader.version = src[4];
        outHeader.packetType = src[5];
        outHeader.headerSize = ReadU16BE(src + 6);

        outHeader.sequence = ReadU32BE(src + 8);
        outHeader.streamId = ReadU32BE(src + 12);

        outHeader.frameId = ReadU32BE(src + 16);
        outHeader.chunkIndex = ReadU16BE(src + 20);
        outHeader.chunkCount = ReadU16BE(src + 22);

        outHeader.sendTimeUs = ReadU64BE(src + 24);

        outHeader.payloadSize = ReadU32BE(src + 32);
        outHeader.flags = ReadU32BE(src + 36);

        outHeader.codecType = src[40];
        outHeader.reserved0 = src[41];
        outHeader.reserved1 = ReadU16BE(src + 42);

        if (outHeader.magic != kRnvpMagic) {
            return false;
        }

        if (outHeader.version != kRnvpVersion) {
            return false;
        }

        if (outHeader.headerSize != kRnvpHeaderV1Size) {
            return false;
        }

        const PacketType type = static_cast<PacketType>(outHeader.packetType);

        if (type == PacketType::Data) {
            if (outHeader.chunkCount == 0) {
                return false;
            }

            if (outHeader.chunkIndex >= outHeader.chunkCount) {
                return false;
            }
        }

        if (outHeader.headerSize + outHeader.payloadSize > size) {
            return false;
        }

        return true;
    }

    // ============================================================
    // Payload Encode / Decode Helpers
    // ------------------------------------------------------------
    // ACK / Ping / Pong / Control 用。
    // 次フェーズで実際の送受信に使う。
    // ============================================================

    inline size_t CalculateAckPayloadSize(const AckPayload& payload) {
        return kAckPayloadBaseSize +
            payload.missingChunkIndices.size() * kAckMissingChunkIndexSize;
    }

    inline void EncodeAckPayload(uint8_t* dst, const AckPayload& payload) {
        WriteU32BE(dst + 0, payload.frameId);
        WriteU32BE(dst + 4, payload.receivedChunkCount);
        WriteU32BE(dst + 8, payload.missingChunkCount);
        WriteU32BE(dst + 12, payload.latestSequence);

        uint8_t* cursor = dst + kAckPayloadBaseSize;
        for (uint16_t chunkIndex : payload.missingChunkIndices) {
            WriteU16BE(cursor, chunkIndex);
            cursor += kAckMissingChunkIndexSize;
        }
    }

    inline bool DecodeAckPayload(const uint8_t* src, size_t size, AckPayload& outPayload) {
        if (!src || size < kAckPayloadBaseSize) {
            return false;
        }

        outPayload.frameId = ReadU32BE(src + 0);
        outPayload.receivedChunkCount = ReadU32BE(src + 4);
        outPayload.missingChunkCount = ReadU32BE(src + 8);
        outPayload.latestSequence = ReadU32BE(src + 12);
        outPayload.missingChunkIndices.clear();

        const size_t availableIndexBytes = size - kAckPayloadBaseSize;
        const size_t encodedIndexCount =
            availableIndexBytes / kAckMissingChunkIndexSize;
        const size_t indexCount = (std::min)(
            encodedIndexCount,
            static_cast<size_t>(outPayload.missingChunkCount)
        );

        outPayload.missingChunkIndices.reserve(indexCount);

        const uint8_t* cursor = src + kAckPayloadBaseSize;
        for (size_t i = 0; i < indexCount; ++i) {
            outPayload.missingChunkIndices.push_back(ReadU16BE(cursor));
            cursor += kAckMissingChunkIndexSize;
        }

        return true;
    }

    inline void EncodePingPayload(uint8_t* dst, const PingPayload& payload) {
        WriteU64BE(dst + 0, payload.clientTimeUs);
    }

    inline bool DecodePingPayload(const uint8_t* src, size_t size, PingPayload& outPayload) {
        if (!src || size < 8) {
            return false;
        }

        outPayload.clientTimeUs = ReadU64BE(src + 0);
        return true;
    }

    inline void EncodePongPayload(uint8_t* dst, const PongPayload& payload) {
        WriteU64BE(dst + 0, payload.clientTimeUs);
        WriteU64BE(dst + 8, payload.serverTimeUs);
    }

    inline bool DecodePongPayload(const uint8_t* src, size_t size, PongPayload& outPayload) {
        if (!src || size < 16) {
            return false;
        }

        outPayload.clientTimeUs = ReadU64BE(src + 0);
        outPayload.serverTimeUs = ReadU64BE(src + 8);

        return true;
    }

    inline void EncodeControlPayload(uint8_t* dst, const ControlPayload& payload) {
        dst[0] = payload.command;
        dst[1] = payload.reserved0;
        WriteU16BE(dst + 2, payload.reserved1);
        WriteU32BE(dst + 4, payload.value);
    }

    inline bool DecodeControlPayload(const uint8_t* src, size_t size, ControlPayload& outPayload) {
        if (!src || size < 8) {
            return false;
        }

        outPayload.command = src[0];
        outPayload.reserved0 = src[1];
        outPayload.reserved1 = ReadU16BE(src + 2);
        outPayload.value = ReadU32BE(src + 4);

        return true;
    }

    // ============================================================
    // Utility
    // ============================================================

    inline bool HasPacketFlag(uint32_t flags, PacketFlags flag) {
        return (flags & static_cast<uint32_t>(flag)) != 0;
    }

    inline uint32_t AddPacketFlag(uint32_t flags, PacketFlags flag) {
        return flags | static_cast<uint32_t>(flag);
    }

    inline uint32_t RemovePacketFlag(uint32_t flags, PacketFlags flag) {
        return flags & ~static_cast<uint32_t>(flag);
    }

    inline bool IsDataPacket(PacketType type) {
        return type == PacketType::Data;
    }

    inline bool IsControlPacket(PacketType type) {
        return type == PacketType::Ack ||
            type == PacketType::Ping ||
            type == PacketType::Pong ||
            type == PacketType::Control;
    }

} // namespace net
