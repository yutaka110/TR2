#pragma once
#include <thread>
#include <vector>
#include <queue>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <Windows.h>


#include "FrameProto.h"



// ---- ヘッダ定義（32B固定）----
#pragma pack(push,1)
struct FrameHdr {
    uint8_t  magic[16];
    uint32_t payloadBytes;
    uint32_t frameId;
    uint32_t srcStride;
    uint8_t  flags;
    uint8_t  reserved[3];
};
#pragma pack(pop)
static_assert(sizeof(FrameHdr) == 32, "FrameHdr must be 32 bytes");

inline constexpr uint8_t FRAME_MAGIC16[16] =
{ 0xDE,0xAD,0xBE,0xEF,0xCA,0xFE,0xBA,0xBE,0x12,0x34,0x56,0x78,0x9A,0xBC,0xEF,0x01 };

inline constexpr size_t FRAME_HDR_LEN = sizeof(FrameHdr);

// 受信スレッドを開始（戻り値はスレッドオブジェクト）
// 呼び出し側で join / detach を管理してください
std::thread startTcpReceiverRgb32(
    std::shared_ptr<std::queue<std::vector<BYTE>>> frameQueue,
    std::shared_ptr<std::mutex> frameQueueMtx,
    std::shared_ptr<std::condition_variable> frameQueueCv,
    std::atomic<bool>& stopRequested,
    uint16_t listenPort,
    int width,
    int height
);