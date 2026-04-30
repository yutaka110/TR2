#include "AppRuntimeUtils.h"
#include <fstream>

std::wstring ConvertString(const std::string& str) {
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size);
    return result;
}

std::string ConvertString(const std::wstring& str) {
    int size = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, &result[0], size, nullptr, nullptr);
    return result;
}

void Log(const std::wstring& message) { OutputDebugStringW(message.c_str()); }

void Log(std::ostream& os, const std::string& message) {
    os << message << std::endl;
    OutputDebugStringA(message.c_str());
}

void Log(const char* message) { OutputDebugStringA(message); }

void Log(const std::string& message) { OutputDebugStringA(message.c_str()); }

LONG WINAPI ExportDump(EXCEPTION_POINTERS* exception) {
    MessageBoxA(nullptr, "Crash!", "Error", MB_OK);
    return EXCEPTION_EXECUTE_HANDLER;
}