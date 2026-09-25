#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace reach {
enum class ActionKind {Defer,Fresh,Protect,Repair,Refresh};
inline const char* ActionName(ActionKind k){switch(k){case ActionKind::Fresh:return "FRESH";case ActionKind::Protect:return "PROTECT";case ActionKind::Repair:return "REPAIR";case ActionKind::Refresh:return "REFRESH";default:return "DEFER";}}
struct ActionFrame {uint32_t id=0,stream=0;uint64_t generation=0,capture=0,bytes=0;bool idr=false,offered=false;};
struct DecisionSnapshot {
    uint64_t now=0,generation=0,remainingIp=0,lastRefresh=0;
    ActionFrame frame;bool hasFrame=false,refreshWanted=false,refreshPending=false;
    uint32_t repairRequests=0;std::vector<uint16_t> missing;
};
struct ActionTicket {
    ActionKind kind=ActionKind::Defer;uint16_t group=0;
    uint64_t ip=0;bool eligible=false;std::string reason;
};
inline uint64_t ActionDataIp(uint64_t bytes){return bytes+72*((bytes+1199)/1200);}
inline uint64_t ActionParityIp(uint64_t bytes,uint16_t group){
    uint64_t cost=0;const auto n=(bytes+1199)/1200;
    if(group)for(uint64_t i=0;i<n;i+=group)if(std::min<uint64_t>(group,n-i)>1)cost+=1280;
    return cost;
}
inline std::string ActionFrameCheck(const DecisionSnapshot& s){
    if(!s.hasFrame||!s.frame.id||!s.frame.stream||!s.frame.bytes||s.frame.bytes>786432)return "frame_unavailable";
    if(s.frame.generation!=s.generation)return "generation_changed";
    if(s.now<s.frame.capture)return "future_capture";
    if(s.now-s.frame.capture>=200000)return "frame_expired";
    return "eligible";
}
// Actual encoded AU only. PROTECT is a bundle: new data + constructible XOR.
// REPAIR is a received request for a cached AU, never speculative recovery.
inline std::vector<ActionTicket> ActionCandidates(const DecisionSnapshot& s){
    std::vector<ActionTicket> out{{ActionKind::Defer,0,0,true,"eligible"}};
    const auto validity=ActionFrameCheck(s);
    auto add=[&](ActionKind kind,uint16_t group,uint64_t ip,std::string reason){
        if(reason=="eligible"&&ip>s.remainingIp)reason="byte_budget_insufficient";
        out.push_back({kind,group,ip,reason=="eligible",std::move(reason)});
    };
    const auto fresh=validity!="eligible"?validity:s.frame.offered?"already_offered":"eligible";
    add(ActionKind::Fresh,0,ActionDataIp(s.frame.bytes),fresh);
    for(uint16_t g:{2,4,8})add(ActionKind::Protect,g,ActionDataIp(s.frame.bytes)+ActionParityIp(s.frame.bytes,g),
        fresh!="eligible"?fresh:s.frame.bytes<=1200?"no_xor_group":"eligible");
    auto repair=validity;uint64_t repairIp=0;
    if(repair=="eligible"&&!s.frame.offered)repair="not_in_send_cache";
    if(repair=="eligible"&&s.missing.empty())repair="no_received_missing_request";
    if(repair=="eligible"&&s.repairRequests>=2)repair="repair_request_limit";
    auto chunks=s.missing;std::sort(chunks.begin(),chunks.end());
    if(repair=="eligible"&&std::adjacent_find(chunks.begin(),chunks.end())!=chunks.end())repair="duplicate_chunk_request";
    for(auto i:chunks){if(uint64_t(i)*1200>=s.frame.bytes){repair="chunk_unavailable";break;}
        repairIp+=std::min<uint64_t>(1200,s.frame.bytes-uint64_t(i)*1200)+72;}
    add(ActionKind::Repair,0,repairIp,repair);
    add(ActionKind::Refresh,0,0,!s.refreshWanted?"no_refresh_request":s.refreshPending?"idr_pending":
        s.now<s.lastRefresh?"future_request":s.lastRefresh&&s.now-s.lastRefresh<500000?"idr_cooldown":"eligible");
    return out; // Seven bounded candidates, below the G4 limit of 64.
}
class RefreshArbiter {
    uint64_t last_=0;uint32_t pendingFrame_=0;bool wanted_=false;
public:
    void Want(bool value){wanted_|=value;}
    bool Wanted()const{return wanted_;}bool Pending()const{return pendingFrame_!=0;}
    uint64_t Last()const{return last_;}uint32_t PendingFrame()const{return pendingFrame_;}
    void Issued(uint64_t now,uint32_t input){last_=now;pendingFrame_=input;wanted_=false;}
    bool Output(bool idr,uint32_t frame){if(idr&&pendingFrame_&&frame>=pendingFrame_){pendingFrame_=0;wanted_=false;return true;}return false;}
};
}
