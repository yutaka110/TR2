#include "../research/ReachBaselinePlanner.h"
#include "../research/ReachBaselineState.h"
#include "../research/ReachDeferredLog.h"
#include "../network/PacketPacer.h"
#include "../network/JitterBuffer.h"
#include <chrono>
#include <thread>
#include <iostream>
#include <limits>
int main(){unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
try{using namespace reach;
    {
        net::JitterBuffer jitter(0,8);net::CompletedFrame newer;newer.frameId=421;newer.streamId=7;newer.receiveTimeUs=100;
        check(jitter.PushFrame(std::move(newer)).incomingOlderThanReleasedFrame==0,"new frame admitted");
        net::CompletedFrame queued;check(jitter.TryPopReadyFrame(100,queued)&&queued.frameId==421,"newer AU released before decoder consumes it");
        net::CompletedFrame older;older.frameId=420;older.streamId=7;older.receiveTimeUs=101;
        const auto rejected=jitter.PushFrame(std::move(older));
        check(rejected.droppedFrames==1&&rejected.incomingOlderThanReleasedFrame==421,"older AU rejection retains actual release witness");
        net::CompletedFrame none;check(!jitter.TryPopReadyFrame(102,none),"rejected AU cannot enter decoder queue");
    }
    {
        const auto path=std::filesystem::temp_directory_path()/("reach_deferred_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".log");
        const std::string data(131077,'x');
        {DeferredLog log(200000);log.open(path);log.exceptions(std::ios::badbit|std::ios::failbit);log<<data;
            check(std::filesystem::file_size(path)==0,"deferred journal never writes during trial");log.close();}
        {std::ifstream in(path,std::ios::binary);std::string actual((std::istreambuf_iterator<char>(in)),{});check(actual==data,"chunked journal persists all bytes exactly");}
        std::filesystem::remove(path);
        {DeferredLog log(5);log.open(path);log.exceptions(std::ios::badbit|std::ios::failbit);bool rejected=false;try{log<<"123456";}catch(...){rejected=true;}check(rejected,"journal overflow fails instead of silent truncation");log.close();}
        check(std::filesystem::file_size(path)==5,"overflow keeps bounded prefix for invalid-run diagnosis");std::filesystem::remove(path);
    }
    {
        SenderStateEstimate e;e.estimateId=1;e.sampledUs=1000000;e.receivedUs=990000;
        e.report.sequence=7;e.report.generatedUs=980000;e.report.reference=ReceiverReference::RecoveryPending;
        e.report.generation=4;e.report.decodeWaitUs=40000;e.report.taskStage=2;
        e.report.observationCaptureUs=900000;e.report.positionErrorMicro=30000;e.report.taskDeadlineUs=60000000;
        const auto b2=ProjectBaselineState("B2",e),b3=ProjectBaselineState("B3",e);
        check(b2.stage==0&&b2.deadlineUs==0&&b2.observationCaptureUs==0&&b2.positionErrorMicro==0,"B2 physically masks task fields");
        check(b3.reference==0&&b3.generation==0&&b3.decodeWaitUs==0,"B3 physically masks decoder fields");
        check(ProjectBaselineState("B1",e).sequence==0,"B1 receives no state");
        check(BaselineStateValue(b2,false,1000000)<1&&BaselineStateValue(b2,true,1000000)>1,"unsynchronized decoder changes local IDR value");
        check(BaselineStateValue(b3,false,1000000)>1,"task urgency affects value");
        check(BaselineStateValue(b3,false,1230000)==1,"state expires at generation plus 250 ms");
        check(BaselineStateValue(b2,false,999999)==1,"future publication cannot influence past");
        for(unsigned i=0;i<40;++i){
            auto hidden=e;hidden.report.taskStage=i%4;hidden.report.positionErrorMicro=i*999;
            hidden.report.observationCaptureUs=900000+i*100;hidden.report.taskDeadlineUs+=i*1000;
            hidden.report.flags=i%8;hidden.report.xMicro=i*333;hidden.report.controlFrame=i;
            const auto masked=ProjectBaselineState("B2",hidden);
            check(BaselineStateValue(masked,false,1000000)==BaselineStateValue(b2,false,1000000),"hidden task values cannot change B2");
            hidden=e;hidden.report.reference=ReceiverReference(i%4);hidden.report.generation=i;
            hidden.report.decodedFrame=i;hidden.report.decodeWaitUs=i*2000;hidden.report.missingChunks=i;
            hidden.report.recoveryDeadlineUs+=i*1000;hidden.decision="reference_unconfirmed";
            check(BaselineStateValue(ProjectBaselineState("B3",hidden),false,1000000)==BaselineStateValue(b3,false,1000000),"hidden decoder fields and joint decision cannot change B3");
        }
        BaselineInput input{1000000,1000000,12000,0,10000,3500000,.1,.2,.3};
        const auto plain=BaselineCandidates(input),same=StateBaselineCandidates(input,1.);
        for(size_t i=0;i<plain.size();++i)check(plain[i].cost==same[i].cost&&plain[i].success==same[i].success,"B1 candidate identity");
        auto low=StateBaselineCandidates(input,.15),high=StateBaselineCandidates(input,4.);
        check(ChooseBaseline(low)!=ChooseBaseline(high),"state can change an action on identical network input");
        for(size_t i=0;i<plain.size();++i)check(plain[i].initialIp==high[i].initialIp&&plain[i].feasible==high[i].feasible&&plain[i].success==high[i].success,"state cannot alter action space or transport model");
    }
    check(BaselineProbeDue(1500000,1000000),"idle stream must explore after 500 ms");
    check(!BaselineProbeDue(1499999,1000000),"probe does not exceed interval");
    check(!BaselineProbeDue(999999,1000000),"future offer cannot trigger probe");
    check(BaselineQueueUs(0,100000)==0,"empty queue has no future delay");
    check(BaselineQueueUs(1200,100000)==96000,"queued bytes determine service time");
    check(BaselineQueueUs(1,3000000)==3,"queue estimate rounds up");
    {
        net::PacketPacer pacer;std::atomic<unsigned> sent=0;
        pacer.SetTargetBitrateBps(100000);pacer.SetEnabled(true);pacer.Start([&](std::vector<uint8_t>&&,const char*){++sent;});
        for(int i=0;i<5;++i)pacer.EnqueuePacket(std::vector<uint8_t>(1200),"queue regression",net::PacketPacingPriority::Normal,0);
        const auto queued=pacer.GetStats();check(queued.queuedPayloadBytes==queued.queuedPackets*1200ull,"snapshot sums real queued payload");
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(sent<5&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto empty=pacer.GetStats();pacer.Stop();check(sent==5,"real pacer drains");
        check(empty.queuedPayloadBytes==0&&empty.currentQueueDelayMs>0,"historical delay remains after backlog drains");
        check(BaselineQueueUs(empty.queuedPayloadBytes,empty.targetBitrateBps)==0,"historical delay cannot cause permanent defer");
    }
    check(BaselineIp(1200,0)==1272,"single IP length");check(BaselineIp(2400,2)==3824,"XOR wire lengths");check(BaselineIp(2401,2)==3897,"singleton tail has no parity");
    for(double p:{.001,.01,.1,.3,.6})for(uint64_t size:{1200,2400,3600,12000,64000}){
        BaselineInput input{1000000,1000000,size,0,10000,100000000,p,p,.1};auto c=BaselineCandidates(input);check(c.size()==13,"bounded action count");
        const auto best=ChooseBaseline(c);check(c[best].feasible,"chosen action feasible");
        for(auto a:c)check(!a.feasible||c[best].cost<=a.cost+1e-12,"global candidate minimum");
        check(std::abs(c[0].success-std::pow(1-p,double((size+1199)/1200)))<1e-12,"independent no-FEC probability");
        if(size==2400){const double q=std::pow(1-p,3)+3*p*std::pow(1-p,2);check(std::abs(c[3].success-q)<1e-12,"2+1 XOR matches at-most-one-loss binomial");}
    }
    BaselineInput in{1000000,700000,2400,0,40000,3500000,.1,.1,0};auto c=BaselineCandidates(in);check(c[ChooseBaseline(c)].defer,"expired candidates deferred");
    in.capture=in.now;in.lambda=3;c=BaselineCandidates(in);check(c[ChooseBaseline(c)].defer,"large cost may defer");
    in.lambda=0;c=BaselineCandidates(in);check(!c[ChooseBaseline(c)].defer,"zero cost chooses feasible delivery");
    const auto clean=c;in.burst=.9;c=BaselineCandidates(in);check(c[0].success<clean[0].success,"burst concentration affects estimate");
    for(int kind=0;kind<6;++kind){auto invalid=in;if(kind==0)invalid.bytes=0;if(kind==1)invalid.capture=invalid.now+1;if(kind==2)invalid.rateBps=0;if(kind==3)invalid.loss=2;if(kind==4)invalid.lambda=std::numeric_limits<double>::quiet_NaN();if(kind==5)invalid.bytes=786433;
        bool rejected=false;try{BaselineCandidates(invalid);}catch(...){rejected=true;}check(rejected,"invalid input rejected");}
    std::cout<<"PASS "<<checks<<" baseline planner checks\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
