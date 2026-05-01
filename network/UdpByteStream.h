#pragma once

#include <mfobjects.h>
#include <wrl.h>
#include <string>
#include <atomic>
#pragma comment(lib, "ws2_32.lib")

class UdpByteStream : public IMFByteStream {
public:
    UdpByteStream(const std::string& ip, uint16_t port);
    ~UdpByteStream();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFByteStream
    STDMETHODIMP GetCapabilities(DWORD* pdwCapabilities) override;
    STDMETHODIMP GetLength(QWORD* pqwLength) override;
    STDMETHODIMP SetLength(QWORD qwLength) override;
    STDMETHODIMP GetCurrentPosition(QWORD* pqwPosition) override;
    STDMETHODIMP SetCurrentPosition(QWORD qwPosition) override;
    STDMETHODIMP IsEndOfStream(BOOL* pfEndOfStream) override;
    STDMETHODIMP Read(BYTE* pb, ULONG cb, ULONG* pcbRead) override;
    STDMETHODIMP BeginRead(BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override;
    STDMETHODIMP EndRead(IMFAsyncResult*, ULONG*) override;
    STDMETHODIMP Write(const BYTE* pb, ULONG cb, ULONG* pcbWritten) override;
    STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override;
    STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) override;
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN, LONGLONG, DWORD, QWORD*) override;
    STDMETHODIMP Flush() override;
    STDMETHODIMP Close() override;

private:
    std::atomic<ULONG> refCount_;
    QWORD position_;
    SOCKET sock_;
    sockaddr_in remoteAddr_;
};
