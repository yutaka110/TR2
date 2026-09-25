#define NOMINMAX
#include "ReachLiveNavigation.h"
#include "ReachReportNavigation.h"
#include "../research/ReachFoundation.h"
#include <fstream>
#include <optional>
#include <vector>
#include <utility>
#include <stdexcept>
#include <cwctype>

namespace reachui {
namespace {
bool requested=false;
std::wstring Environment(const wchar_t* name){
    const auto count=GetEnvironmentVariableW(name,nullptr,0);if(!count)return {};
    std::wstring result(count,L'\0');const auto copied=GetEnvironmentVariableW(name,result.data(),count);
    if(copied>=count)throw std::runtime_error("environment changed during navigation");result.resize(copied);return result;
}
bool ResearchVariable(const std::wstring& name){
    return name.starts_with(L"RNVP_")||name.starts_with(L"TR2_NETWORK_")||name.starts_with(L"TR2_REACH_")||name.starts_with(L"TR2_RESEARCH_");
}
class EnvironmentScope {
    std::vector<std::pair<std::wstring,std::optional<std::wstring>>> saved_;
public:
    EnvironmentScope()=default;
    void Set(const std::wstring& key,const wchar_t* value){
        bool saved=false;for(const auto& item:saved_)if(item.first==key)saved=true;
        if(!saved){SetLastError(ERROR_SUCCESS);auto old=Environment(key.c_str());const bool exists=!old.empty()||GetLastError()!=ERROR_ENVVAR_NOT_FOUND;saved_.emplace_back(key,exists?std::optional(old):std::nullopt);}
        if(!SetEnvironmentVariableW(key.c_str(),value))throw std::runtime_error("could not configure research mode");
    }
    void ClearExperimentVariables(){
        std::vector<std::wstring> names;auto block=GetEnvironmentStringsW();if(!block)throw std::runtime_error("environment unavailable");
        for(const wchar_t* p=block;*p;p+=wcslen(p)+1){const std::wstring entry(p);const auto end=entry.find(L'=');if(end!=0&&end!=std::wstring::npos){auto name=entry.substr(0,end);std::wstring upper=name;for(auto& c:upper)c=static_cast<wchar_t>(towupper(c));if(ResearchVariable(upper))names.push_back(name);}}
        FreeEnvironmentStringsW(block);for(const auto& name:names)Set(name,nullptr);
    }
    ~EnvironmentScope(){for(auto i=saved_.rbegin();i!=saved_.rend();++i)SetEnvironmentVariableW(i->first.c_str(),i->second?i->second->c_str():nullptr);}
};
}
void RequestResearchLive(){requested=true;}
bool ResearchLiveRequested(){return requested;}
void ResetResearchLiveRequest(){requested=false;}
std::wstring NavigationDiagnostic(){const auto value=Environment(L"TR2_REACH_NAVIGATION_TEST");return value==L"roundtrip"||value==L"interrupt"||value==L"missing"||value==L"window"?value:L"";}
void NavigationTrace(const char* event,int result){
    const auto root=Environment(L"TR2_REACH_NAVIGATION_TEST_ROOT");if(root.empty())return;
    std::filesystem::create_directories(root);std::ofstream out(std::filesystem::path(root)/"navigation.jsonl",std::ios::app);
    out<<"{\"event\":\""<<event<<"\",\"result\":"<<result<<",\"pid\":"<<GetCurrentProcessId()<<"}\n";
}
bool RunResearchLive(int& exitCode,const std::wstring& diagnostic){
    NavigationTrace("research_enter");
    const auto diagnosticRoot=Environment(L"TR2_REACH_NAVIGATION_TEST_ROOT");
    try {
        const auto hub=FindReportHub();if(hub.empty())throw std::runtime_error("project directory was not found");
        const auto root=hub.parent_path().parent_path();
        auto config=root/"config/reach_rt_live_demo.json";
        auto model=root/"artifacts/reach_g4_prediction_20260924/verification/study_02/paths.csv";
        if(diagnostic==L"missing")model=root/"artifacts/missing_navigation_test_model.csv";
        if(!std::filesystem::is_regular_file(config)||!std::filesystem::is_regular_file(model))throw std::runtime_error("live configuration or prediction model is missing");
        reach::ResearchUiNavigation navigation;
        navigation.diagnosticAutoReturn=!diagnostic.empty();
        navigation.diagnosticReturnAfterUs=diagnostic==L"interrupt"?1000000:0;
        auto output=root/"artifacts/reach_rt_live/sessions";
        if(!diagnostic.empty()){
            if(diagnosticRoot.empty())throw std::runtime_error("navigation test needs an output directory");
            output=std::filesystem::path(diagnosticRoot)/"sessions";
            auto testConfig=reach::FoundationConfig::Load(config);testConfig.durationUs=diagnostic==L"window"?20000000:2000000;
            config=std::filesystem::path(diagnosticRoot)/"test_config.json";
            std::ofstream file(config,std::ios::binary);file.exceptions(std::ios::badbit|std::ios::failbit);file<<testConfig.EffectiveJson();file.close();
        }
        {
            // All legacy workers have stopped; restore caller settings after every exit.
            EnvironmentScope environment;environment.ClearExperimentVariables();
            environment.Set(L"TR2_RESEARCH_MODE",L"reach_rt");
            environment.Set(L"TR2_REACH_CONFIG",config.c_str());
            environment.Set(L"TR2_REACH_OUTPUT_ROOT",output.c_str());
            environment.Set(L"TR2_REACH_PREDICTION_MODEL",model.c_str());
            environment.Set(L"TR2_REACH_SCHEDULER_VARIANT",L"joint");
            environment.Set(L"TR2_REACH_PRECISE_WAIT",L"1");
            environment.Set(L"TR2_REACH_PACKET_TRACE",L"1");
            environment.Set(L"TR2_REACH_HEADLESS",diagnostic.empty()||diagnostic==L"window"?L"0":L"1");
            if(!reach::RunResearchModeFromEnvironment(exitCode,&navigation))throw std::runtime_error("research mode was not entered");
        }
        NavigationTrace("research_exit",exitCode);
        return navigation.returnToLegacy;
    }catch(const std::exception& error){
        exitCode=2;NavigationTrace("research_launch_failed",exitCode);
        OutputDebugStringA(error.what());
        if(diagnostic.empty())MessageBoxW(nullptr,L"研究モードを開始できませんでした。config/reach_rt_live_demo.json と G4-02の予測モデルを確認してください。通常画面へ戻ります。",L"Reach-RT",MB_OK|MB_ICONERROR);
        return true;
    }
}
}
