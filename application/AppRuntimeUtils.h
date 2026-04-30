#pragma once
#include <string>
#include <Windows.h>
#include <ostream>

std::wstring ConvertString(const std::string& str);
std::string ConvertString(const std::wstring& str);

void Log(std::ostream& os, const std::string& message);

LONG WINAPI ExportDump(EXCEPTION_POINTERS* exception);

void Log(const std::wstring& message);

void Log(std::ostream& os, const std::string& message);

void Log(const char* message);

void Log(const std::string& message);