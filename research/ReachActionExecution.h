#pragma once
#include "ReachActionModel.h"
#include "ReachFoundation.h"
#include "ReachBudgetTransport.h"
#include "ReachPrediction.h"
#include "ReachStateProtocol.h"
#include "../network/NetworkManager.h"
#include <map>
#include <set>
namespace reach {
// G4-01 wiring policy, not ReachScheduler or a calibrated success predictor.
class ActionExecution {
    struct Entry {ActionFrame frame;uint64_t offeredDecision=0,repairDecision=0;uint16_t group=0;uint32_t repairs=0;std::set<uint16_t> missing;};
    std::mutex mutex_;std::map<uint32_t,Entry> frames_;RefreshArbiter refresh_;
    uint64_t generation_=0,decision_=0;uint32_t stream_=0;BudgetTransport& budget_;
    BufferedLog candidates_,events_,wire_;bool closed_=false;
    NetworkManager* network_=nullptr;int task_=0;bool predict_=false;
    PathPredictor predictor_;SenderStateEstimate state_;BufferedLog predictions_,predictionFeedback_;
    uint64_t feedbackId_=0,feedbackUs_=0,lastSequence_=0;uint32_t lastIdr_=0;
    double loss_=.01,burst_=.01;bool lastMissing_=false;
    void Predict(const DecisionSnapshot&,const std::vector<ActionTicket>&,uint64_t,uint32_t);
    uint64_t Record(const DecisionSnapshot&,const std::vector<ActionTicket>&,size_t,uint32_t);
public:
    ActionExecution(BudgetTransport&,const std::filesystem::path&,NetworkManager* =nullptr,int task=0);
    void PublishState(const SenderStateEstimate&);
    void Feedback(std::span<const uint8_t>);
    bool Plan(uint32_t,uint32_t,uint64_t,uint64_t,bool,NetworkManager::RnvpFrameProtectionOptions&);
    bool Repair(uint32_t,std::span<const uint16_t>);
    bool Request(uint32_t,bool);
    int Send(SOCKET,std::span<const uint8_t>,const sockaddr_in&);
    void Close();
};
}
