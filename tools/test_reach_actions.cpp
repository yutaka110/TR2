#include "../research/ReachActionModel.h"
#include "../research/ReachIpBudget.h"
#include <iostream>
#include <stdexcept>
using namespace reach;
static int checks=0;
static void check(bool v){++checks;if(!v)throw std::runtime_error("action check "+std::to_string(checks));}
int main(){try{
    DecisionSnapshot s;s.now=1000000;s.generation=2;s.remainingIp=100000;s.hasFrame=true;s.frame={10,9,2,900000,2500,false,false};
    auto c=ActionCandidates(s);check(c.size()==7);check(c[0].eligible&&c[1].eligible&&c[3].eligible);
    check(c[1].ip==2716&&c[3].ip==3996);check(!c[5].eligible&&c[5].reason=="not_in_send_cache");
    s.frame.offered=true;s.missing={0,2};c=ActionCandidates(s);check(!c[1].eligible&&c[5].eligible&&c[5].ip==1444);
    s.missing={2,2};check(ActionCandidates(s)[5].reason=="duplicate_chunk_request");
    s.missing={3};check(ActionCandidates(s)[5].reason=="chunk_unavailable");
    s.missing={1};s.repairRequests=2;check(ActionCandidates(s)[5].reason=="repair_request_limit");
    s.repairRequests=0;s.remainingIp=1271;check(ActionCandidates(s)[5].reason=="byte_budget_insufficient");
    s.remainingIp=1272;check(ActionCandidates(s)[5].eligible);
    s.generation=3;check(ActionCandidates(s)[5].reason=="generation_changed");s.generation=2;
    s.now=1100000;check(ActionCandidates(s)[5].reason=="frame_expired");s.now=1099999;check(ActionCandidates(s)[5].eligible);
    s.now=899999;check(ActionCandidates(s)[5].reason=="future_capture");s.now=1000000;
    s.hasFrame=false;check(ActionCandidates(s)[1].reason=="frame_unavailable");check(ActionCandidates(s)[0].eligible);
    s.refreshWanted=true;check(ActionCandidates(s)[6].eligible);
    s.refreshPending=true;check(ActionCandidates(s)[6].reason=="idr_pending");s.refreshPending=false;
    s.lastRefresh=500001;check(ActionCandidates(s)[6].reason=="idr_cooldown");s.lastRefresh=500000;check(ActionCandidates(s)[6].eligible);
    s.lastRefresh=1000001;check(ActionCandidates(s)[6].reason=="future_request");
    RefreshArbiter refresh;refresh.Want(true);check(refresh.Wanted());refresh.Issued(1000000,45);
    check(refresh.Pending());check(!refresh.Output(true,44));check(!refresh.Output(false,45));check(refresh.Output(true,46));check(!refresh.Pending());
    s.hasFrame=true;s.frame.offered=false;s.frame.bytes=1200;s.remainingIp=100000;
    check(ActionCandidates(s)[2].reason=="no_xor_group");
    s.frame.bytes=1201;check(ActionCandidates(s)[3].ip==2625);
    // Independently sum actual data datagram lengths and full XOR groups.
    for(uint64_t bytes=1;bytes<8000;bytes+=37)for(uint16_t group:{2,4,8}){
        uint64_t expected=0;std::vector<uint64_t> sizes;
        for(uint64_t remaining=bytes;remaining;){auto k=std::min<uint64_t>(1200,remaining);sizes.push_back(k);expected+=k+72;remaining-=k;}
        for(size_t i=0;i<sizes.size();i+=group){auto end=std::min(sizes.size(),i+group);if(end-i>1)expected+=*std::max_element(sizes.begin()+i,sizes.begin()+end)+80;}
        check(expected==ActionDataIp(bytes)+ActionParityIp(bytes,group));
    }
    IpBudgetConfig config;config.total={1000000,5000,1500};config.up=config.total;config.down=config.total;IpBudget budget(config);
    check(budget.Admit(0,true,1272)=="allowed");check(budget.Admit(0,false,240)=="total_byte_limit");
    check(budget.Admit(0,false,116)=="allowed");check(budget.Used(0)==1388);
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
