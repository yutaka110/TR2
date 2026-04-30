#pragma once
#include <mfobjects.h>
#include <mfidl.h>
#include <deque>
#include <mutex>
#include <wrl.h>
#include <vector>


//class CustomByteStream : public IMFByteStream {
//public:
//    CustomByteStream();
//    ~CustomByteStream();
//
//    // 書き込み用API
//    void PushData(const uint8_t* data, size_t size);
//    bool HasSPS() const;
//
//    // IUnknown
//    STDMETHOD(QueryInterface)(REFIID riid, void** ppv);
//    STDMETHOD_(ULONG, AddRef)();
//    STDMETHOD_(ULONG, Release)();
//
//    // IMFByteStream
//    STDMETHOD(GetCapabilities)(DWORD* pdwCapabilities);
//    STDMETHOD(GetLength)(QWORD* pqwLength);
//    STDMETHOD(SetLength)(QWORD qwLength);
//    STDMETHOD(GetCurrentPosition)(QWORD* pqwPosition);
//    STDMETHOD(SetCurrentPosition)(QWORD qwPosition);
//    STDMETHOD(IsEndOfStream)(BOOL* pfEndOfStream);
//    STDMETHOD(Read)(BYTE* pb, ULONG cb, ULONG* pcbRead);
//    STDMETHOD(BeginRead)(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* punkState);
//    STDMETHOD(EndRead)(IMFAsyncResult* pResult, ULONG* pcbRead);
//    STDMETHOD(Write)(const BYTE*, ULONG, ULONG*) { return E_NOTIMPL; }
//    STDMETHOD(BeginWrite)(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) { return E_NOTIMPL; }
//    STDMETHOD(EndWrite)(IMFAsyncResult*, ULONG*) { return E_NOTIMPL; }
//    STDMETHOD(Seek)(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset, DWORD dwSeekFlags, QWORD* pqwCurrentPosition);
//    STDMETHOD(Flush)() { return S_OK; }
//    STDMETHOD(Close)() { return S_OK; }
//
//private:
//    LONG refCount_;
//    std::vector<BYTE> buffer_;
//    size_t position_;
//    bool hasSPS_;
//    mutable std::mutex mutex_;
//};


class CustomByteStream : public IMFByteStream {
public:
    CustomByteStream();
    ~CustomByteStream();

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
    STDMETHODIMP BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndRead(IMFAsyncResult* pResult, ULONG* pcbRead) override;
    STDMETHODIMP Write(const BYTE* pb, ULONG cb, ULONG* pcbWritten) override;
    STDMETHODIMP BeginWrite(const BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndWrite(IMFAsyncResult* pResult, ULONG* pcbWritten) override;
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset, DWORD dwSeekFlags, QWORD* pqwCurrentPosition) override;
    STDMETHODIMP Flush() override;
    STDMETHODIMP Close() override;

    // 外部からの受信データ追加
    void PushData(const uint8_t* data, size_t size);
    bool HasSPS() const { return hasSPS_; }

private:
    std::atomic<ULONG> refCount_;
    std::deque<uint8_t> buffer_;
    QWORD position_;
    std::mutex mutex_;
    std::atomic<bool> hasSPS_ = false;
};
