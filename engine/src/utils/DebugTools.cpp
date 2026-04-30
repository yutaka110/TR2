#include "utils/DebugTools.h"

#include <cassert>

#include <filesystem>
#include <fstream>


#include <strsafe.h>

#include <DbgHelp.h>
#pragma comment(lib, "DbgHelp.lib")

namespace debugtools {

	void Log(const std::wstring& message) {
		OutputDebugStringW(message.c_str());
	}

	void Log(std::ostream& os, const std::string& message) {
		os << message << std::endl;
		OutputDebugStringA(message.c_str());
	}

	void Log(const char* message) {
		OutputDebugStringA(message);
		OutputDebugStringA("\n");
	}

	void Log(const std::string& message) {
		OutputDebugStringA(message.c_str());
		OutputDebugStringA("\n");
	}

	// UTF-16(wstring) -> UTF-8(string)
	std::string ConvertString(const std::wstring& wstr) {
		if (wstr.empty()) { return {}; }

		int sizeNeeded = WideCharToMultiByte(
			CP_UTF8, 0,
			wstr.data(), (int)wstr.size(),
			nullptr, 0,
			nullptr, nullptr
		);
		if (sizeNeeded <= 0) { return {}; }

		std::string out(sizeNeeded, '\0');
		WideCharToMultiByte(
			CP_UTF8, 0,
			wstr.data(), (int)wstr.size(),
			out.data(), sizeNeeded,
			nullptr, nullptr
		);
		return out;
	}

	// UTF-8(string) -> UTF-16(wstring)
	std::wstring ConvertString(const std::string& str) {
		if (str.empty()) { return {}; }

		int sizeNeeded = MultiByteToWideChar(
			CP_UTF8, 0,
			str.data(), (int)str.size(),
			nullptr, 0
		);
		if (sizeNeeded <= 0) { return {}; }

		std::wstring out(sizeNeeded, L'\0');
		MultiByteToWideChar(
			CP_UTF8, 0,
			str.data(), (int)str.size(),
			out.data(), sizeNeeded
		);
		return out;
	}


	static std::wstring MakeDumpPath() {
		// 実行ファイルと同じ場所に Dump を出す
		wchar_t exePath[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exePath, MAX_PATH);

		wchar_t drive[_MAX_DRIVE]{};
		wchar_t dir[_MAX_DIR]{};
		_wsplitpath_s(exePath, drive, _MAX_DRIVE, dir, _MAX_DIR, nullptr, 0, nullptr, 0);

		wchar_t folder[MAX_PATH]{};
		(void)StringCchPrintfW(folder, MAX_PATH, L"%s%s", drive, dir);

		// dumps フォルダを作る
		std::filesystem::path dumpDir = std::filesystem::path(folder) / L"dumps";
		std::error_code ec;
		std::filesystem::create_directories(dumpDir, ec);

		// dump ファイル名（日時）
		SYSTEMTIME st{};
		GetLocalTime(&st);

		wchar_t fileName[MAX_PATH]{};
		(void)StringCchPrintfW(
			fileName, MAX_PATH,
			L"dump_%04d%02d%02d_%02d%02d%02d.dmp",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond
		);

		return (dumpDir / fileName).wstring();
	}

	LONG WINAPI ExportDump(EXCEPTION_POINTERS* exception) {

		const std::wstring dumpPath = MakeDumpPath();

		HANDLE hFile = CreateFileW(
			dumpPath.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (hFile == INVALID_HANDLE_VALUE) {
			Log(L"[ExportDump] CreateFileW failed.\n");
			return EXCEPTION_EXECUTE_HANDLER;
		}

		MINIDUMP_EXCEPTION_INFORMATION expInfo{};
		expInfo.ThreadId = GetCurrentThreadId();
		expInfo.ExceptionPointers = exception;
		expInfo.ClientPointers = FALSE;

		const BOOL ok = MiniDumpWriteDump(
			GetCurrentProcess(),
			GetCurrentProcessId(),
			hFile,
			MiniDumpNormal,
			(exception ? &expInfo : nullptr),
			nullptr,
			nullptr
		);

		CloseHandle(hFile);

		if (ok) {
			Log(L"[ExportDump] dump created: " + dumpPath + L"\n");
		}
		else {
			Log(L"[ExportDump] MiniDumpWriteDump failed.\n");
		}

		return EXCEPTION_EXECUTE_HANDLER;
	}

} // namespace debugtools
