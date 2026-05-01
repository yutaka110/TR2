#define _WINSOCKAPI_    // winsock.h を抑制
#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "UdpByteStream.h"
#include <cassert>
#include <iostream>

UdpByteStream::UdpByteStream(const std::string& ip, uint16_t port)
    : refCount_(1), position_(0)
{
    // WinSock 初期化（アプリで一度だけ必要）
    static bool initialized = false;
    if (!initialized) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        initialized = true;
    }

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(sock_ != INVALID_SOCKET);

    remoteAddr_.sin_family = AF_INET;
    remoteAddr_.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &remoteAddr_.sin_addr);
}

UdpByteStream::~UdpByteStream() {
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
    }
}

ULONG UdpByteStream::AddRef() {
    return ++refCount_;
}

ULONG UdpByteStream::Release() {
    ULONG count = --refCount_;
    if (count == 0) delete this;
    return count;
}

HRESULT UdpByteStream::QueryInterface(REFIID riid, void** ppv) {

    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFByteStream)) {
        *ppv = static_cast<IMFByteStream*>(this);
        AddRef();
        OutputDebugStringA("QueryInterface: IMFByteStream OK\n");
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

HRESULT UdpByteStream::GetCapabilities(DWORD* pdwCapabilities) {

    if (!pdwCapabilities) return E_POINTER;
    //*pdwCapabilities = MFBYTESTREAM_IS_WRITABLE;
    *pdwCapabilities = MFBYTESTREAM_IS_WRITABLE | MFBYTESTREAM_IS_SEEKABLE | MFBYTESTREAM_DOES_NOT_USE_NETWORK;

    return S_OK;
}

HRESULT UdpByteStream::GetLength(QWORD* pqwLength) {

    if (!pqwLength) return E_POINTER;
    *pqwLength = 0;
    return S_OK;
}

HRESULT UdpByteStream::SetLength(QWORD) {

    //return E_NOTIMPL;
    return S_OK;
}

HRESULT UdpByteStream::GetCurrentPosition(QWORD* pqwPosition) {

    if (!pqwPosition) return E_POINTER;
    *pqwPosition = position_;
    return S_OK;
}

HRESULT UdpByteStream::SetCurrentPosition(QWORD qwPosition) {

    position_ = qwPosition;
    return S_OK;
}

HRESULT UdpByteStream::IsEndOfStream(BOOL* pfEndOfStream) {

    if (!pfEndOfStream) return E_POINTER;
    *pfEndOfStream = FALSE;
    return S_OK;
}

HRESULT UdpByteStream::Read(BYTE*, ULONG, ULONG*) {
    return E_NOTIMPL;
}

HRESULT UdpByteStream::BeginRead(BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) {
    return E_NOTIMPL;
}

HRESULT UdpByteStream::EndRead(IMFAsyncResult*, ULONG*) {
    return E_NOTIMPL;
}

HRESULT UdpByteStream::Write(const BYTE* pb, ULONG cb, ULONG* pcbWritten) {
    if (!pb || !pcbWritten) return E_POINTER;

    int sent = sendto(sock_, reinterpret_cast<const char*>(pb), cb, 0,
        reinterpret_cast<sockaddr*>(&remoteAddr_), sizeof(remoteAddr_));

    if (sent == SOCKET_ERROR) {
        *pcbWritten = 0;
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    position_ += sent;
    *pcbWritten = static_cast<ULONG>(sent);
    return S_OK;
}

HRESULT UdpByteStream::BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) {
    return E_NOTIMPL;
}

HRESULT UdpByteStream::EndWrite(IMFAsyncResult*, ULONG*) {
    return E_NOTIMPL;
}

HRESULT UdpByteStream::Seek(MFBYTESTREAM_SEEK_ORIGIN, LONGLONG, DWORD, QWORD*) {
    return E_NOTIMPL;
}

HRESULT UdpByteStream::Flush() {
    return S_OK;
}

HRESULT UdpByteStream::Close() {
    return S_OK;
}
