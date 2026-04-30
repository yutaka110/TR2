#include "utils/Checked.h"
#include "utils/StringConv.h"    // ToUtf8 を使う
#include <stdexcept>
#include <comdef.h>               // _com_error
#include <string>
#include <cstdio>

void ThrowIfFailed(HRESULT hr) {
    if (!FAILED(hr)) return;

    _com_error e(hr);
    std::string msg;

#ifdef UNICODE
    // ErrorMessage() は const wchar_t*（LPCTSTR）。UTF-8 に変換して std::string へ。
    const wchar_t* wmsg = e.ErrorMessage();
    if (wmsg && *wmsg) {
        msg = CoverString(wmsg);
    }
    else {
        msg = "HRESULT failed";
    }
#else
    // マルチバイト設定ならそのまま使える
    const char* amsg = e.ErrorMessage();
    msg = (amsg && *amsg) ? amsg : "HRESULT failed";
#endif

    // ついでに hr の 16進コードを添える
    char hex[32]{};
    std::snprintf(hex, sizeof(hex), " (hr=0x%08lX)", static_cast<unsigned long>(hr));
    msg += hex;

    throw std::runtime_error(msg);
}
