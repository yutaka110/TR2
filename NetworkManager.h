#pragma once
#define _WINSOCKAPI_ 
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

class NetworkManager {
public:
    NetworkManager(const std::string& ip, uint16_t port);
    ~NetworkManager();

    void SendUDPFragmented(const std::string& data, uint32_t frameId);
    void SendUDPFragmentedParallel(const std::string& data, uint32_t frameId, int threadCount = 16);

private:
    SOCKET udpSocket_;
    sockaddr_in udpAddr_;
};