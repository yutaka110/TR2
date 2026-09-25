#include "../research/ReachStateProtocol.h"
#include <iostream>
int main(){
    unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
    try{
        using namespace reach;CommandSessionId session{};session[0]=1;
        StateReport r;r.sequence=1;r.generatedUs=1100000;r.taskDeadlineUs=5000000;r.remainingTaskUs=3900000;r.stream=1;
        r.completeFrame=8;r.decodedFrame=7;r.decodedCaptureUs=1050000;r.generation=2;r.reference=ReceiverReference::Synchronized;
        r.observationFrame=7;r.observationCaptureUs=1050000;r.observationReceivedUs=1090000;r.flags=1;r.xMicro=1000000;r.yMicro=-50000;r.yawMicro=-1000;r.positionErrorMicro=20000;r.yawErrorMicro=30000;
        auto packet=EncodeState(session,r);CommandSessionId decodedSession;StateReport decoded;std::string reason;
        check(packet.size()==208&&DecodeState(packet,decodedSession,decoded,reason)&&decodedSession==session&&decoded.yMicro==-50000&&decoded.generation==2,"wire roundtrip with signed pose");
        SenderStateEstimator estimator(session,1000000,4000000);
        check(estimator.At(1100000).report.sequence==0,"unsent receiver state must remain unknown");
        check(estimator.Receive(packet,1150000)=="accepted","valid notification accepted");
        auto e=estimator.At(1150000);check(e.report.decodedFrame==7&&e.ageUs==50000&&e.observationAgeUs==100000&&e.observationUsable&&e.referenceReportedSynchronized,"two independent ages");
        check(e.positionRadiusMicro==50000&&e.yawRadiusMicro==110000,"reachable uncertainty envelope");
        check(estimator.Receive(packet,1150000)=="duplicate","duplicate does not refresh time");
        e=estimator.At(1250000);check(e.observationUsable&&e.positionRadiusMicro==80000,"200ms observation boundary");
        check(!estimator.At(1250001).observationUsable,"expired observation rejected while report still live");
        e=estimator.At(1350000);check(!e.notificationLive&&!e.referenceReportedSynchronized&&e.decision==std::string("notification_stale"),"250ms notification boundary");
        check(e.report.sequence==1&&e.positionRadiusMicro==110000,"silence grows uncertainty but never invents state");
        auto expired=r;expired.sequence=2;auto old=EncodeState(session,expired);
        check(estimator.Receive(old,1350000)=="expired_on_arrival"&&estimator.AcceptedSequence()==1&&estimator.HighestSequence()==2,"expired packet cannot revive liveness");
        check(estimator.Receive(packet,1350000)=="reordered","old packet cannot overwrite newer sequence");
        auto fresh=r;fresh.sequence=3;fresh.generatedUs=1400000;fresh.remainingTaskUs=3600000;fresh.flags=0;
        check(estimator.Receive(EncodeState(session,fresh),1410000)=="accepted","fresh report recovers after silence");
        auto foreign=session;foreign[1]=1;fresh.sequence=999;
        check(estimator.Receive(EncodeState(foreign,fresh),1410000)=="wrong_session"&&estimator.HighestSequence()==3,"wrong session cannot poison ordering");
        fresh.generatedUs=2000000;fresh.remainingTaskUs=3000000;
        check(estimator.Receive(EncodeState(session,fresh),1410000)=="future_notification"&&estimator.HighestSequence()==3,"future high sequence cannot poison ordering");
        auto invalid=packet;invalid[128]^=1;check(!DecodeState(invalid,decodedSession,decoded,reason)&&reason=="invalid_crc","CRC rejects corrupt pose");
        check(!DecodeState(std::span(packet).first(80),decodedSession,decoded,reason)&&reason=="invalid_length","short notification rejected");
        for(auto offset:{4,6,72,74,76,188}){
            invalid=packet;invalid[offset]=255;state_wire::Put(invalid,204,state_wire::Crc(std::span(invalid).first(204)),4);
            check(!DecodeState(invalid,decodedSession,decoded,reason),"unknown schema enum or capability rejected");
        }
        invalid=packet;state_wire::Put(invalid,128,0x7fffffff,4);state_wire::Put(invalid,204,state_wire::Crc(std::span(invalid).first(204)),4);
        check(!DecodeState(invalid,decodedSession,decoded,reason),"out of range pose rejected");
        check(estimator.Receive(packet,1400000)=="clock_reversed","clock reversal fails closed");
        SenderStateEstimator a(session,1000000,4000000),b(session,1000000,4000000);
        a.Receive(packet,1150000);b.Receive(packet,1150000);
        // Different local/hidden receiver state is not an input of either estimator.
        for(uint64_t now=1150000;now<4000000;now+=50000){auto x=a.At(now),y=b.At(now);check(x.report.sequence==y.report.sequence&&x.positionRadiusMicro==y.positionRadiusMicro&&x.notificationLive==y.notificationLive,"equal arrival prefixes produce equal sender estimates");}
        check(!a.At(5100000).remainingUs,"remaining task time uses local session contract");
        std::cout<<"PASS "<<checks<<" state protocol and causal estimator checks\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
}
