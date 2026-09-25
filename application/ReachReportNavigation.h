#pragma once
#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#pragma comment(lib, "shell32.lib")

namespace reachui {
// Resolve from the working directory or executable ancestors; no machine-specific path.
inline std::filesystem::path FindReportHub() {
    wchar_t executable[32768]{};
    const auto length=GetModuleFileNameW(nullptr,executable,32768);
    std::error_code error;
    const auto working=std::filesystem::current_path(error);
    const std::filesystem::path starts[]={error?std::filesystem::path{}:working,
        length&&length<32768?std::filesystem::path(executable).parent_path():std::filesystem::path{}};
    for(auto directory:starts){
        for(int depth=0;depth<12&&!directory.empty();++depth){
            const auto page=directory/L"docs"/L"Reach_RT_Verification.html";
            if(std::filesystem::is_regular_file(page,error)&&!error)return page;
            const auto parent=directory.parent_path();if(parent==directory)break;directory=parent;
        }
    }
    return {};
}
inline void OpenReportHub(HWND owner=nullptr) {
    const auto page=FindReportHub();
    if(page.empty()){
        MessageBoxW(owner,L"検証ページが見つかりません。プロジェクトの tools\\open_reach_verification.cmd から開いてください。",L"Reach-RT 検証結果",MB_OK|MB_ICONINFORMATION);
        return;
    }
    const auto result=ShellExecuteW(owner,L"open",page.c_str(),nullptr,page.parent_path().c_str(),SW_SHOWNORMAL);
    if(reinterpret_cast<INT_PTR>(result)<=32)
        MessageBoxW(owner,L"ブラウザーを起動できませんでした。docs\\Reach_RT_Verification.html を直接開いてください。",L"Reach-RT 検証結果",MB_OK|MB_ICONERROR);
}
}
