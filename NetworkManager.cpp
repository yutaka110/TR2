#include "NetworkManager.h"
#include <iostream>
#include <cstring>

NetworkManager::NetworkManager(const std::string& ip, uint16_t port) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    udpSocket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket_ == INVALID_SOCKET) {
        std::cerr << "[UDP Error] Failed to create socket: " << WSAGetLastError() << "\n";
        return;
    }

    udpAddr_.sin_family = AF_INET;
    udpAddr_.sin_port = htons(port);
    InetPtonA(AF_INET, ip.c_str(), &udpAddr_.sin_addr);
}

NetworkManager::~NetworkManager() {
    closesocket(udpSocket_);
    WSACleanup();
}

void NetworkManager::SendUDPFragmented(const std::string& data, uint32_t frameId) {
    const size_t maxPayload = 1400;  // MTUより小さく
    size_t totalSize = data.size();
    uint16_t chunkCount = static_cast<uint16_t>((totalSize + maxPayload - 1) / maxPayload);

    for (uint16_t i = 0; i < chunkCount; ++i) {
        size_t offset = i * maxPayload;
        size_t size = (std::min)(maxPayload, totalSize - offset);

        std::string packet(8, '\0');  // ヘッダ8バイト
        packet.append(data.substr(offset, size));

        uint32_t netFrameId = htonl(frameId);
        uint16_t netChunkIdx = htons(i);
        uint16_t netChunkCnt = htons(chunkCount);

        std::memcpy(&packet[0], &netFrameId, 4);
        std::memcpy(&packet[4], &netChunkIdx, 2);
        std::memcpy(&packet[6], &netChunkCnt, 2);

        sendto(udpSocket_, packet.data(), static_cast<int>(packet.size()), 0,
            reinterpret_cast<sockaddr*>(&udpAddr_), sizeof(udpAddr_));
    }
}

void NetworkManager::SendUDPFragmentedParallel(const std::string& data, uint32_t frameId, int threadCount) {
    const size_t maxPayload = 1400;
    size_t totalSize = data.size();
    size_t totalChunks = (totalSize + maxPayload - 1) / maxPayload;

    std::vector<std::thread> threads;
    size_t chunksPerThread = (totalChunks + threadCount - 1) / threadCount;

    for (int t = 0; t < threadCount; ++t) {
        size_t begin = t * chunksPerThread;
        size_t end = (std::min)(begin + chunksPerThread, totalChunks);
        if (begin >= end) continue;

        threads.emplace_back([=]() {
            for (size_t i = begin; i < end; ++i) {
                size_t offset = i * maxPayload;
                size_t size = (std::min)(maxPayload, totalSize - offset);

                std::vector<uint8_t> packet(8 + size);
                packet[0] = (frameId >> 24) & 0xFF;
                packet[1] = (frameId >> 16) & 0xFF;
                packet[2] = (frameId >> 8) & 0xFF;
                packet[3] = (frameId >> 0) & 0xFF;

                packet[4] = (i >> 8) & 0xFF;
                packet[5] = (i >> 0) & 0xFF;

                packet[6] = (totalChunks >> 8) & 0xFF;
                packet[7] = (totalChunks >> 0) & 0xFF;

                std::memcpy(packet.data() + 8, data.data() + offset, size);

                sendto(udpSocket_,
                    reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()),
                    0, reinterpret_cast<const sockaddr*>(&udpAddr_), sizeof(udpAddr_));
            }
            });
    }

    for (auto& thread : threads) {
        if (thread.joinable()) thread.join();
    }
}
