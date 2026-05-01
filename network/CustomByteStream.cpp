#include "CustomByteStream.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mferror.h>
#include <memory>
#include <condition_variable>
#include <strmif.h>
#include <cstring>
//
CustomByteStream::CustomByteStream() : refCount_(1), position_(0) {}
CustomByteStream::~CustomByteStream() {}

ULONG CustomByteStream::AddRef() {
    return ++refCount_;
}

ULONG CustomByteStream::Release() {
    ULONG count = --refCount_;
    if (count == 0) delete this;
    return count;
}

//HRESULT CustomByteStream::QueryInterface(REFIID riid, void** ppv) {
//
//    if (!ppv) return E_POINTER;
//    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFByteStream)) {
//        *ppv = static_cast<IMFByteStream*>(this);
//        AddRef();
//        return S_OK;
//    }
//    *ppv = nullptr;
//    return E_NOINTERFACE;
//}

HRESULT CustomByteStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        OutputDebugStringA("[ERROR] QueryInterface: ppv is null\n");
        return E_POINTER;
    }

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFByteStream)) {
        *ppv = static_cast<IMFByteStream*>(this);
        AddRef();
        OutputDebugStringA("[DEBUG] QueryInterface: returning IMFByteStream\n");
        return S_OK;
    }

    if (riid == __uuidof(IMFAttributes)) {
        IMFAttributes* dummy = nullptr;
        HRESULT hr = MFCreateAttributes(&dummy, 0);
        if (SUCCEEDED(hr)) {
            *ppv = dummy;
            OutputDebugStringA("[DEBUG] IMFAttributes dummy returned\n");
            return S_OK;
        }
        else {
            *ppv = nullptr;
            return hr;
        }
    }

    // ★ログを強化して、実際に何を要求されたか知る
    LPOLESTR guidString = nullptr;
    StringFromIID(riid, &guidString);
    if (guidString) {
        char buf[256];
        sprintf_s(buf, "[WARN] QueryInterface: Unsupported IID = %ws\n", guidString);
        OutputDebugStringA(buf);
        CoTaskMemFree(guidString);
    }

    *ppv = nullptr;
    //OutputDebugStringA("[WARN] QueryInterface: unsupported interface requested\n");
    return E_NOINTERFACE;
}


HRESULT CustomByteStream::GetCapabilities(DWORD* pdwCapabilities) {

    if (!pdwCapabilities) return E_POINTER;
    *pdwCapabilities = MFBYTESTREAM_IS_READABLE;
    OutputDebugStringA("[DEBUG] GetCapabilities called\n");
    return S_OK;
}

