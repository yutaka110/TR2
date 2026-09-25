#include "../research/ReachFoundation.h"
#include "../research/ReachJson.h"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
unsigned checks=0;
void Require(bool condition,const char* why) { ++checks; if(!condition) throw std::runtime_error(why); }
template<class F> void Reject(F action,const char* why) {
    ++checks; try { action(); } catch(const std::exception&) { return; } throw std::runtime_error(why);
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: test_reach_g1 config output-directory");
        const auto config=reach::FoundationConfig::Load(argv[1]);
        Require(config.physicsHz==100&&config.cameraHz==30&&config.controlHz==20,"initial rates");
        Require(reach::FoundationConfig::Parse(config.EffectiveJson()).durationUs==config.durationUs,"effective config roundtrip");
        auto feedbackConfig=reach::FoundationConfig::Load("config/reach_rt_g1_command.json");
        feedbackConfig.boundedLink=true;feedbackConfig.budgeted=true;feedbackConfig.stateFeedback=true;
        Require(reach::FoundationConfig::Parse(feedbackConfig.EffectiveJson()).stateFeedback,"state feedback config roundtrip");
        auto stateText=feedbackConfig.EffectiveJson();
        for(const std::string value:{"false","1","\"true\"","null"}){
            auto invalid=stateText;auto at=invalid.find("\"state_feedback\":true");invalid.replace(at,21,"\"state_feedback\":"+value);
            Reject([&](){reach::FoundationConfig::Parse(invalid);},"non-true state feedback option accepted");
        }
        feedbackConfig.budgeted=false;Reject([&](){reach::FoundationConfig::Parse(feedbackConfig.EffectiveJson());},"state feedback without budget accepted");
        feedbackConfig.budgeted=true;feedbackConfig.boundedLink=false;Reject([&](){reach::FoundationConfig::Parse(feedbackConfig.EffectiveJson());},"state feedback without modeled link accepted");
        feedbackConfig.boundedLink=true;feedbackConfig.baseline="B1";feedbackConfig.baselineLambda=.3;
        Require(reach::FoundationConfig::Parse(feedbackConfig.EffectiveJson()).baseline=="B1","baseline roundtrip");
        for(const auto mode:{"B2","B3"}){auto valid=feedbackConfig;valid.baseline=mode;Require(reach::FoundationConfig::Parse(valid.EffectiveJson()).baseline==mode,"ablation mode roundtrip");}
        for(const auto mode:{"B4","R","b1"}){auto invalid=feedbackConfig;invalid.baseline=mode;Reject([&](){reach::FoundationConfig::Parse(invalid.EffectiveJson());},"unknown baseline accepted");}
        for(double lambda:{-1.,3.01}){auto invalid=feedbackConfig;invalid.baselineLambda=lambda;Reject([&](){reach::FoundationConfig::Parse(invalid.EffectiveJson());},"invalid lambda accepted");}
        {auto invalid=feedbackConfig;invalid.stateFeedback=false;Reject([&](){reach::FoundationConfig::Parse(invalid.EffectiveJson());},"baseline without state mode accepted");}
        {auto invalid=feedbackConfig;invalid.baseline="B0";Reject([&](){reach::FoundationConfig::Parse(invalid.EffectiveJson());},"variable B0 accepted");}
        Require(reach::Sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 known vector");
        for(const std::string invalid:std::initializer_list<std::string>{"{\"a\":1,\"a\":2}","{\"a\":1,\"\\u0061\":2}","[1,]","{\"x\":true,}",
              "01","1e999","NaN","1e","+2","{} false","\"\\uD800\"","\"\\uDC00\"","\"\\x20\"",std::string("\"\xc0\x80\"")})
            Reject([&](){ reach::ParseJson(invalid); },"malformed JSON accepted");
        Require(reach::ParseJson("\"\\uD83D\\uDE80\"").StringValue()=="\xf0\x9f\x9a\x80","unicode surrogate pair");
        Require(reach::ParseJson("\"日本語\"").StringValue()=="日本語","UTF8 input");
        Require(reach::ParseJson(reach::JsonString("quote\"\n\\")).StringValue()=="quote\"\n\\","escaped log roundtrip");
        Require(reach::ParseJson(std::string(65536,' ')+"0").NumberValue()==0,"trace-sized JSON supported");
        Reject([](){reach::ParseJson(std::string(4194304,' ')+"0");},"oversized config accepted");
        Reject([](){reach::ParseJson(std::string(18,'[')+"0"+std::string(18,']'));},"excessive nesting accepted");
        auto invalidConfig=[&](const std::string& before,const std::string& after) {
            auto text=config.EffectiveJson(); auto p=text.find(before);
            Require(p!=std::string::npos,"test mutation target missing"); text.replace(p,before.size(),after);
            Reject([&](){reach::FoundationConfig::Parse(text);},"invalid research config accepted");
        };
        invalidConfig("\"schema_version\":1","\"schema_version\":2");
        invalidConfig("\"stage\":\"foundation\"","\"stage\":\"closed_loop\"");
        invalidConfig("\"task\":\"T1\"","\"task\":\"T3\"");
        invalidConfig("\"seed\":101","\"seed\":-1");
        invalidConfig("\"seed\":101","\"seed\":1.5");
        invalidConfig("\"seed\":101","\"seed\":true");
        invalidConfig("\"camera\":30","\"camera\":0");
        invalidConfig("\"camera\":30","\"camera\":\"30\"");
        invalidConfig("\"physics\":100","\"physics\":10");
        invalidConfig("\"max_catchup_steps\":5","\"max_catchup_step\":5");
        invalidConfig("\"duration_s\":60.000000","\"duration_s\":0");
        invalidConfig("\"duration_s\":60.000000","\"duration_s\":3601");
        reach::PeriodicDeadline clock(123456,30,5);
        Require(clock.Poll(123456)==0,"tick fired at time origin");
        for(uint64_t second=0;second<3600;++second)
            for(uint64_t i=1;i<=30;++i) clock.Poll(123456+(second*30+i)*1000000/30);
        Require(clock.Count()==108000,"30 Hz drift over one hour");
        Require(clock.NextUs()==123456+108001ull*1000000/30,"rational next deadline");
        reach::PeriodicDeadline catchup(1000,100,5);
        Require(catchup.Poll(51000)==5,"bounded catchup");
        Reject([&](){catchup.Poll(111000);},"catchup overrun not rejected");
        Reject([](){reach::PeriodicDeadline timer(1000,100,5);timer.Poll(999);},"backwards time not rejected");
        Reject([](){reach::PeriodicDeadline timer(0,0,5);},"zero timer rate accepted");
        const auto output=std::filesystem::path(argv[2]);
        std::string firstId; std::filesystem::path firstPath, secondPath;
        {
            reach::ResearchSession first(config,output,argv[0],true);
            reach::ResearchSession second(config,output,argv[0],true);
            firstId=first.Id(); firstPath=first.Directory(); secondPath=second.Directory();
            Require(first.Id()!=second.Id(),"session IDs reused");
            std::vector<std::thread> workers;
            for(int i=0;i<4;++i) workers.emplace_back([&](){for(int j=0;j<50;++j) first.Event("test_event","concurrent",1);});
            for(auto& worker:workers) worker.join();
            first.Finish("foundation_completed","test",100,30,20);
            first.Finish("invalid","must not overwrite",0,0,0);
            // second session deliberately leaves scope without successful finish.
        }
        const auto summary=reach::ParseJson(reach::ReadFile(firstPath/"summary.json",65536));
        Require(summary.At("status").StringValue()=="foundation_completed","completed result overwritten");
        const auto incomplete=reach::ParseJson(reach::ReadFile(secondPath/"summary.json",65536));
        Require(incomplete.At("status").StringValue()=="incomplete","incomplete session marked complete");
        std::istringstream events(reach::ReadFile(firstPath/"events.jsonl",1048576));
        std::string line; uint64_t seq=0,previousTime=0; unsigned concurrent=0;
        while(std::getline(events,line)) {
            const auto event=reach::ParseJson(line);
            Require(event.At("session_id").StringValue()==firstId,"cross-session event");
            Require(event.At("seq").NumberValue()==++seq,"event sequence gap");
            const auto time=static_cast<uint64_t>(event.At("monotonic_us").NumberValue());
            Require(time>=previousTime,"event clock went backwards"); previousTime=time;
            if(event.At("event").StringValue()=="test_event") ++concurrent;
        }
        Require(concurrent==200,"lost concurrent events");
        std::cout << "PASS " << checks << " checks\n";
        return 0;
    } catch(const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
