#include "utils/StringConv.h"
#include <Windows.h>

std::wstring CoverString(const std::string& s) {
    if (s.empty()) return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
    out.resize(len ? len - 1 : 0);
    return out;
}

std::string CoverString(const std::wstring& ws) {
    if (ws.empty()) return "";
    const int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
    out.resize(len ? len - 1 : 0);
    return out;
}
