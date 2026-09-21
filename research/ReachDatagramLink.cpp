#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include "ReachDatagramLink.h"
#include "ReachFoundation.h"
#include "ReachJson.h"
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#pragma comment(lib,"ws2_32.lib")
namespace reach {
namespace {
void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string Hex(const std::vector<uint8_t>& bytes){std::string s;s.reserve(bytes.size()*2);for(auto b:bytes){s+="0123456789abcdef"[b>>4];s+="0123456789abcdef"[b&15];}return s;}
}
struct DatagramLink::Impl {
    SOCKET socket=INVALID_SOCKET,outputSocket=INVALID_SOCKET;sockaddr_in destination{};uint16_t port=0,sourcePort=0;
    bool winsock=false,finished=false;std::atomic<bool> stop=false;
    std::thread worker;mutable std::mutex mutex;std::string error;
    LinkConfig config;LinkModel model;std::filesystem::path directory;std::string direction;
    BufferedLog log;uint64_t origin=0,offered=0,accepted=0,dropped=0,delivered=0,cancelled=0,offeredBytes=0,deliveredBytes=0,dropBytes=0,cancelBytes=0,maxLag=0;
    Impl(const LinkConfig& c,const std::filesystem::path& dir,const std::string& name,uint16_t target):config(c),model(c),directory(dir),direction(name){
        Require(name=="uplink"||name=="downlink","invalid link direction");
        try{
            WSADATA w{};Require(WSAStartup(MAKEWORD(2,2),&w)==0,"link WSAStartup failed");winsock=true;
            socket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);Require(socket!=INVALID_SOCKET,"link socket failed");
            sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            Require(bind(socket,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0,"link bind failed");
            int length=sizeof(local),buffer=4194304;u_long nonblocking=1;
            Require(getsockname(socket,reinterpret_cast<sockaddr*>(&local),&length)==0&&ioctlsocket(socket,FIONBIO,&nonblocking)==0&&
                setsockopt(socket,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&buffer),sizeof(buffer))==0,"link socket setup failed");
            port=ntohs(local.sin_port);destination=local;destination.sin_port=htons(target);
            // Separate egress from ingress: receiver feedback must never loop back
            // through this direction. RNVP feedback is unused here, as in G1.
            outputSocket=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);Require(outputSocket!=INVALID_SOCKET,"link output socket failed");
            local.sin_port=0;Require(bind(outputSocket,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0&&
                getsockname(outputSocket,reinterpret_cast<sockaddr*>(&local),&length)==0&&ioctlsocket(outputSocket,FIONBIO,&nonblocking)==0,"link output bind failed");
            sourcePort=ntohs(local.sin_port);
            log.open(dir/(name+"_link.csv"));log.exceptions(std::ios::badbit|std::ios::failbit);
            log<<"packet_id,event,event_us,arrival_us,start_us,completion_us,actual_send_us,payload_bytes,ip_bytes,occupied_ip_bytes,remaining_work,wire_hex\n";
        }catch(...){Close();throw;}
    }
    ~Impl(){stop=true;if(worker.joinable())worker.join();Close();}
    void Close(){if(socket!=INVALID_SOCKET){closesocket(socket);socket=INVALID_SOCKET;}if(outputSocket!=INVALID_SOCKET){closesocket(outputSocket);outputSocket=INVALID_SOCKET;}if(winsock){WSACleanup();winsock=false;}}
    void Record(const LinkPacket& p,const char* event,uint64_t at,uint64_t actual=0,bool bytes=false){
        log<<p.id<<','<<event<<','<<at<<','<<p.enqueuedUs<<','<<p.startedUs<<','<<p.completedUs<<','<<actual<<','<<p.payload.size()<<','<<p.ipBytes<<','<<model.OccupiedBytes()<<','<<p.remaining<<','<<(bytes?Hex(p.payload):"")<<'\n';
    }
    void Deliver(uint64_t now){
        for(auto& p:model.Advance(now)){
            const auto actual=MonotonicUs()-origin;Require(actual>=p.completedUs,"link delivered before serialization");
            const int n=sendto(outputSocket,reinterpret_cast<const char*>(p.payload.data()),static_cast<int>(p.payload.size()),0,reinterpret_cast<sockaddr*>(&destination),sizeof(destination));
            Require(n==static_cast<int>(p.payload.size()),"link sendto failed");
            ++delivered;deliveredBytes+=p.ipBytes;maxLag=std::max(maxLag,actual-p.completedUs);Record(p,"delivered",p.completedUs,actual);
        }
    }
    void Run() noexcept {
        try{
            uint64_t closeAt=0;
            for(;;){
                if(stop&&!closeAt)closeAt=MonotonicUs();
                Deliver(MonotonicUs()-origin);
                for(int i=0;i<256;++i){
                    std::array<uint8_t,65536> data{};sockaddr_in peer{};int size=sizeof(peer);
                    const int n=recvfrom(socket,reinterpret_cast<char*>(data.data()),static_cast<int>(data.size()),0,reinterpret_cast<sockaddr*>(&peer),&size);
                    if(n==SOCKET_ERROR){Require(WSAGetLastError()==WSAEWOULDBLOCK,"link recvfrom failed");break;}
                    Require(peer.sin_addr.s_addr==htonl(INADDR_LOOPBACK),"link non-loopback peer");
                    const auto now=MonotonicUs()-origin;Deliver(now);
                    LinkPacket p;p.id=++offered;p.enqueuedUs=now;p.ipBytes=n+28;p.payload.assign(data.begin(),data.begin()+n);offeredBytes+=p.ipBytes;
                    const bool ok=model.Offer(p.id,now,p.payload);
                    if(ok)++accepted;else{++dropped;dropBytes+=p.ipBytes;}
                    Record(p,ok?"admitted":"tail_drop",now,0,true);
                }
                if(closeAt&&((model.Pending()==0&&MonotonicUs()-closeAt>=20000)||MonotonicUs()-closeAt>=250000))break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // Bounded shutdown drain uses real elapsed time; residual work is explicit.
            for(auto& p:model.Cancel()){++cancelled;cancelBytes+=p.ipBytes;Record(p,"cancelled_at_close",MonotonicUs()-origin);}
        }catch(const std::exception& e){std::lock_guard lock(mutex);error=e.what();}
    }
};
DatagramLink::DatagramLink(const LinkConfig& c,const std::filesystem::path& dir,const std::string& name,uint16_t target):impl_(std::make_unique<Impl>(c,dir,name,target)){}
DatagramLink::~DatagramLink(){try{Finish();}catch(...){}}
uint16_t DatagramLink::Port() const{return impl_->port;}
uint16_t DatagramLink::SourcePort() const{return impl_->sourcePort;}
void DatagramLink::Start(uint64_t origin){auto& p=*impl_;Require(!p.worker.joinable()&&!p.origin,"link already started");p.origin=origin;p.worker=std::thread([&p]{p.Run();});}
void DatagramLink::Check() const {std::lock_guard lock(impl_->mutex);if(!impl_->error.empty())throw std::runtime_error(impl_->error);}
void DatagramLink::Finish(){
    auto& p=*impl_;if(p.finished)return;p.stop=true;if(p.worker.joinable())p.worker.join();p.log.close();
    std::ofstream out(p.directory/(p.direction+"_link_summary.json"));out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"{\"direction\":"<<JsonString(p.direction)<<",\"origin_us\":"<<p.origin<<",\"relay_port\":"<<p.port<<",\"source_port\":"<<p.sourcePort<<",\"destination_port\":"<<ntohs(p.destination.sin_port)
       <<",\"queue_limit_ip_bytes\":"<<p.config.queueBytes<<",\"maximum_occupied_ip_bytes\":"<<p.model.MaximumBytes()
       <<",\"offered_packets\":"<<p.offered<<",\"admitted_packets\":"<<p.accepted<<",\"tail_dropped_packets\":"<<p.dropped<<",\"delivered_packets\":"<<p.delivered<<",\"cancelled_packets\":"<<p.cancelled
       <<",\"offered_ip_bytes\":"<<p.offeredBytes<<",\"delivered_ip_bytes\":"<<p.deliveredBytes<<",\"tail_dropped_ip_bytes\":"<<p.dropBytes<<",\"cancelled_ip_bytes\":"<<p.cancelBytes
       <<",\"maximum_dispatch_lag_us\":"<<p.maxLag<<",\"error\":"<<JsonString(p.error)<<",\"capacity_trace\":[";
    bool first=true;for(auto point:p.config.capacity){if(!first)out<<',';first=false;out<<"{\"at_us\":"<<point.atUs<<",\"bps\":"<<point.bps<<'}';}
    out<<"]}\n";out.close();p.finished=true;Check();
}
}
