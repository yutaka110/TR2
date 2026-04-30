#pragma once
#include <Windows.h>
#include <ostream>
#include <string>

namespace debugtools {

	void Log(const std::wstring& message);
	void Log(std::ostream& os, const std::string& message);
	void Log(const char* message);
	void Log(const std::string& message);

	std::string ConvertString(const std::wstring& wstr);
	std::wstring ConvertString(const std::string& str);

	// Use with SetUnhandledExceptionFilter
	LONG WINAPI ExportDump(EXCEPTION_POINTERS* exception);

} // namespace debugtools
