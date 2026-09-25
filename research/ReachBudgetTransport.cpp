#define NOMINMAX
#include "ReachBudgetTransport.h"
#include "ReachFoundation.h"
#include "ReachJson.h"
namespace reach {
namespace {std::string Hex(std::span<const uint8_t> data){std::string s;for(auto b:data){s+="0123456789abcdef"[b>>4];s+="0123456789abcdef"[b&15];}return s;}}
BudgetTransport::BudgetTransport(const IpBudgetConfig& c,const std::filesystem::path& dir,uint64_t duration):budget_(c),config_(c),directory_(dir),duration_(duration){
    log_.open(dir/"ip_budget.csv");feedback_.open(dir/"feedback_rx.csv");log_.exceptions(std::ios::badbit|std::ios::failbit);feedback_.exceptions(std::ios::badbit|std::ios::failbit);
    log_<<"event_id,event_us,direction,kind,status,payload_bytes,ip_bytes,attempted_ip_bytes,discarded_before_send_bytes,total_used_ip_bytes,direction_used_ip_bytes,wire_hex\n";
    feedback_<<"received_us,kind,wire_hex\n";
    timing_.open(dir/"budget_timing.csv");timing_.exceptions(std::ios::badbit|std::ios::failbit);
    timing_<<"event_id,start_us,lock_wait_us,send_us,journal_us\n";
}
BudgetTransport::~BudgetTransport(){try{Finish();}catch(...){}}
void BudgetTransport::Start(uint64_t origin){std::lock_guard lock(mutex_);if(origin_)throw std::runtime_error("budget already started");origin_=origin;}
int BudgetTransport::Send(bool up,SOCKET socket,std::span<const uint8_t> bytes,const sockaddr_in& destination,uint64_t deadlineUs){
    const auto started=MonotonicUs();std::lock_guard lock(mutex_);const auto now=MonotonicUs();const auto relative=origin_?now-origin_:0;
    // Opt-in media gate after the budget mutex wait, before any debit/sendto.
    // The action journal records this pre-admission rejection; old callers pass zero.
    if(deadlineUs&&now>=deadlineUs)return ExpiredBeforeAdmission;
    const std::string direction=up?"uplink":"downlink",kind=IpPacketClass(bytes);const auto ip=bytes.size()+28;
    auto& count=counts_[direction+"/"+kind];count.offered+=ip;
    std::string status;
    if(!origin_||closed_||relative>=duration_)status="trial_closed";
    else if(kind=="unknown"){status="unknown_protocol";error_="unclassified budget packet";}
    else status=budget_.Admit(relative,up,ip);
    int result=0;const bool attempt=status=="allowed";const auto beforeSend=MonotonicUs();
    if(attempt){
        count.attempted+=ip;
        result=sendto(socket,reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination));
        if(result==static_cast<int>(bytes.size())){count.sent+=ip;status="sent";}
        else{count.errors+=ip;status="send_error";error_="budgeted sendto failed";}
    }else count.rejected+=ip;
    const auto afterSend=MonotonicUs();log_<<++sequence_<<','<<relative<<','<<direction<<','<<kind<<','<<status<<','<<bytes.size()<<','<<ip<<','<<(attempt?ip:0)<<','<<(attempt?0:ip)<<','<<budget_.Used(0)<<','<<budget_.Used(up?1:2)<<','<<Hex(bytes)<<'\n';
    const auto afterLog=MonotonicUs();if(afterLog-started>2000)timing_<<sequence_<<','<<started<<','<<now-started<<','<<afterSend-beforeSend<<','<<afterLog-afterSend<<'\n';
    return result;
}
void BudgetTransport::Feedback(std::span<const uint8_t> bytes){std::lock_guard lock(mutex_);++received_;feedback_<<MonotonicUs()<<','<<IpPacketClass(bytes)<<','<<Hex(bytes)<<'\n';}
uint64_t BudgetTransport::Received(){std::lock_guard lock(mutex_);return received_;}
uint64_t BudgetTransport::RemainingUplinkBytes(){std::lock_guard lock(mutex_);if(!origin_||closed_)return 0;
    return std::min(config_.total.maxBytes-budget_.Used(0),config_.up.maxBytes-budget_.Used(1));}
void BudgetTransport::CloseAdmission(){std::lock_guard lock(mutex_);closed_=true;}
void BudgetTransport::Check(){std::lock_guard lock(mutex_);if(!error_.empty())throw std::runtime_error(error_);}
void BudgetTransport::Finish(){
    std::lock_guard lock(mutex_);if(finished_)return;closed_=true;
    std::ofstream out(directory_/"ip_budget_summary.json");out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"{\"origin_us\":"<<origin_<<",\"duration_us\":"<<duration_<<",\"attempted_ip_bytes\":"<<budget_.Used(0)<<",\"uplink_ip_bytes\":"<<budget_.Used(1)<<",\"downlink_ip_bytes\":"<<budget_.Used(2)<<",\"feedback_received\":"<<received_<<",\"error\":"<<JsonString(error_)<<",\"classes\":{";
    bool first=true;for(const auto& [name,c]:counts_){if(!first)out<<',';first=false;out<<JsonString(name)<<":{\"offered_ip_bytes\":"<<c.offered<<",\"attempted_ip_bytes\":"<<c.attempted<<",\"sent_ip_bytes\":"<<c.sent<<",\"discarded_before_send_bytes\":"<<c.rejected<<",\"send_error_ip_bytes\":"<<c.errors<<'}';}
    out<<"}}\n";out.close();log_.close();feedback_.close();timing_.close();finished_=true;
}
}
