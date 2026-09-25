#pragma once
#include "../network/DatagramSendHook.h"
#include "ReachIpBudget.h"
#include "ReachBufferedLog.h"
#include "ReachDeferredLog.h"
#include <filesystem>
#include <map>
#include <mutex>
namespace reach {
class BudgetTransport {
    struct Counts {uint64_t offered=0,attempted=0,sent=0,rejected=0,errors=0;};
    IpBudget budget_;IpBudgetConfig config_;std::filesystem::path directory_;
    DeferredLog log_,feedback_;BufferedLog timing_;std::mutex mutex_;uint64_t origin_=0,duration_=0,sequence_=0,received_=0;
    bool closed_=false,finished_=false;std::string error_;
    std::map<std::string,Counts> counts_;
public:
    BudgetTransport(const IpBudgetConfig& config,const std::filesystem::path& directory,uint64_t duration);
    ~BudgetTransport();
    void Start(uint64_t origin);
    static constexpr int ExpiredBeforeAdmission=-2;
    int Send(bool uplink,SOCKET socket,std::span<const uint8_t> bytes,const sockaddr_in& destination,uint64_t deadlineUs=0);
    void Feedback(std::span<const uint8_t> bytes);
    uint64_t Received();
    uint64_t RemainingUplinkBytes(); // advisory snapshot; actual admission remains atomic in Send
    void CloseAdmission();
    void Check();
    void Finish();
};
}
