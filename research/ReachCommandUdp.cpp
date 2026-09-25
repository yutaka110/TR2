#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include "ReachCommandUdp.h"
#include "ReachFoundation.h"
#include "ReachJson.h"
#include "ReachDatagramLink.h"
#include "ReachBudgetTransport.h"
#include <fstream>
#include <map>
#include <stdexcept>
#pragma comment(lib,"ws2_32.lib")

namespace reach {
namespace {
void Require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::string Hex(std::span<const uint8_t> bytes){std::string s;constexpr char digits[]="0123456789abcdef";for(auto b:bytes){s+=digits[b>>4];s+=digits[b&15];}return s;}
SOCKET BoundSocket(sockaddr_in& address){
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);Require(s!=INVALID_SOCKET,"command socket creation failed");
    address={};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int size=sizeof(address);u_long mode=1;int buffer=65536;
    if(bind(s,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR||getsockname(s,reinterpret_cast<sockaddr*>(&address),&size)==SOCKET_ERROR||
        ioctlsocket(s,FIONBIO,&mode)==SOCKET_ERROR||setsockopt(s,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&buffer),sizeof(buffer))==SOCKET_ERROR){closesocket(s);throw std::runtime_error("command socket setup failed");}
    return s;
}
}
struct CommandUdp::Impl {
    SOCKET tx=INVALID_SOCKET,rx=INVALID_SOCKET;
    sockaddr_in source{},destination{};
    sockaddr_in sendDestination{};
    std::unique_ptr<DatagramLink> link;
    std::shared_ptr<BudgetTransport> budget;
    uint64_t budgetRejected=0;
    bool winsock=false,finished=false;
    CommandSessionId session;
    std::filesystem::path directory;
    std::string scenario;
    std::unique_ptr<CommandGuard> guard;
    std::multimap<uint64_t,std::vector<uint8_t>> pending;
    BufferedLog txLog,rxLog;
    std::map<std::string,uint64_t> statuses;
    uint64_t offered=0,sent=0,received=0,dropped=0,ipBytes=0,sendFailures=0,origin=0;
    Impl(const std::string& id,const std::filesystem::path& dir,const std::string& profile,const LinkConfig* capacity):session(ParseCommandSession(id)),directory(dir),scenario(profile){
        Require(profile=="normal"||profile=="duplicate"||profile=="reorder"||profile=="late"||profile=="outage"||profile=="recovery"||profile=="invalid","unknown command diagnostic profile");
        try {
            WSADATA data{};Require(WSAStartup(MAKEWORD(2,2),&data)==0,"command WSAStartup failed");winsock=true;
            tx=BoundSocket(source);rx=BoundSocket(destination);
            sendDestination=destination;
            if(capacity){link=std::make_unique<DatagramLink>(*capacity,dir,"downlink",ntohs(destination.sin_port));sendDestination.sin_port=htons(link->Port());}
            txLog.open(dir/"command_tx.csv");rxLog.open(dir/"command_rx.csv");
            txLog.exceptions(std::ios::badbit|std::ios::failbit);rxLog.exceptions(std::ios::badbit|std::ios::failbit);
            txLog<<"event_us,due_us,event,bytes,ip_bytes,wire_hex\n";
            rxLog<<"received_us,bytes,sequence,status,highest_sequence,accepted_sequence,last_accepted_us,wire_hex\n";
        }catch(...){Close();throw;}
    }
    void Close(){if(tx!=INVALID_SOCKET){closesocket(tx);tx=INVALID_SOCKET;}if(rx!=INVALID_SOCKET){closesocket(rx);rx=INVALID_SOCKET;}if(winsock){WSACleanup();winsock=false;}}
    ~Impl(){Close();}
    void Queue(std::span<const uint8_t> data,uint64_t due){Require(pending.size()<64,"command diagnostic queue overflow");pending.emplace(due,std::vector<uint8_t>(data.begin(),data.end()));}
};
CommandUdp::CommandUdp(const std::string& id,const std::filesystem::path& dir,const std::string& scenario,const LinkConfig* link,std::shared_ptr<BudgetTransport> budget):impl_(std::make_unique<Impl>(id,dir,scenario,link)){impl_->budget=std::move(budget);}
uint16_t CommandUdp::IngressPort()const{return impl_->link?impl_->link->Port():0;}
void CommandUdp::SetFeedbackDestination(uint16_t port){Require(impl_->link!=nullptr,"feedback requires link");impl_->link->SetFeedbackDestination(port);}
uint64_t CommandUdp::FeedbackDelivered()const{return impl_->link?impl_->link->FeedbackDelivered():0;}
void CommandUdp::SetStateDestination(uint16_t port){Require(impl_->link!=nullptr,"state requires link");impl_->link->SetStateDestination(port);}
uint16_t CommandUdp::RelaySourcePort()const{Require(impl_->link!=nullptr,"state requires link");return impl_->link->SourcePort();}
uint64_t CommandUdp::StateDelivered()const{return impl_->link?impl_->link->StateDelivered():0;}
CommandUdp::~CommandUdp(){try{Finish();}catch(...){}}
void CommandUdp::Start(uint64_t origin){
    auto& p=*impl_;Require(!p.guard,"command UDP already started");p.origin=origin;p.guard=std::make_unique<CommandGuard>(p.session,origin);
    if(p.link)p.link->Start(origin);
    std::ofstream out(p.directory/"command_model.json");out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"{\"protocol\":\"RCMD_v1\",\"datagram_bytes\":88,\"endianness\":\"big\",\"velocity_unit\":\"micro_m_s_and_micro_rad_s\",\"lifetime_us\":100000,\"watchdog_us\":250000,\"origin_us\":"<<origin
       <<",\"scenario\":"<<JsonString(p.scenario)<<",\"sender_port\":"<<ntohs(p.source.sin_port)<<",\"receiver_port\":"<<ntohs(p.destination.sin_port)
       <<",\"address\":\"127.0.0.1\",\"diagnostics\":{\"duplicate\":\"two copies of each command\",\"reorder\":\"sequence 20 delayed 80ms\",\"late\":\"all commands delayed 150ms\",\"outage\":\"drop sequence >=41\",\"recovery\":\"drop sequence 41 through 80\",\"invalid\":\"sequence 20 plus wrong session, corrupt CRC, truncated, future high sequence\"},\"ip_header_assumption\":\"IPv4 20 + UDP 8 bytes\",\"capacity_queue_model\":"<<(p.link?"true":"false")<<",\"authenticated\":false,\"closed_loop_validated\":false}\n";
}
void CommandUdp::Offer(const MotionCommand& c){
    auto& p=*impl_;Require(p.guard&&!p.finished,"command UDP not active");const auto packet=EncodeCommand(p.session,c);++p.offered;
    const auto now=MonotonicUs();
    if((p.scenario=="outage"&&c.sequence>=41)||(p.scenario=="recovery"&&c.sequence>=41&&c.sequence<=80)){
        ++p.dropped;p.txLog<<now<<','<<now<<",diagnostic_drop,"<<packet.size()<<",0,"<<Hex(packet)<<'\n';return;
    }
    const uint64_t delay=p.scenario=="late"?150000:(p.scenario=="reorder"&&c.sequence==20?80000:0);
    p.Queue(packet,now+delay);if(p.scenario=="duplicate")p.Queue(packet,now+delay);
    if(p.scenario=="invalid"&&c.sequence==20){
        auto foreign=p.session;foreign[0]^=0xff;p.Queue(EncodeCommand(foreign,c),now);
        auto corrupted=packet;corrupted[64]^=1;p.Queue(corrupted,now);p.Queue(std::span(packet).first(50),now);
        auto future=c;future.sequence+=100000;future.generatedUs+=1000000;future.validUntilUs+=1000000;
        // A stop command allows the old source timestamp; the future time is still rejected.
        future.v=future.w=0;future.estimatedComplete=false;future.state="OBSERVE";future.reason="no_image";
        p.Queue(EncodeCommand(p.session,future),now);
    }
}
void CommandUdp::Pump(){
    auto& p=*impl_;Require(p.guard&&!p.finished,"command UDP not active");
    if(p.link)p.link->Check();
    while(!p.pending.empty()&&p.pending.begin()->first<=MonotonicUs()){
        auto item=p.pending.extract(p.pending.begin());const auto& bytes=item.mapped();const auto now=MonotonicUs();
        const int n=p.budget?p.budget->Send(false,p.tx,bytes,p.sendDestination):sendto(p.tx,reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0,reinterpret_cast<sockaddr*>(&p.sendDestination),sizeof(p.sendDestination));
        if(p.budget&&n==0){++p.budgetRejected;p.txLog<<now<<','<<item.key()<<",budget_rejected,"<<bytes.size()<<",0,"<<Hex(bytes)<<'\n';continue;}
        p.ipBytes+=bytes.size()+28;
        const bool ok=n==static_cast<int>(bytes.size());if(ok)++p.sent;else++p.sendFailures;
        p.txLog<<now<<','<<item.key()<<','<<(ok?"sent":"send_error")<<','<<bytes.size()<<','<<bytes.size()+28<<','<<Hex(bytes)<<'\n';
        Require(ok,"command sendto failed");
    }
    for(int i=0;i<256;++i){
        std::array<uint8_t,65536> bytes{};sockaddr_in peer{};int size=sizeof(peer);
        const int n=recvfrom(p.rx,reinterpret_cast<char*>(bytes.data()),static_cast<int>(bytes.size()),0,reinterpret_cast<sockaddr*>(&peer),&size);
        if(n==SOCKET_ERROR){const int error=WSAGetLastError();if(error==WSAEWOULDBLOCK)break;throw std::runtime_error("command recvfrom failed");}
        const auto now=MonotonicUs();++p.received;std::span<const uint8_t> data(bytes.data(),static_cast<size_t>(n));
        const bool expected=peer.sin_family==AF_INET&&peer.sin_addr.s_addr==p.source.sin_addr.s_addr&&peer.sin_port==(p.link?htons(p.link->SourcePort()):p.source.sin_port);
        const std::string status=expected?p.guard->Receive(data,now):"wrong_peer";++p.statuses[status];
        MotionCommand decoded;CommandSessionId session{};std::string why;DecodeCommand(data,session,decoded,why);
        p.rxLog<<now<<','<<n<<','<<decoded.sequence<<','<<status<<','<<p.guard->HighestSequence()<<','<<p.guard->AcceptedSequence()<<','<<p.guard->LastAcceptedUs()<<','<<Hex(data)<<'\n';
    }
}
CommandApplication CommandUdp::Application(uint64_t now){Require(impl_->guard!=nullptr,"command UDP not started");return impl_->guard->At(now);}
void CommandUdp::Finish(){
    auto& p=*impl_;if(p.finished)return;
    if(p.link){p.link->Finish();if(p.guard)Pump();}
    std::ofstream out(p.directory/"command_udp_summary.json");out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"{\"scenario\":"<<JsonString(p.scenario)<<",\"offered\":"<<p.offered<<",\"sent_datagrams\":"<<p.sent<<",\"received_datagrams\":"<<p.received
       <<",\"diagnostic_dropped\":"<<p.dropped<<",\"budget_rejected\":"<<p.budgetRejected<<",\"pending_on_close\":"<<p.pending.size()<<",\"send_errors\":"<<p.sendFailures<<",\"attempted_ip_bytes\":"<<p.ipBytes<<",\"statuses\":{";
    bool first=true;for(const auto& [key,count]:p.statuses){if(!first)out<<',';first=false;out<<JsonString(key)<<':'<<count;}
    out<<"},\"command_udp\":true,\"closed_loop_validated\":false}\n";p.txLog.close();p.rxLog.close();p.finished=true;
}
}
