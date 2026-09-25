#pragma once
#include "../network/PacketProtocol.h"
#include <array>
#include <span>
#include <stdexcept>
#include <string>
namespace reach {
struct BudgetLimit {
    uint64_t rateBps=8000000,burstBytes=65536,maxBytes=60000000;
    void Validate() const {if(rateBps>1000000000||burstBytes<28||burstBytes>16777216||maxBytes>1000000000000ULL)throw std::runtime_error("invalid IP budget limit");}
};
struct IpBudgetConfig {
    BudgetLimit total,up,down;uint16_t fecGroup=4;
    void Validate() const {total.Validate();up.Validate();down.Validate();if(fecGroup!=0&&fecGroup!=4&&fecGroup!=8&&fecGroup!=16)throw std::runtime_error("invalid budget FEC group");}
};
// All-or-none admission across aggregate and direction. Work = IP bytes*8e6.
// Caller serializes concurrent access, including the actual UDP send.
class IpBudget {
    struct Bucket {BudgetLimit limit;uint64_t credit=0,used=0;};
    std::array<Bucket,3> buckets_;uint64_t at_=0;
public:
    explicit IpBudget(const IpBudgetConfig& c):buckets_{{{c.total,c.total.burstBytes*8000000,0},{c.up,c.up.burstBytes*8000000,0},{c.down,c.down.burstBytes*8000000,0}}}{c.Validate();}
    std::string Admit(uint64_t now,bool uplink,uint64_t bytes){
        if(now<at_||now>3601000000ULL||bytes<28||bytes>65535)throw std::runtime_error("invalid IP budget event");
        for(auto& b:buckets_){const auto gap=b.limit.burstBytes*8000000-b.credit;
            b.credit+=(std::min)(gap,b.limit.rateBps*(now-at_));}at_=now;
        const size_t direction=uplink?1:2;const auto cost=bytes*8000000;
        for(auto i:{size_t(0),direction}){const auto& b=buckets_[i];
            if(bytes>b.limit.maxBytes-b.used)return i==0?"total_byte_limit":"direction_byte_limit";
            if(cost>b.credit)return i==0?"total_rate_limit":"direction_rate_limit";}
        for(auto i:{size_t(0),direction}){buckets_[i].credit-=cost;buckets_[i].used+=bytes;}
        return "allowed";
    }
    uint64_t Used(size_t i)const{return buckets_.at(i).used;}
};
inline std::string IpPacketClass(std::span<const uint8_t> data){
    if(data.size()>=4&&std::equal(data.begin(),data.begin()+4,"RSTA"))return "state_notification";
    if(data.size()==88&&std::equal(data.begin(),data.begin()+4,"RCMD"))return "command";
    net::RnvpHeaderV1 h{};if(!net::DecodeRnvpHeaderV1(data.data(),data.size(),h))return "unknown";
    const auto type=static_cast<net::PacketType>(h.packetType);
    if(h.flags&net::PacketFlag_Retransmit)return "retransmission";
    switch(type){
        case net::PacketType::Data:return h.flags&net::PacketFlag_KeyFrame?"video_idr":"video_delta";
        case net::PacketType::Fec:return "fec";
        case net::PacketType::Ack:{net::AckPayload ack{};if(!net::DecodeAckPayload(data.data()+h.headerSize,h.payloadSize,ack))return "unknown";return ack.missingChunkCount?"nack":"ack";}
        case net::PacketType::Control:return "control";
        case net::PacketType::TransportFeedback:return "transport_feedback";
        case net::PacketType::Ping:return "ping";
        case net::PacketType::Pong:return "pong";
    }return "unknown";
}
}
