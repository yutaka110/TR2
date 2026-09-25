#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include "ReachStateFeedbackUdp.h"
#include "ReachBudgetTransport.h"
#include "ReachFoundation.h"
#include "ReachJson.h"
#include <map>
#include <thread>
#pragma comment(lib,"ws2_32.lib")
namespace reach {
namespace {
void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string Hex(std::span<const uint8_t> data){std::string s;for(auto b:data){s+="0123456789abcdef"[b>>4];s+="0123456789abcdef"[b&15];}return s;}
}
struct StateFeedbackUdp::Impl {
    SOCKET tx=INVALID_SOCKET,rx=INVALID_SOCKET;bool wsa=false,finished=false;uint16_t port=0,sourcePort=0;sockaddr_in destination{};
    CommandSessionId session;std::shared_ptr<BudgetTransport> budget;std::unique_ptr<SenderStateEstimator> estimator;
    std::filesystem::path directory;BufferedLog txLog,rxLog,estimates;std::map<std::string,uint64_t> statuses;
    uint64_t sequence=0,lastGenerated=0,received=0,sent=0,rejected=0,suppressed=0,estimateSequence=0;std::string diagnostic="normal";
    Impl(const std::string& id,const std::filesystem::path& dir,std::shared_ptr<BudgetTransport> b):session(ParseCommandSession(id)),budget(std::move(b)),directory(dir){
        Require(budget!=nullptr,"state notifications require common budget");
        try{
            WSADATA w{};Require(WSAStartup(MAKEWORD(2,2),&w)==0,"state WSAStartup failed");wsa=true;
            auto open=[&](SOCKET& socket){
                socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);Require(socket!=INVALID_SOCKET,"state socket failed");
                sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
                Require(bind(socket,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0,"state bind failed");
                int len=sizeof(local),buffer=4194304;u_long nonblocking=1;
                Require(getsockname(socket,reinterpret_cast<sockaddr*>(&local),&len)==0&&ioctlsocket(socket,FIONBIO,&nonblocking)==0&&setsockopt(socket,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&buffer),sizeof(buffer))==0,"state socket setup failed");return ntohs(local.sin_port);
            };
            open(tx);port=open(rx);
            char value[32]{};auto n=GetEnvironmentVariableA("TR2_REACH_STATE_DIAGNOSTIC",value,32);
            Require(n<32,"invalid state diagnostic");if(n)diagnostic=value;
            Require(diagnostic=="normal"||diagnostic=="duplicate"||diagnostic=="invalid","unknown state diagnostic");
            for(auto p:{std::pair{&txLog,"state_tx.csv"},std::pair{&rxLog,"state_rx.csv"},std::pair{&estimates,"sender_estimates.csv"}}){p.first->open(dir/p.second);p.first->exceptions(std::ios::badbit|std::ios::failbit);}
            txLog<<"sent_us,sequence,generated_us,status,wire_hex\n";
            rxLog<<"received_us,status,highest_sequence,accepted_sequence,wire_hex\n";
            estimates<<"estimate_id,event_us,accepted_sequence,received_us,generated_us,notification_age_us,notification_live,reported_reference,generation,complete_frame,decoded_frame,control_frame,observation_frame,observation_age_us,observation_usable,reference_reported_synchronized,x_micro,y_micro,yaw_micro,position_radius_micro,yaw_radius_micro,remaining_task_us,decision\n";
        }catch(...){Close();throw;}
    }
    void Close(){if(tx!=INVALID_SOCKET){closesocket(tx);tx=INVALID_SOCKET;}if(rx!=INVALID_SOCKET){closesocket(rx);rx=INVALID_SOCKET;}if(wsa){WSACleanup();wsa=false;}}
    ~Impl(){Close();}
    void Transmit(std::span<const uint8_t> packet,const StateReport& source){
        auto now=MonotonicUs();const int n=budget->Send(false,tx,packet,destination);Require(n==0||n==int(packet.size()),"state sendto failed");
        if(n)++sent;else++rejected;
        txLog<<now<<','<<source.sequence<<','<<source.generatedUs<<','<<(n?"sent":"budget_rejected")<<','<<Hex(packet)<<'\n';
    }
};
StateFeedbackUdp::StateFeedbackUdp(const std::string& id,const std::filesystem::path& dir,std::shared_ptr<BudgetTransport> b):impl_(std::make_unique<Impl>(id,dir,std::move(b))){}
StateFeedbackUdp::~StateFeedbackUdp()=default;
uint16_t StateFeedbackUdp::Port()const{return impl_->port;}
void StateFeedbackUdp::Start(uint64_t origin,uint64_t duration,uint16_t ingress,uint16_t source){
    auto& p=*impl_;Require(!p.estimator&&ingress&&source,"invalid state route");p.estimator=std::make_unique<SenderStateEstimator>(p.session,origin,duration);p.sourcePort=source;
    p.destination.sin_family=AF_INET;p.destination.sin_addr.s_addr=htonl(INADDR_LOOPBACK);p.destination.sin_port=htons(ingress);
    std::ofstream out(p.directory/"state_model.json");out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"{\"protocol\":\"RSTA_v1\",\"bytes\":208,\"ip_bytes\":236,\"period_us\":50000,\"minimum_interval_us\":20000,\"notification_lifetime_us\":250000,\"observation_age_limit_us\":200000,\"capabilities\":7,\"origin_us\":"<<origin
       <<",\"duration_us\":"<<duration<<",\"receiver_port\":"<<p.port<<",\"relay_source_port\":"<<source<<",\"relay_ingress_port\":"<<ingress<<",\"diagnostic\":"<<JsonString(p.diagnostic)
       <<",\"endianness\":\"big\",\"units\":\"microseconds_micrometers_microradians\",\"missing_chunks\":\"latest observed frame snapshot; not global missing total\",\"sender_input\":\"received RSTA bytes plus local clock and session contract only\",\"policy_action\":\"eligibility hint only; G4 scheduler not implemented\",\"authenticated\":false}\n";
}
void StateFeedbackUdp::Send(StateReport report){
    auto& p=*impl_;Require(p.estimator&&!p.finished,"state transport not active");
    if(p.lastGenerated&&report.generatedUs-p.lastGenerated<20000){++p.suppressed;return;}
    report.sequence=++p.sequence;p.lastGenerated=report.generatedUs;auto packet=EncodeState(p.session,report);p.Transmit(packet,report);
    if(p.diagnostic=="duplicate")p.Transmit(packet,report);
    if(p.diagnostic=="invalid"&&report.sequence==20){
        auto foreign=p.session;foreign[0]^=255;p.Transmit(EncodeState(foreign,report),report);
        auto bad=packet;bad[128]^=1;p.Transmit(bad,report);p.Transmit(std::span(packet).first(80),report);
        bad=packet;bad[5]=2;p.Transmit(bad,report);
        auto future=report;future.sequence+=100000;future.generatedUs+=1000000;future.remainingTaskUs=future.taskDeadlineUs>future.generatedUs?future.taskDeadlineUs-future.generatedUs:0;future.flags&=~3u;p.Transmit(EncodeState(p.session,future),future);
    }
}
void StateFeedbackUdp::Poll(){
    auto& p=*impl_;Require(p.estimator&&!p.finished,"state transport not active");
    for(int i=0;i<256;++i){
        std::array<uint8_t,65536> buffer{};sockaddr_in peer{};int len=sizeof(peer);
        const int n=recvfrom(p.rx,reinterpret_cast<char*>(buffer.data()),int(buffer.size()),0,reinterpret_cast<sockaddr*>(&peer),&len);
        if(n==SOCKET_ERROR){Require(WSAGetLastError()==WSAEWOULDBLOCK,"state recvfrom failed");break;}
        const auto now=MonotonicUs();std::span<const uint8_t> data(buffer.data(),size_t(n));++p.received;
        const bool expected=peer.sin_family==AF_INET&&peer.sin_addr.s_addr==htonl(INADDR_LOOPBACK)&&ntohs(peer.sin_port)==p.sourcePort;
        const std::string status=expected?p.estimator->Receive(data,now):"wrong_peer";++p.statuses[status];
        p.rxLog<<now<<','<<status<<','<<p.estimator->HighestSequence()<<','<<p.estimator->AcceptedSequence()<<','<<Hex(data)<<'\n';
    }
}
SenderStateEstimate StateFeedbackUdp::Estimate(){
    auto& p=*impl_;const auto now=MonotonicUs();auto e=p.estimator->At(now);const auto& r=e.report;
    e.estimateId=++p.estimateSequence;
    p.estimates<<e.estimateId<<','<<now<<','<<r.sequence<<','<<e.receivedUs<<','<<r.generatedUs<<','<<e.ageUs<<','<<e.notificationLive<<','<<uint16_t(r.reference)<<','<<r.generation<<','
        <<r.completeFrame<<','<<r.decodedFrame<<','<<r.controlFrame<<','<<r.observationFrame<<','<<e.observationAgeUs<<','<<e.observationUsable<<','<<e.referenceReportedSynchronized<<','
        <<r.xMicro<<','<<r.yMicro<<','<<r.yawMicro<<','<<e.positionRadiusMicro<<','<<e.yawRadiusMicro<<','<<e.remainingUs<<','<<e.decision<<'\n';return e;
}
void StateFeedbackUdp::Finish(uint64_t expected){
    auto& p=*impl_;if(p.finished)return;const auto deadline=MonotonicUs()+1000000;
    do{Poll();if(p.received>=expected)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(MonotonicUs()<deadline);
    Require(p.received==expected,"state downlink delivery mismatch");
    std::ofstream out(p.directory/"state_summary.json");out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"{\"reports\":"<<p.sequence<<",\"sent\":"<<p.sent<<",\"budget_rejected\":"<<p.rejected<<",\"interval_suppressed\":"<<p.suppressed<<",\"received\":"<<p.received<<",\"estimates\":"<<p.estimateSequence<<",\"statuses\":{";
    bool first=true;for(auto [status,n]:p.statuses){if(!first)out<<',';first=false;out<<JsonString(status)<<':'<<n;}out<<"}}\n";
    p.txLog.close();p.rxLog.close();p.estimates.close();p.finished=true;
}
}
