#pragma once
#include "ReachActionModel.h"
#include "ReachPrediction.h"
#include "ReachRanking.h"
#include <functional>
#include <limits>
namespace reach {
constexpr uint64_t SchedulerPeriodUs=20000,SchedulerBudgetUs=5000;
constexpr size_t SchedulerMaxCandidates=64;
struct SchedulerCandidate {
    DecisionSnapshot snapshot;size_t local=0;ActionTicket action;
    PredictionInput input;PredictionResult prediction;
    double score=-std::numeric_limits<double>::infinity(),value=1;
};
struct SchedulerOptions {PredictionMask mask=PredictionMask::Both;bool scalar=false;double lambda=.1;};
inline double SchedulerScalarValue(const PredictionInput& q){
    if(!q.live)return 1;
    const double dependency=q.reference==1?1.:(q.idr?2.:.15);
    const double decode=dependency*(1.-.5*std::clamp(q.x[14]/200.,0.,1.));
    const double stage=q.stage==0?2.:q.stage==1?1.:q.stage==2?1.5:2.;
    const double task=std::min(4.,stage+std::clamp(q.x[8]/200.,0.,1.)+std::clamp((q.x[13]+.0003*q.x[8])/.1,0.,1.)+(q.x[9]>0&&q.x[9]<=5?.5:0.));
    return std::clamp(decode*task,.075,4.);
}
struct SchedulerChoice {size_t index=0,evaluated=0;std::string reason="no_action";bool fallback=true;};
// Bounded, deterministic recovery. Requests must already have crossed the reverse link.
inline size_t SchedulerFallback(const std::vector<SchedulerCandidate>& cs){
    size_t repair=0,refresh=0,fresh=0;
    for(size_t i=1;i<cs.size();++i){const auto& c=cs[i];if(!c.action.eligible)continue;
        if(c.action.kind==ActionKind::Repair&&(!repair||c.snapshot.frame.capture<cs[repair].snapshot.frame.capture))repair=i;
        if(c.action.kind==ActionKind::Refresh)refresh=i;
        if((c.local==3||c.local==1)&&(!fresh||c.snapshot.frame.capture>cs[fresh].snapshot.frame.capture||
          (c.snapshot.frame.id==cs[fresh].snapshot.frame.id&&c.local==3)))fresh=i;
    }
    return repair?repair:refresh?refresh:fresh;
}
inline SchedulerChoice SelectReachAction(std::vector<SchedulerCandidate>& cs,const PathPredictor& model,const std::function<bool()>& stop,const SchedulerOptions& options={}){
    if(cs.empty()||cs.size()>SchedulerMaxCandidates)throw std::runtime_error("invalid scheduler candidate bound");
    SchedulerChoice result;result.index=SchedulerFallback(cs);
    const auto mask=options.scalar?PredictionMask::Common:options.mask;
    for(auto& c:cs){c.value=options.scalar?SchedulerScalarValue(c.input):1.;c.input=ProjectPrediction(c.input,mask);}
    // Refresh has no materialized AU yet: its short-path label cannot price its eventual bytes.
    if(cs[result.index].action.kind==ActionKind::Refresh){result.reason="common_refresh";return result;}
    size_t best=0;double bestScore=0;bool supported=false,unknown=false;std::string unavailable;
    cs[0].score=0;
    for(size_t i=1;i<cs.size();++i){auto& c=cs[i];if(!c.action.eligible||c.action.kind==ActionKind::Refresh)continue;
        if(stop()){result.reason="compute_budget";return result;}
        c.prediction=model.Predict(c.input,stop,mask);++result.evaluated;
        if(c.prediction.status=="compute_budget"){result.reason="compute_budget";return result;}
        if(c.prediction.raw[3]<0){unknown=true;if(unavailable.empty())unavailable=c.prediction.status;continue;}
        supported=true;
        // G4-02 histogram calibration worsened held-out error. Use raw 200 ms
        // target-image command availability only as an experimental ranking proxy.
        // It is NOT calibrated task success, causal benefit, or the complete R policy.
        c.score=ReachShortScore(c.prediction.raw[3],double(c.action.ip),options.lambda,65536.,c.value);
        if(c.score>bestScore){bestScore=c.score;best=i;}
    }
    if(stop()){result.reason="compute_budget";return result;}
    if(unknown){result.reason=unavailable;return result;}
    if(!supported){result.reason="no_supported_action";return result;}
    result.index=best;result.reason="raw_short_horizon_proxy";result.fallback=false;return result;
}
// Skip missed slots; do not bunch several decisions after a Windows scheduling stall.
inline uint64_t SchedulerSlot(uint64_t origin,uint64_t now){return now<=origin?0:(now-origin)/SchedulerPeriodUs;}
}
