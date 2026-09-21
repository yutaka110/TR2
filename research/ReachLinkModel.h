#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <vector>

namespace reach {
struct CapacityPoint { uint64_t atUs=0,bps=0; };
struct LinkConfig {
    uint64_t queueBytes=16384;
    std::vector<CapacityPoint> capacity{{0,3000000}};
    void Validate() const {
        if(queueBytes<28||queueBytes>16777216||capacity.empty()||capacity.size()>128||capacity.front().atUs!=0)
            throw std::runtime_error("invalid link queue/trace");
        for(size_t i=0;i<capacity.size();++i)
            if(capacity[i].bps>1000000000||capacity[i].atUs>3600000000ULL||(i&&capacity[i].atUs<=capacity[i-1].atUs))
                throw std::runtime_error("invalid link capacity point");
    }
};
struct LinkPacket {
    uint64_t id=0,enqueuedUs=0,startedUs=0,completedUs=0,ipBytes=0,remaining=0;
    std::vector<uint8_t> payload;
};
// Deterministic FIFO server; time is relative to the shared experiment origin.
// Work unit = bit * 1,000,000. At bps bits/s, one microsecond serves bps units.
// A packet completes at the first integer microsecond with sufficient service.
// Occupancy includes the complete IP length of the packet currently in service.
class LinkModel {
public:
    explicit LinkModel(LinkConfig config):config_(std::move(config)){config_.Validate();}
    std::vector<LinkPacket> Advance(uint64_t now){
        if(now<cursor_||now>3601000000ULL)throw std::runtime_error("link clock outside monotonic horizon");
        std::vector<LinkPacket> completed;
        while(cursor_<now){
            while(segment_+1<config_.capacity.size()&&config_.capacity[segment_+1].atUs<=cursor_)++segment_;
            if(queue_.empty()){cursor_=now;break;} // Idle time never creates future credit.
            const auto end=segment_+1<config_.capacity.size()?(std::min)(now,config_.capacity[segment_+1].atUs):now;
            const auto rate=config_.capacity[segment_].bps;
            if(!rate){cursor_=end;continue;}
            auto& p=queue_.front();
            const auto needed=p.remaining/rate+(p.remaining%rate!=0);
            if(needed<=end-cursor_){
                cursor_+=needed;p.completedUs=cursor_;p.remaining=0;
                occupied_-=p.ipBytes;completed.push_back(std::move(p));queue_.pop_front();
                if(!queue_.empty())queue_.front().startedUs=cursor_;
            }else{p.remaining-=rate*(end-cursor_);cursor_=end;}
        }
        return completed;
    }
    // Caller must Advance(now) first, handling completions before same-time arrivals.
    bool Offer(uint64_t id,uint64_t now,std::vector<uint8_t> payload){
        if(now!=cursor_||payload.size()>65507)throw std::runtime_error("invalid link arrival");
        const uint64_t bytes=payload.size()+28;
        if(bytes>config_.queueBytes-occupied_)return false; // Tail drop; no eviction.
        LinkPacket p;p.id=id;p.enqueuedUs=now;p.startedUs=queue_.empty()?now:0;
        p.ipBytes=bytes;p.remaining=bytes*8*1000000;p.payload=std::move(payload);
        occupied_+=bytes;maximum_=(std::max)(maximum_,occupied_);queue_.push_back(std::move(p));return true;
    }
    std::vector<LinkPacket> Cancel(){std::vector<LinkPacket> result;while(!queue_.empty()){result.push_back(std::move(queue_.front()));queue_.pop_front();}occupied_=0;return result;}
    uint64_t OccupiedBytes() const{return occupied_;}
    uint64_t MaximumBytes() const{return maximum_;}
    size_t Pending() const{return queue_.size();}
private:
    LinkConfig config_;
    std::deque<LinkPacket> queue_;
    uint64_t cursor_=0,occupied_=0,maximum_=0;
    size_t segment_=0;
};
}
