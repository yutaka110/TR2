#pragma once
#include "ReachCommandProtocol.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace reach {
constexpr size_t StatePacketBytes=208;
constexpr uint64_t StateNotificationLifetimeUs=250000,StateNotificationPeriodUs=50000;
enum class ReceiverReference:uint16_t {AwaitingRandomAccess, Synchronized, ReferenceUncertain, RecoveryPending};
struct StateReport {
    uint64_t sequence=0,generatedUs=0,taskDeadlineUs=0;
    uint32_t stream=0,completeFrame=0,decodedFrame=0,controlFrame=0;
    uint64_t generation=0;
    ReceiverReference reference=ReceiverReference::AwaitingRandomAccess;
    uint16_t taskStage=0;uint32_t flags=0;
    uint64_t decodedCaptureUs=0,decodeWaitUs=0;
    uint32_t observationFrame=0,missingFrame=0;
    uint64_t observationCaptureUs=0,observationReceivedUs=0,stateEventUs=0;
    int32_t xMicro=0,yMicro=0,yawMicro=0;uint32_t positionErrorMicro=0,yawErrorMicro=0,missingChunks=0;
    uint64_t missingSnapshotUs=0,recoveryDeadlineUs=0,controlGeneratedUs=0,controlSequence=0;
    uint32_t commandSourceFrame=0,capabilities=7;
    uint64_t remainingTaskUs=0;
    uint32_t receiverEventSequence=0;
};
using StatePacket=std::array<uint8_t,StatePacketBytes>;
namespace state_wire {
inline void Put(StatePacket& b,size_t at,uint64_t n,size_t size){for(size_t i=0;i<size;++i)b[at+size-i-1]=static_cast<uint8_t>(n>>(i*8));}
inline uint64_t Get(std::span<const uint8_t> b,size_t at,size_t size){uint64_t n=0;for(size_t i=0;i<size;++i)n=(n<<8)|b[at+i];return n;}
inline uint32_t Crc(std::span<const uint8_t> b){uint32_t crc=0xffffffff;for(auto v:b){crc^=v;for(int i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320:0);}return ~crc;}
inline bool Values(const StateReport& r){
    if(!r.sequence||!r.generatedUs||r.generatedUs>UINT64_MAX-StateNotificationLifetimeUs||!r.stream||!r.taskDeadlineUs||r.capabilities!=7||
       static_cast<uint16_t>(r.reference)>3||r.taskStage>3||r.flags>7||r.remainingTaskUs!=(r.taskDeadlineUs>r.generatedUs?r.taskDeadlineUs-r.generatedUs:0))return false;
    for(auto t:{r.decodedCaptureUs,r.observationCaptureUs,r.observationReceivedUs,r.stateEventUs,r.missingSnapshotUs,r.controlGeneratedUs})if(t>r.generatedUs)return false;
    if((!r.decodedFrame)!=(!r.decodedCaptureUs)||(!r.observationFrame)!=(!r.observationCaptureUs)||(!r.observationFrame)!=(!r.observationReceivedUs)||
       r.observationCaptureUs>r.observationReceivedUs||(!r.controlSequence)!=(!r.controlGeneratedUs))return false;
    if(std::abs(int64_t(r.xMicro))>100000000||std::abs(int64_t(r.yMicro))>100000000||std::abs(int64_t(r.yawMicro))>3141593||
       r.positionErrorMicro>10000000||r.yawErrorMicro>6283186||r.missingChunks>65535||r.decodeWaitUs>60000000)return false;
    if((r.flags&1)&&(!r.observationFrame||r.generatedUs-r.observationCaptureUs>200000))return false;
    if((r.flags&2)&&(!(r.flags&1)||r.taskStage!=3))return false;
    if(r.reference==ReceiverReference::Synchronized&&!r.generation)return false;
    if((r.flags&4)&&(!r.missingFrame||!r.missingSnapshotUs))return false;
    return true;
}
}
inline StatePacket EncodeState(const CommandSessionId& session,const StateReport& r){
    if(!state_wire::Values(r)||std::all_of(session.begin(),session.end(),[](auto b){return b==0;}))throw std::runtime_error("invalid state report");
    StatePacket b{};auto put=[&](size_t at,uint64_t n,size_t size){state_wire::Put(b,at,n,size);};
    put(0,0x52535441,4);put(4,1,2);put(6,b.size(),2);std::copy(session.begin(),session.end(),b.begin()+8);
    put(24,r.sequence,8);put(32,r.generatedUs,8);put(40,r.taskDeadlineUs,8);put(48,r.stream,4);put(52,r.completeFrame,4);put(56,r.decodedFrame,4);put(60,r.controlFrame,4);
    put(64,r.generation,8);put(72,static_cast<uint16_t>(r.reference),2);put(74,r.taskStage,2);put(76,r.flags,4);put(80,r.decodedCaptureUs,8);put(88,r.decodeWaitUs,8);
    put(96,r.observationFrame,4);put(100,r.missingFrame,4);put(104,r.observationCaptureUs,8);put(112,r.observationReceivedUs,8);put(120,r.stateEventUs,8);
    put(128,std::bit_cast<uint32_t>(r.xMicro),4);put(132,std::bit_cast<uint32_t>(r.yMicro),4);put(136,std::bit_cast<uint32_t>(r.yawMicro),4);
    put(140,r.positionErrorMicro,4);put(144,r.yawErrorMicro,4);put(148,r.missingChunks,4);put(152,r.missingSnapshotUs,8);put(160,r.recoveryDeadlineUs,8);
    put(168,r.controlGeneratedUs,8);put(176,r.controlSequence,8);put(184,r.commandSourceFrame,4);put(188,r.capabilities,4);put(192,r.remainingTaskUs,8);put(200,r.receiverEventSequence,4);put(204,state_wire::Crc(std::span(b).first(204)),4);return b;
}
inline bool DecodeState(std::span<const uint8_t> b,CommandSessionId& session,StateReport& out,std::string& reason){
    auto reject=[&](const char* why){reason=why;return false;};auto get=[&](size_t at,size_t n){return state_wire::Get(b,at,n);};
    if(b.size()!=StatePacketBytes)return reject("invalid_length");
    if(get(0,4)!=0x52535441)return reject("invalid_magic");if(get(4,2)!=1)return reject("unknown_version");if(get(6,2)!=StatePacketBytes)return reject("invalid_length");
    if(get(204,4)!=state_wire::Crc(b.first(204)))return reject("invalid_crc");
    StateReport r;
    r.sequence=get(24,8);r.generatedUs=get(32,8);r.taskDeadlineUs=get(40,8);r.stream=uint32_t(get(48,4));r.completeFrame=uint32_t(get(52,4));r.decodedFrame=uint32_t(get(56,4));r.controlFrame=uint32_t(get(60,4));
    r.generation=get(64,8);r.reference=static_cast<ReceiverReference>(get(72,2));r.taskStage=uint16_t(get(74,2));r.flags=uint32_t(get(76,4));r.decodedCaptureUs=get(80,8);r.decodeWaitUs=get(88,8);
    r.observationFrame=uint32_t(get(96,4));r.missingFrame=uint32_t(get(100,4));r.observationCaptureUs=get(104,8);r.observationReceivedUs=get(112,8);r.stateEventUs=get(120,8);
    r.xMicro=std::bit_cast<int32_t>(uint32_t(get(128,4)));r.yMicro=std::bit_cast<int32_t>(uint32_t(get(132,4)));r.yawMicro=std::bit_cast<int32_t>(uint32_t(get(136,4)));
    r.positionErrorMicro=uint32_t(get(140,4));r.yawErrorMicro=uint32_t(get(144,4));r.missingChunks=uint32_t(get(148,4));r.missingSnapshotUs=get(152,8);r.recoveryDeadlineUs=get(160,8);
    r.controlGeneratedUs=get(168,8);r.controlSequence=get(176,8);r.commandSourceFrame=uint32_t(get(184,4));r.capabilities=uint32_t(get(188,4));r.remainingTaskUs=get(192,8);r.receiverEventSequence=uint32_t(get(200,4));
    if(!state_wire::Values(r))return reject("invalid_values");std::copy_n(b.begin()+8,16,session.begin());out=r;reason="decoded";return true;
}
struct SenderStateEstimate {
    uint64_t estimateId=0,sampledUs=0; // Local provenance, never serialized into RSTA.
    StateReport report;uint64_t receivedUs=0,ageUs=0,observationAgeUs=0,remainingUs=0,positionRadiusMicro=0,yawRadiusMicro=0;
    bool notificationLive=false,referenceReportedSynchronized=false,observationUsable=false;
    const char* decision="awaiting_notification";
};
// Sender-owned: only received bytes, local clock, and the session contract enter.
// No ReceiverStateTracker, journal, simulator, or future trace input is available.
class SenderStateEstimator {
    CommandSessionId session_;uint64_t origin_,deadline_,lastNow_,highest_=0,highestGenerated_=0,received_=0;StateReport accepted_;
public:
    SenderStateEstimator(CommandSessionId session,uint64_t origin,uint64_t duration):session_(session),origin_(origin),deadline_(origin+duration),lastNow_(origin){}
    std::string Receive(std::span<const uint8_t> bytes,uint64_t now){
        if(now<lastNow_)return "clock_reversed";lastNow_=now;
        StateReport r;CommandSessionId session;std::string reason;
        if(!DecodeState(bytes,session,r,reason))return reason;
        if(session!=session_)return "wrong_session";
        if(r.generatedUs>now)return "future_notification";
        if(r.generatedUs<origin_||r.taskDeadlineUs!=deadline_)return "session_contract_mismatch";
        if(r.sequence==highest_)return "duplicate";if(r.sequence<highest_)return "reordered";
        if(r.generatedUs<highestGenerated_)return "generation_reversed";
        highest_=r.sequence;highestGenerated_=r.generatedUs;
        if(now-r.generatedUs>=StateNotificationLifetimeUs)return "expired_on_arrival";
        accepted_=r;received_=now;return "accepted";
    }
    SenderStateEstimate At(uint64_t now){
        SenderStateEstimate e;e.sampledUs=now;e.report=accepted_;e.receivedUs=received_;
        if(now<lastNow_){e.decision="clock_reversed";return e;}lastNow_=now;
        e.remainingUs=deadline_>now?deadline_-now:0;
        if(!accepted_.sequence)return e;
        e.ageUs=now-accepted_.generatedUs;e.notificationLive=e.ageUs<StateNotificationLifetimeUs;
        e.referenceReportedSynchronized=e.notificationLive&&accepted_.reference==ReceiverReference::Synchronized;
        if(accepted_.observationCaptureUs){
            e.observationAgeUs=now-accepted_.observationCaptureUs;
            // Reachable error envelope around the last received pose, not a probability.
            e.positionRadiusMicro=accepted_.positionErrorMicro+(3*e.observationAgeUs+9)/10;
            e.yawRadiusMicro=accepted_.yawErrorMicro+(4*e.observationAgeUs+4)/5;
        }
        e.observationUsable=e.notificationLive&&(accepted_.flags&1)&&e.observationAgeUs<=200000;
        e.decision=!e.remainingUs?"task_deadline":!e.notificationLive?"notification_stale":!e.referenceReportedSynchronized?"reference_unconfirmed":!e.observationUsable?"observation_unavailable":"reported_state_usable";
        return e;
    }
    uint64_t HighestSequence()const{return highest_;}
    uint64_t AcceptedSequence()const{return accepted_.sequence;}
};
}
