#pragma once

#include "FrameProto.h"

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