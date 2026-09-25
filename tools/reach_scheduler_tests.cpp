#include "../research/ReachScheduler.h"
#include <iostream>
using namespace reach;
int main(){try{
    int checks=0;auto check=[&](bool value){++checks;if(!value)throw std::runtime_error("scheduler test "+std::to_string(checks));};
    auto candidates=[](){DecisionSnapshot s;s.now=100000;s.generation=1;s.remainingIp=1000000;s.hasFrame=true;s.frame={1,1,1,90000,5000,false,false};
        std::vector<SchedulerCandidate> out;auto plans=ActionCandidates(s);for(size_t i=0;i<plans.size();++i){SchedulerCandidate c;c.snapshot=s;c.action=plans[i];c.local=i;
            c.input.kind=static_cast<int>(c.action.kind);c.input.group=c.action.group;c.input.task=1;c.input.live=c.input.eligible=c.action.eligible;out.push_back(c);}return out;};
    auto cs=candidates();PathPredictor empty;
    auto never=[](){return false;};
    auto result=SelectReachAction(cs,empty,never);check(result.fallback&&result.index==3&&result.reason=="calibration_not_loaded");
    PathPredictor model;
    for(int kind=1;kind<=4;++kind)for(int j=0;j<128;++j){auto s=PredictionSample{};s.run=1+j%2;s.input=cs[kind].input;s.y.fill(kind==2?1:0);model.Add(s);}
    result=SelectReachAction(cs,model,never);check(!result.fallback&&result.index==2&&result.reason=="raw_short_horizon_proxy");
    check(cs[2].prediction.probability[3]==-1&&cs[2].prediction.raw[3]==1); // no calibration implicitly used
    auto timeout=[](){return true;};result=SelectReachAction(cs,model,timeout);check(result.fallback&&result.index==3&&result.reason=="compute_budget");
    int polls=0;auto cancelled=model.Predict(cs[2].input,[&](){return ++polls==3;});check(cancelled.status=="compute_budget"&&cancelled.raw[3]==-1&&polls==3);
    for(size_t feature=0;feature<16;++feature){auto changed=candidates();changed[2].input.x[feature]=PredictionScales[feature]*3;
        result=SelectReachAction(changed,model,never);check(result.fallback&&result.reason=="out_of_domain");}
    auto changed=candidates();changed[1].input.live=false;result=SelectReachAction(changed,model,never);check(result.fallback&&result.reason=="notification_unavailable");
    changed=candidates();changed[1].input.causal=false;result=SelectReachAction(changed,model,never);check(result.fallback&&result.reason=="future_input");
    changed=candidates();for(auto& c:changed)if(c.local){c.action.eligible=false;c.action.reason="byte_budget_insufficient";}
    result=SelectReachAction(changed,model,never);check(result.fallback&&result.index==0);
    changed=candidates();changed[6].action.eligible=true;result=SelectReachAction(changed,model,never);check(result.index==6&&result.reason=="common_refresh");
    changed[5].action.eligible=true;check(SchedulerFallback(changed)==5);
    auto second=changed[5];second.snapshot.frame.capture=100;changed.push_back(second);check(SchedulerFallback(changed)==7);
    changed=candidates();second=changed[1];second.snapshot.frame.id=2;second.snapshot.frame.capture=99999;changed.push_back(second);check(SchedulerFallback(changed)==7);
    changed[7].action.eligible=false;changed[3].action.eligible=false;check(SchedulerFallback(changed)==1);
    for(uint64_t n=0;n<100;++n){check(SchedulerSlot(1000,1000+n*20000)==n);check(SchedulerSlot(1000,1000+n*20000+19999)==n);}
    auto stale=candidates()[1].snapshot;stale.now=stale.frame.capture+200000;check(!ActionCandidates(stale)[1].eligible);
    stale.now=100000;stale.generation=2;check(!ActionCandidates(stale)[1].eligible);stale.generation=1;stale.remainingIp=1;check(!ActionCandidates(stale)[1].eligible);
    auto oversized=candidates();oversized.resize(65);bool rejected=false;try{SelectReachAction(oversized,model,never);}catch(...){rejected=true;}check(rejected);
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"scope\":\"selection, interruption inside prediction, unsupported inputs, recovery ordering, stale revalidation, skipped slots\"}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
