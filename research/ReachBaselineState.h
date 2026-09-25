#pragma once
#include "ReachBaselinePlanner.h"
#include "ReachStateProtocol.h"
#include <string_view>
namespace reach {
// Only the adapter can see the complete received report. Planner-visible
// features are zeroed before publication, not merely ignored in the formula.
struct BaselineState {
    uint64_t estimateId=0,sampledUs=0,receivedUs=0,generatedUs=0,sequence=0;
    uint16_t mask=0,reference=0,stage=0;
    uint64_t generation=0,decodeWaitUs=0,observationCaptureUs=0,deadlineUs=0;
    uint32_t positionErrorMicro=0;
};
inline BaselineState ProjectBaselineState(std::string_view mode,const SenderStateEstimate& e){
    BaselineState s;
    if(mode!="B2"&&mode!="B3")return s;
    s.mask=mode=="B2"?1:2;s.estimateId=e.estimateId;s.sampledUs=e.sampledUs;
    s.receivedUs=e.receivedUs;s.generatedUs=e.report.generatedUs;s.sequence=e.report.sequence;
    if(s.mask==1){s.reference=uint16_t(e.report.reference);s.generation=e.report.generation;s.decodeWaitUs=e.report.decodeWaitUs;}
    else{s.stage=e.report.taskStage;s.observationCaptureUs=e.report.observationCaptureUs;
        s.positionErrorMicro=e.report.positionErrorMicro;s.deadlineUs=e.report.taskDeadlineUs;}
    return s;
}
inline bool BaselineStateLive(const BaselineState& s,uint64_t now){
    return s.sequence&&s.receivedUs<=s.sampledUs&&s.sampledUs<=now&&s.generatedUs<=s.receivedUs&&now-s.generatedUs<250000;
}
inline double BaselineStateValue(const BaselineState& s,bool idr,uint64_t now){
    if(!BaselineStateLive(s,now)||!s.mask)return 1.;
    if(s.mask==1){
        const bool synchronized=s.reference==1&&s.generation>0;
        const double dependency=synchronized?1.:(idr?2.:.15);
        return dependency*(1.-.5*std::min(1.,s.decodeWaitUs/200000.));
    }
    // Task-only urgency heuristic. It never reads reference/decoded frame IDs,
    // controller truth, task completion truth, or the sender estimator's joint decision.
    const double stage=s.stage==0?2.:s.stage==1?1.:s.stage==2?1.5:2.;
    const uint64_t age=s.observationCaptureUs&&s.observationCaptureUs<=now?now-s.observationCaptureUs:200000;
    const double uncertainty=s.observationCaptureUs?std::min(1.,(s.positionErrorMicro+.3*age)/100000.):1.;
    const double deadline=s.deadlineUs>now&&s.deadlineUs-now<=5000000?.5:0.;
    return std::min(4.,stage+std::min(1.,age/200000.)+uncertainty+deadline);
}
inline std::vector<BaselineCandidate> StateBaselineCandidates(const BaselineInput& input,double value){
    if(!std::isfinite(value)||value<=0||value>4)throw std::runtime_error("invalid state value");
    auto plans=BaselineCandidates(input);
    if(value==1.)return plans; // Exact B1 compatibility, including floating-point order.
    const double normalizer=double(BaselineIp(input.bytes,2));
    for(auto& c:plans)c.cost=c.defer?value:value*(1.-c.success)+input.lambda*c.expectedIp/normalizer;
    return plans;
}
}
