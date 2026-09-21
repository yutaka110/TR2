#include "../research/ReachCommandProtocol.h"
#include "../research/ReachRobotWorld.h"
#include "../research/ReachFoundation.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

void Seal(reach::CommandPacket& b){uint32_t crc=~uint32_t{0};for(int i=0;i<84;++i){crc^=b[i];for(int k=0;k<8;++k)crc=(crc>>1)^((crc&1)?0xedb88320:0);}crc=~crc;for(int i=0;i<4;++i)b[87-i]=uint8_t(crc>>(8*i));}
int main(int argc,char** argv){int checks=0;auto check=[&](bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);};
    try {
        using namespace reach;const auto session=ParseCommandSession("01234567-89AB-CDEF-0123-456789ABCDEF");
        check(session[0]==1&&session[15]==239,"canonical GUID byte order");
        bool malformedGuid=false;try{ParseCommandSession("-1234567-89AB-CDEF-0123-456789ABCDEF");}catch(...){malformedGuid=true;}
        check(malformedGuid,"extra GUID separator rejected");
        MotionCommand c;c.sequence=1;c.generatedUs=1000000;c.validUntilUs=1100000;c.sourceCaptureUs=950000;c.sourceFrameId=9;c.sourceStreamId=3;c.state="APPROACH";c.reason="image_feedback";c.v=.3;c.w=-.8;
        auto bytes=EncodeCommand(session,c);check(bytes.size()==88&&bytes[0]=='R'&&bytes[3]=='D'&&bytes[4]==0&&bytes[5]==1&&bytes[31]==1,"fixed big endian wire header");
        MotionCommand d;CommandSessionId id{};std::string reason;
        check(DecodeCommand(bytes,id,d,reason)&&id==session&&d.sequence==1&&d.generatedUs==c.generatedUs&&d.sourceFrameId==9&&d.v==.3&&d.w==-.8,"wire round trip with negative angular speed");
        auto bad=bytes;bad[64]^=1;check(!DecodeCommand(bad,id,d,reason)&&reason=="invalid_crc","corrupted datagram rejected");
        check(!DecodeCommand(std::span(bytes).first(87),id,d,reason)&&reason=="invalid_length","truncated datagram rejected");
        std::vector<uint8_t> longPacket(bytes.begin(),bytes.end());longPacket.push_back(0);check(!DecodeCommand(longPacket,id,d,reason),"trailing bytes rejected");
        bad=bytes;bad[5]=2;Seal(bad);check(!DecodeCommand(bad,id,d,reason)&&reason=="unknown_version","unknown version rejected");
        bad=bytes;bad[80]=1;Seal(bad);check(!DecodeCommand(bad,id,d,reason)&&reason=="reserved_bits","reserved bits rejected");
        bad=bytes;bad[73]=10;Seal(bad);check(!DecodeCommand(bad,id,d,reason)&&reason=="unknown_enum","unknown state rejected");
        bad=bytes;bad[64]=0x7f;Seal(bad);check(!DecodeCommand(bad,id,d,reason)&&reason=="invalid_values","overspeed wire rejected");
        for(double value:{std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),-.01,.31}){auto invalid=c;invalid.v=value;bool threw=false;try{EncodeCommand(session,invalid);}catch(...){threw=true;}check(threw,"nonfinite or out of range API rejected");}
        auto invalid=c;invalid.validUntilUs++;bool threw=false;try{EncodeCommand(session,invalid);}catch(...){threw=true;}check(threw,"extended lifetime rejected");
        CommandGuard guard(session,900000);check(guard.At(900000).reason=="awaiting_command","startup is stopped");
        check(guard.Receive(bytes,1000010)=="accepted","fresh command accepted");
        check(guard.At(1000020).live&&guard.At(1000020).command.v==.3,"receiver applies accepted packet");
        check(guard.Receive(bytes,1050000)=="duplicate"&&guard.LastAcceptedUs()==1000010,"duplicate does not renew watchdog");
        check(guard.At(1099999).live&&!guard.At(1100000).live&&guard.At(1100000).reason=="command_expired","absolute deadline boundary not receive-relative");
        check(guard.Receive(bytes,1200000)=="duplicate","late duplicate cannot revive command");
        check(guard.At(1250010).reason=="watchdog_timeout"&&guard.At(1250010).command.v==0,"watchdog boundary counts only accepted receive");
        c.sequence=3;c.generatedUs=1260000;c.validUntilUs=1360000;c.sourceCaptureUs=1250000;auto fresh=EncodeCommand(session,c);
        check(guard.Receive(fresh,1260010)=="accepted"&&guard.At(1260011).live,"fresh command recovers after outage");
        c.sequence=2;check(guard.Receive(EncodeCommand(session,c),1260020)=="reordered"&&guard.AcceptedSequence()==3,"late older sequence cannot override new command");
        auto foreign=session;foreign[0]^=1;c.sequence=1000;
        check(guard.Receive(EncodeCommand(foreign,c),1260030)=="wrong_session"&&guard.HighestSequence()==3,"foreign session cannot poison sequence");
        c.generatedUs=1500000;c.validUntilUs=1600000;c.sourceCaptureUs=1490000;auto future=EncodeCommand(session,c);
        check(guard.Receive(future,1260040)=="future_command"&&guard.HighestSequence()==3,"future command cannot poison sequence");
        c.sequence=4;c.generatedUs=1270000;c.validUntilUs=1370000;c.sourceCaptureUs=1260000;
        check(guard.Receive(EncodeCommand(session,c),1400000)=="expired_on_arrival"&&guard.LastAcceptedUs()==1260010&&guard.HighestSequence()==4,"expired packet advances ordering without heartbeat");
        check(guard.At(1510010).reason=="watchdog_timeout","expired arrivals do not suppress watchdog");
        check(guard.Receive(bytes,1500000)=="clock_reversed","clock reversal rejected");
        CommandGuard noPackets(session,1000000);check(noPackets.At(1250000).reason=="watchdog_timeout","startup without packets times out");
        // Fault cannot change a valid stop into movement or renew a superseded command.
        c.sequence=5;c.generatedUs=1520000;c.validUntilUs=1620000;c.sourceCaptureUs=1510000;c.v=c.w=0;c.state="OBSERVE";c.reason="marker_missing";
        check(guard.Receive(EncodeCommand(session,c),1520010)=="accepted"&&guard.At(1520011).command.v==0,"fresh explicit stop accepted");
        RobotWorld robot("T2");robot.Command(.3,.8);for(int i=0;i<100;++i)robot.Step(.01);
        c.sequence=1;c.generatedUs=2000000;c.validUntilUs=2100000;c.sourceCaptureUs=1990000;c.v=.3;c.w=.8;c.state="APPROACH";c.reason="image_feedback";
        CommandGuard brake(session,2000000);brake.Receive(EncodeCommand(session,c),2000000);
        std::ofstream log(argc>1?argv[1]:"command_braking_unit.csv");log.precision(12);log<<"tick,time_us,reason,target_v,target_w,v,w\n";
        double distance=0;bool gradual=false,watchdogWhileMoving=false;
        for(int tick=1;tick<=80;++tick){const uint64_t now=2000000+tick*10000;const auto a=brake.At(now);const auto before=robot.Truth();robot.Command(a.command.v,a.command.w);robot.Step(.01);const auto& t=robot.Truth();
            if(!a.live){distance+=(before.v+t.v)*.005;check(std::abs(t.v-std::max(0.,before.v-.006))<1e-10,"linear brake is 0.60 m/s2");check(std::abs(t.w-std::max(0.,before.w-.016))<1e-10,"angular deceleration is 1.6 rad/s2");}
            if(tick==10)gradual=t.v>.29&&t.v<.3;if(a.reason=="watchdog_timeout"&&t.v>0)watchdogWhileMoving=true;
            log<<tick<<','<<now<<','<<a.reason<<','<<a.command.v<<','<<a.command.w<<','<<t.v<<','<<t.w<<'\n';
        }
        check(gradual&&watchdogWhileMoving,"deadline and watchdog do not teleport velocity");
        check(robot.Truth().v==0&&robot.Truth().w==0&&std::abs(distance-.075)<1e-10,"full-speed braking distance 7.5cm and complete stop");
        auto cfg=FoundationConfig::Load("config/reach_rt_g1_command.json");check(FoundationConfig::Parse(cfg.EffectiveJson()).stage=="command_udp","command config roundtrip");
        cfg.commandScenario="typo";threw=false;try{FoundationConfig::Parse(cfg.EffectiveJson());}catch(...){threw=true;}check(threw,"unknown fault profile rejected");
        std::cout<<"PASS "<<checks<<" assertions; 0.30 m/s braking path 0.075 m; 0.60 m/s2 and angular 1.6 rad/s2\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
}
