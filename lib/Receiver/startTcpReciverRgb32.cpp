
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>    // ← winsock2は必ず windows.h より先
#include <ws2tcpip.h>
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdarg>

#include "startTcpReceiverRgb32.h"
#pragma comment(lib, "Ws2_32.lib")

std::thread startTcpReceiverRgb32(
    std::shared_ptr<std::queue<std::vector<BYTE>>> frameQueue,
    std::shared_ptr<std::mutex> frameQueueMtx,
    std::shared_ptr<std::condition_variable> frameQueueCv,
    std::atomic<bool>& stopRequested,
    uint16_t listenPort,
    int width,
    int height
) {
    return std::thread([=, &stopRequested]() {
        auto dbg = [](const char* s) { OutputDebugStringA(s); };

        // ---- 定数 ----
        constexpr size_t bytesPerPixel = 4;
        constexpr size_t maxFrameBytes = (1ull << 30);
        constexpr DWORD  recvTimeoutMs = 2000;
        constexpr BOOL   enable = TRUE;

        if (width <= 0 || height <= 0) { dbg("[ERROR] Invalid width/height\n"); return; }
        const size_t expectedBytes = size_t(width) * size_t(height) * bytesPerPixel;
        if (expectedBytes == 0 || expectedBytes > maxFrameBytes) {
            dbg("[ERROR] expectedBytes is invalid or too large\n"); return;
        }

        // ---- ソケット ----
        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) { dbg("[ERROR] TCP socket creation failed\n"); return; }
        setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enable), sizeof(enable));

        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(listenPort); addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            dbg("[ERROR] bind() failed\n"); closesocket(listenSock); return;
        }
        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
            dbg("[ERROR] listen() failed\n"); closesocket(listenSock); return;
        }
        dbg("[INFO] Waiting for TCP client...\n");

        SOCKET clientSock = accept(listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) { dbg("[ERROR] accept() failed\n"); closesocket(listenSock); return; }
        closesocket(listenSock);
        dbg("[INFO] TCP client connected\n");

        setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enable), sizeof(enable));
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeoutMs), sizeof(recvTimeoutMs));

        // ---- 受信バッファ＆ヘルパ ----
        std::vector<BYTE> sbuf; sbuf.reserve(expectedBytes * 2 + FRAME_HDR_LEN);

        auto logf = [](const char* fmt, ...) {
            char buf[256]; va_list ap; va_start(ap, fmt);
            vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
            OutputDebugStringA(buf);
            };
        auto find_magic = [&](const uint8_t* p, size_t n)->ptrdiff_t {
            if (!p || n < 16) return -1;
            for (size_t i = 0; i + 16 <= n; ++i)
                if (memcmp(p + i, FRAME_MAGIC16, 16) == 0) return (ptrdiff_t)i;
            return -1;
            };

        // ---- 受信ループ ----
        while (!stopRequested) {
            BYTE tmp[128 * 1024];
            int ret = recv(clientSock, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
            if (ret == 0) { dbg("[INFO] TCP connection closed by peer\n"); break; }
            if (ret < 0) {
                const int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) { if (stopRequested) break; continue; }
                dbg("[ERROR] recv() failed\n"); break;
            }
            sbuf.insert(sbuf.end(), tmp, tmp + ret);

            for (;;) {
                if (stopRequested) break;

                // 1) MAGIC 同期
                ptrdiff_t mpos = find_magic(sbuf.data(), sbuf.size());
                if (mpos < 0) {
                    if (sbuf.size() > (expectedBytes + FRAME_HDR_LEN) * 2) {
                        const size_t keep = expectedBytes + FRAME_HDR_LEN;
                        sbuf.erase(sbuf.begin(), sbuf.end() - (ptrdiff_t)keep);
                        logf("[TRIM] sbuf trimmed, now=%zu\n", sbuf.size());
                    }
                    break;
                }
                if (mpos > 0) sbuf.erase(sbuf.begin(), sbuf.begin() + mpos);

                // 2) ヘッダ待ち
                if (sbuf.size() < FRAME_HDR_LEN) break;

                // 3) ヘッダ検証
                FrameHdr hdr{}; memcpy(&hdr, sbuf.data(), FRAME_HDR_LEN);
                if (memcmp(hdr.magic, FRAME_MAGIC16, 16) != 0) { sbuf.erase(sbuf.begin()); continue; }

                const size_t pay = (size_t)hdr.payloadBytes;
                if (pay == 0 || pay > maxFrameBytes) { sbuf.erase(sbuf.begin()); logf("[WARN] invalid payloadBytes=%zu\n", pay); continue; }

#ifndef NDEBUG
                if (hdr.srcStride != (uint32_t)width * 4) {
                    char m[128]; sprintf_s(m, "[RXWARN] srcStride=%u expected=%u\n", hdr.srcStride, width * 4); OutputDebugStringA(m);
                }
#endif
                const size_t need = FRAME_HDR_LEN + pay;
                if (sbuf.size() < need) break;

                // 4) ペイロード抽出
                std::vector<BYTE> frame(
                    sbuf.begin() + (ptrdiff_t)FRAME_HDR_LEN,
                    sbuf.begin() + (ptrdiff_t)need
                );
                sbuf.erase(sbuf.begin(), sbuf.begin() + (ptrdiff_t)need);

#ifndef NDEBUG
                static int s_dump = 0;
                if (s_dump < 8) {
                    size_t z = 0, nz = 0; for (size_t i = 3; i < frame.size(); i += 4) (frame[i] == 0) ? ++z : ++nz;
                    char head[16 * 3 + 1] = {}; int p = 0;
                    for (int i = 0; i < 16 && i < (int)frame.size(); ++i) p += sprintf_s(head + p, sizeof(head) - p, "%02X ", frame[i]);
                    char m[256];
                    sprintf_s(m, "[RX] id=%u pay=%zu stride=%u flags=0x%02X head16:%s | A0=%zu A!=0=%zu\n",
                        hdr.frameId, pay, hdr.srcStride, hdr.flags, head, z, nz);
                    OutputDebugStringA(m); ++s_dump;
                }
#endif
                // 5) キュー投入（過密ならドロップ）
                {
                    std::lock_guard<std::mutex> lock(*frameQueueMtx);
                    constexpr size_t kMaxQueued = 3;
                    while (frameQueue->size() >= kMaxQueued) frameQueue->pop();
                    frameQueue->push(std::move(frame));
                }
                frameQueueCv->notify_one();
            }
        }

        shutdown(clientSock, SD_BOTH);
        closesocket(clientSock);
        dbg("[INFO] TCP receiver thread exited\n");
        });
}
