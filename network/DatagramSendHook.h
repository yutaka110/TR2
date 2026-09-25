#pragma once
#include <winsock2.h>
#include <functional>
#include <cstdint>
#include <span>
namespace net {
// Install before starting traffic. Zero means explicitly rejected before send;
// SOCKET_ERROR means actual transport failure. An unset hook preserves sendto.
using DatagramSendHook=std::function<int(SOCKET,std::span<const uint8_t>,const sockaddr_in&)>;
using DatagramObserver=std::function<void(std::span<const uint8_t>)>;
}
