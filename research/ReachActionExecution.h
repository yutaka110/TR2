#pragma once
#include "ReachActionModel.h"
#include "ReachFoundation.h"
#include "ReachBudgetTransport.h"
#include "ReachPrediction.h"
#include "ReachScheduler.h"
#include "ReachStateProtocol.h"
#include "../network/NetworkManager.h"
#include <map>
#include <set>
#include <thread>
#include <atomic>
namespace reach {
// Opt-in G4-01/02 wiring and G4-03 periodic, bounded experimental scheduler.
class ActionExecution {
    struct Entry {ActionFrame frame;uint64_t offeredDecision=0,repairDecision=0;uint16_t group=0;uint32_t repairs=0;std::set<uint16_t> missing;
        std::shared_ptr<const std::vector<uint8_t>> payload;std::vector<uint16_t> pendingMissing;};
    std::mutex mutex_;std::map<uint32_t,Entry> frames_;RefreshArbiter refresh_;
    uint64_t generation_=0,decision_=0;uint32_t stream_=0;BudgetTransport& budget_;
    BufferedLog candidates_,events_,wire_;bool closed_=false;
    NetworkManager* network_=nullptr;int task_=0;bool predict_=false;
    PathPredictor predictor_;SenderStateEstimate state_;BufferedLog predictions_,predictionFeedback_;
    uint64_t feedbackId_=0,feedbackUs_=0,lastSequence_=0;uint32_t lastIdr_=0;
    double loss_=.01,burst_=.01;bool lastMissing_=false;
    bool scheduled_=false,forceTimeout_=false;std::atomic<bool> schedulerStop_{false};std::thread schedulerThread_;
    std::string schedulerError_;
    BufferedLog schedulerLog_,schedulerCandidates_,schedulerExecution_;
    uint64_t refreshPermit_=0,refreshPermitUs_=0;
    SchedulerOptions schedulerOptions_;
    void Tick(uint64_t,uint64_t,uint64_t);
    void Predict(const DecisionSnapshot&,const std::vector<ActionTicket>&,uint64_t,uint32_t);
    uint64_t Record(const DecisionSnapshot&,const std::vector<ActionTicket>&,size_t,uint32_t);
public:
    ActionExecution(BudgetTransport&,const std::filesystem::path&,NetworkManager* =nullptr,int task=0,bool scheduled=false,bool comparison=false,double lambda=.1);
    ~ActionExecution();
    bool Scheduled()const{return scheduled_;}
    void Queue(uint32_t,uint32_t,uint64_t,std::vector<uint8_t>,bool,bool);
    void StartScheduler(uint64_t,uint64_t);
    void StopScheduler();void CheckScheduler();
    void PublishState(const SenderStateEstimate&);
    void Feedback(std::span<const uint8_t>);
    bool Plan(uint32_t,uint32_t,uint64_t,uint64_t,bool,NetworkManager::RnvpFrameProtectionOptions&);
    bool Repair(uint32_t,std::span<const uint16_t>);
    bool Request(uint32_t,bool);
    int Send(SOCKET,std::span<const uint8_t>,const sockaddr_in&);
    void Close();
};
}
