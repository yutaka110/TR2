#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <stdexcept>

namespace reach {
inline bool BaselineProbeDue(uint64_t now,uint64_t lastOffer){return now>=lastOffer&&now-lastOffer>=500000;}
inline uint64_t BaselineQueueUs(uint64_t queuedPayloadBytes,uint32_t rateBps){
    if(rateBps<100000||queuedPayloadBytes>16777216)throw std::runtime_error("invalid baseline queue snapshot");
    return (queuedPayloadBytes*8000000+rateBps-1)/rateBps;
}
// Transport-only inputs: deliberately no task, pose, decoder state or trace.
struct BaselineInput {
    uint64_t now=0,capture=0,bytes=0,queueUs=0,rttUs=40000;
    uint32_t rateBps=3500000;
    double loss=.01,burst=.01,lambda=.1;
};
struct BaselineCandidate {
    uint16_t group=0,rounds=0;
    uint64_t initialIp=0;
    double expectedIp=0,success=0,cost=1;
    bool feasible=false,defer=false;
};
inline uint64_t BaselineIp(uint64_t bytes,uint16_t group){
    const uint64_t chunks=(bytes+1199)/1200;uint64_t ip=bytes+72*chunks;
    if(group)for(uint64_t i=0;i<chunks;i+=group){const auto n=std::min<uint64_t>(group,chunks-i);if(n>1)ip+=1200+72+8;}
    return ip;
}
// XOR groups are approximated independent; within-frame burst pressure inflates
// Bernoulli p. This is a declared engineering model, not Hairpin's RS MDP.
inline std::vector<BaselineCandidate> BaselineCandidates(const BaselineInput& in){
    if(!in.bytes||in.bytes>786432||in.rateBps<100000||in.capture>in.now||!std::isfinite(in.loss)||!std::isfinite(in.burst)||!std::isfinite(in.lambda)||in.loss<0||in.loss>1||in.burst<0||in.burst>1||in.lambda<0||in.lambda>3)throw std::runtime_error("invalid baseline input");
    const uint64_t chunks=(in.bytes+1199)/1200;
    const double p=std::clamp(in.loss+.25*std::max(0.,in.burst-in.loss),.00001,.75);
    const double normalizer=static_cast<double>(BaselineIp(in.bytes,2));std::vector<BaselineCandidate> result;
    for(uint16_t group:{0,2,4,8})for(uint16_t rounds=0;rounds<=2;++rounds){
        BaselineCandidate c;c.group=group;c.rounds=rounds;c.initialIp=BaselineIp(in.bytes,group);c.expectedIp=static_cast<double>(c.initialIp);
        for(unsigned j=1;j<=rounds;++j)c.expectedIp+=BaselineIp(in.bytes,0)*std::pow(p,j);
        const auto finish=in.now+in.queueUs+in.rttUs/2+8000+static_cast<uint64_t>(std::ceil(c.expectedIp*8000000/in.rateBps))+rounds*in.rttUs;
        c.feasible=finish<=in.capture+200000;
        const double residual=std::pow(p,rounds+1);c.success=1;
        for(uint64_t i=0;i<chunks;){const auto n=group?std::min<uint64_t>(group,chunks-i):chunks;
            double q=std::pow(1-residual,static_cast<double>(n));
            if(group&&n>1)q+=n*residual*std::pow(1-residual,static_cast<double>(n-1))*(1-p);
            c.success*=q;i+=n;
        }
        if(!c.feasible)c.success=0;
        c.cost=1-c.success+in.lambda*c.expectedIp/normalizer;result.push_back(c);
    }
    BaselineCandidate skip;skip.defer=true;skip.feasible=true;result.push_back(skip);return result;
}
inline size_t ChooseBaseline(const std::vector<BaselineCandidate>& candidates){
    size_t best=candidates.size()-1;
    for(size_t i=0;i<candidates.size();++i)if(candidates[i].feasible&&candidates[i].cost<candidates[best].cost-1e-12)best=i;
    return best;
}
}
