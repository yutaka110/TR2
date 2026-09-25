#pragma once
#include "ReachFoundation.h"
#include "ReachBaselinePlanner.h"
#include "ReachBaselineState.h"
#include "../network/NetworkManager.h"
#include <deque>
#include <map>
#include <span>
namespace reach {
class Baseline {
    struct Frame {uint64_t capture,bytes;uint16_t rounds;uint32_t repairs=0;};
    std::string mode_;double lambda_;uint32_t ceiling_;
    std::mutex mutex_;BufferedLog feedback_,decisions_,candidates_,repairs_,states_;
    BaselineState state_;uint64_t stateId_=0;
    uint64_t feedbackId_=0,lastFeedbackUs_=0,nextPing_=0,nacks_=0,lastSequence_=0,lastOfferUs_=0;
    double loss_=.01,burst_=.01,rttUs_=40000;bool lastMissing_=false;
    std::map<uint32_t,Frame> frames_;std::deque<uint32_t> order_;
public:
    Baseline(std::string mode,double lambda,uint32_t pacingCeiling,const std::filesystem::path&);
    void Feedback(std::span<const uint8_t>);
    bool Repair(uint32_t,std::span<const uint16_t>,NetworkManager&);
    void PublishState(const BaselineState&);
    bool Plan(uint32_t,uint64_t,uint64_t,bool,NetworkManager&,NetworkManager::RnvpFrameProtectionOptions&);
    void Close();
};
}