HRESULT CustomByteStream::GetLength(QWORD* pqwLength) {
    if (!pqwLength) {
        OutputDebugStringA("[ERROR] GetLength: pqwLength is null\n");
        return E_POINTER;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    *pqwLength = buffer_.size();

    char log[128];
    sprintf_s(log, "[DEBUG] GetLength called, size = %llu\n", *pqwLength);
    OutputDebugStringA(log);

    return S_OK;
}

HRESULT CustomByteStream::SetLength(QWORD) {
    return E_NOTIMPL; }

//HRESULT CustomByteStream::GetCurrentPosition(QWORD* pqwPosition) {
//
//    if (!pqwPosition) return E_POINTER;
//    *pqwPosition = position_;
//    return S_OK;
//}

HRESULT CustomByteStream::GetCurrentPosition(QWORD* pqwPosition) {
    if (!pqwPosition) {
        OutputDebugStringA("[ERROR] GetCurrentPosition: pqwPosition is null\n");
        return E_POINTER;
    }

    *pqwPosition = position_;

    char log[128];
    sprintf_s(log, "[DEBUG] GetCurrentPosition called, pos = %llu\n", position_);
    OutputDebugStringA(log);

    return S_OK;
}


HRESULT CustomByteStream::SetCurrentPosition(QWORD qwPosition) {

    std::lock_guard<std::mutex> lock(mutex_);
    position_ = qwPosition;
    return S_OK;
}

HRESULT CustomByteStream::IsEndOfStream(BOOL* pfEndOfStream) {
    if (!pfEndOfStream) {
        OutputDebugStringA("[ERROR] IsEndOfStream: pfEndOfStream is null\n");
        return E_POINTER;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    *pfEndOfStream = buffer_.empty();

    char log[128];
    sprintf_s(log, "[DEBUG] IsEndOfStream called, result = %d\n", *pfEndOfStream);
    OutputDebugStringA(log);

    return S_OK;
}

HRESULT CustomByteStream::Read(BYTE* pb, ULONG cb, ULONG* pcbRead) {
    if (!pb || !pcbRead) {
        return E_POINTER;
    }

    if (buffer_.empty()) {
        *pcbRead = 0;
        return S_FALSE; // データがまだ届いていないことを示す
    }

    std::lock_guard<std::mutex> lock(mutex_);
    size_t available = buffer_.size();
    size_t toRead = (std::min)(size_t(cb), available);
    for (size_t i = 0; i < toRead; ++i) {
        pb[i] = buffer_.front();
        buffer_.pop_front();
    }
    position_ += toRead;
    *pcbRead = static_cast<ULONG>(toRead);
    return S_OK;
}

HRESULT CustomByteStream::BeginRead(BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) {
    return E_NOTIMPL;
}

HRESULT CustomByteStream::EndRead(IMFAsyncResult*, ULONG*) {
    return E_NOTIMPL;
}

HRESULT CustomByteStream::Write(const BYTE*, ULONG, ULONG*) {
    return E_NOTIMPL;
}

HRESULT CustomByteStream::BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) {
    return E_NOTIMPL;
}

HRESULT CustomByteStream::EndWrite(IMFAsyncResult*, ULONG*) {
    return E_NOTIMPL;
}

HRESULT CustomByteStream::Seek(MFBYTESTREAM_SEEK_ORIGIN, LONGLONG, DWORD, QWORD*) {
    OutputDebugStringA("[DEBUG] Seek called\n");
    return E_NOTIMPL;
}

HRESULT CustomByteStream::Flush() {

    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
    return S_OK;
}

HRESULT CustomByteStream::Close() {
    return S_OK;
}

void CustomByteStream::PushData(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.insert(buffer_.end(), data, data + size);

    // SPS (NAL type 7) の検出：00 00 00 01 67  もしくは 00 00 01 67
    for (size_t i = 0; i + 4 < buffer_.size(); ++i) {
        if ((buffer_[i] == 0x00 && buffer_[i + 1] == 0x00 &&
            buffer_[i + 2] == 0x00 && buffer_[i + 3] == 0x01 &&
            (buffer_[i + 4] & 0x1F) == 7) ||
            (buffer_[i] == 0x00 && buffer_[i + 1] == 0x00 &&
                buffer_[i + 2] == 0x01 && (buffer_[i + 3] & 0x1F) == 7)) {

            if (!hasSPS_) {
                OutputDebugStringA("[INFO] SPS detected in PushData\n");

                // 先頭数バイトも表示
                if (buffer_.size() >= 10) {
                    char log[128];
                    sprintf_s(log, "[DEBUG] Buffer head: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                        buffer_[0], buffer_[1], buffer_[2], buffer_[3],
                        buffer_[4], buffer_[5], buffer_[6], buffer_[7]);
                    OutputDebugStringA(log);
                }
            }

            hasSPS_ = true;
            break;
        }
    }
}




// CustomByteStream.cpp



//CustomByteStream::CustomByteStream()
//    : refCount_(1), position_(0), hasSPS_(false) {
//}
//
//CustomByteStream::~CustomByteStream() {}
//
//void CustomByteStream::PushData(const uint8_t* data, size_t size) {
//    std::lock_guard<std::mutex> lock(mutex_);
//    buffer_.insert(buffer_.end(), data, data + size);
//
//    // SPS検出処理（省略可）
//    if (!hasSPS_) {
//        for (size_t i = 0; i + 4 < size; ++i) {
//            if ((data[i] == 0x00 && data[i + 1] == 0x00 &&
//                data[i + 2] == 0x00 && data[i + 3] == 0x01 &&
//                (data[i + 4] & 0x1F) == 7)) {
//                hasSPS_ = true;
//                OutputDebugStringA("[INFO] SPS detected (2-arg PushData)\n");
//                break;
//            }
//        }
//    }
//}
//
//bool CustomByteStream::HasSPS() const {
//    std::lock_guard<std::mutex> lock(mutex_);
//    return hasSPS_;
//}
//
//// -------------------------
//// IUnknown
//// -------------------------
//STDMETHODIMP CustomByteStream::QueryInterface(REFIID riid, void** ppv) {
//    if (!ppv) return E_POINTER;
//    if (riid == IID_IUnknown || riid == IID_IMFByteStream) {
//        *ppv = static_cast<IMFByteStream*>(this);
//        AddRef();
//        return S_OK;
//    }
//    *ppv = nullptr;
//    return E_NOINTERFACE;
//}
//
//STDMETHODIMP_(ULONG) CustomByteStream::AddRef() {
//    return InterlockedIncrement(&refCount_);
//}
//
//STDMETHODIMP_(ULONG) CustomByteStream::Release() {
//    ULONG count = InterlockedDecrement(&refCount_);
//    if (count == 0) delete this;
//    return count;
//}
//
//// -------------------------
//// IMFByteStream
//// -------------------------
//
//STDMETHODIMP CustomByteStream::GetCapabilities(DWORD* pdwCapabilities) {
//    if (!pdwCapabilities) return E_POINTER;
//    *pdwCapabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;
//    return S_OK;
//}
//
//STDMETHODIMP CustomByteStream::GetLength(QWORD* pqwLength) {
//    if (!pqwLength) return E_POINTER;
//    std::lock_guard<std::mutex> lock(mutex_);
//    *pqwLength = static_cast<QWORD>(buffer_.size());
//    return S_OK;
//}
//
//STDMETHODIMP CustomByteStream::SetLength(QWORD) {
//    return E_NOTIMPL;
//}
//
//STDMETHODIMP CustomByteStream::GetCurrentPosition(QWORD* pqwPosition) {
//    if (!pqwPosition) return E_POINTER;
//    std::lock_guard<std::mutex> lock(mutex_);
//    *pqwPosition = static_cast<QWORD>(position_);
//    return S_OK;
//}
//
//STDMETHODIMP CustomByteStream::SetCurrentPosition(QWORD qwPosition) {
//    std::lock_guard<std::mutex> lock(mutex_);
//    if (qwPosition > buffer_.size()) return E_INVALIDARG;
//    position_ = static_cast<size_t>(qwPosition);
//    return S_OK;
//}
//
//STDMETHODIMP CustomByteStream::IsEndOfStream(BOOL* pfEndOfStream) {
//    if (!pfEndOfStream) return E_POINTER;
//    std::lock_guard<std::mutex> lock(mutex_);
//    *pfEndOfStream = (position_ >= buffer_.size());
//    return S_OK;
//}
//
//STDMETHODIMP CustomByteStream::Read(BYTE* pb, ULONG cb, ULONG* pcbRead) {
//    if (!pb || !pcbRead) return E_POINTER;
//    std::lock_guard<std::mutex> lock(mutex_);
//    size_t remain = buffer_.size() - position_;
//    ULONG toRead = (cb < remain) ? cb : static_cast<ULONG>(remain);
//    if (toRead > 0) {
//        memcpy(pb, buffer_.data() + position_, toRead);
//        position_ += toRead;
//        *pcbRead = toRead;
//        return S_OK;
//    }else {
//        *pcbRead = 0;
//            return S_FALSE; // データ未到達の合図
//    }
//   
//}
//
//STDMETHODIMP CustomByteStream::BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* punkState) {
//    // 同期で実行
//    ULONG bytesRead = 0;
//    HRESULT hr = Read(pb, cb, &bytesRead);
//    IMFAsyncResult* pResult = nullptr;
//    HRESULT hr2=MFCreateAsyncResult(nullptr, pCallback, punkState, &pResult);
//    if (FAILED(hr2)) return hr2;
//    pCallback->Invoke(pResult);
//    if (pResult) pResult->Release();
//    return S_OK;
//}
//
//STDMETHODIMP CustomByteStream::EndRead(IMFAsyncResult*, ULONG* pcbRead) {
//    if (!pcbRead) return E_POINTER;
//    *pcbRead = 0; // 同期済みなので 0 にする
//    return S_OK;
//}
//
//STDMETHODIMP CustomByteStream::Seek(
//    MFBYTESTREAM_SEEK_ORIGIN SeekOrigin,
//    LONGLONG llSeekOffset,
//    DWORD /*dwSeekFlags*/,
//    QWORD* pqwCurrentPosition
//) {
//    std::lock_guard<std::mutex> lock(mutex_);
//
//    LONGLONG newPos = 0;
//    if (SeekOrigin == msoBegin) {
//        newPos = llSeekOffset;
//    }
//    else if (SeekOrigin == msoCurrent) {
//        newPos = static_cast<LONGLONG>(position_) + llSeekOffset;
//    }
//    else {
//        return E_INVALIDARG;
//    }
//
//    if (newPos < 0 || static_cast<size_t>(newPos) > buffer_.size())
//        return STG_E_INVALIDFUNCTION;
//
//    position_ = static_cast<size_t>(newPos);
//    if (pqwCurrentPosition) *pqwCurrentPosition = position_;
//    return S_OK;
//}
