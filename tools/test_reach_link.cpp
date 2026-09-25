#include "../research/ReachLinkModel.h"
#include "../research/ReachFoundation.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv){
    int checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
    auto packet=[](size_t ipBytes){return std::vector<uint8_t>(ipBytes-28,7);};
    try{
        using namespace reach;
        LinkModel constant({16384,{{0,800000}}});constant.Advance(0);check(constant.Offer(1,0,packet(1000)),"constant admission");
        check(constant.Advance(9999).empty(),"no early service");auto done=constant.Advance(10000);
        check(done.size()==1&&done[0].completedUs==10000,"1000 bytes at 800 kbps takes 10 ms");
        constant.Advance(1000000);constant.Offer(2,1000000,packet(1000));check(constant.Advance(1009999).empty(),"idle time not banked");
        check(constant.Advance(1010000).size()==1,"idle completion");
        LinkModel changed({16384,{{0,800000},{5000,400000}}});changed.Advance(0);changed.Offer(1,0,packet(1000));
        check(changed.Advance(14999).empty(),"mid-packet capacity drop not ignored");
        check(changed.Advance(15000)[0].completedUs==15000,"500 bytes at old rate, 500 at new");
        LinkModel paused({16384,{{0,800000},{5000,0},{20000,800000}}});paused.Advance(0);paused.Offer(1,0,packet(1000));
        check(paused.Advance(24999).empty(),"zero capacity freezes service");check(paused.Advance(25000).size()==1,"resume remaining work");
        LinkModel faster({16384,{{0,400000},{10000,800000}}});faster.Advance(0);faster.Offer(1,0,packet(1000));
        check(faster.Advance(15000)[0].completedUs==15000,"mid-packet capacity rise");
        LinkModel finite({2000,{{0,800000}}});finite.Advance(0);
        check(finite.Offer(1,0,packet(1000))&&finite.Offer(2,0,packet(1000)),"exact queue boundary");
        check(!finite.Offer(3,0,packet(28)),"tail-drop counts packet in service");
        finite.Advance(10000);check(finite.Offer(4,10000,packet(1000)),"completion before same-time arrival");
        done=finite.Advance(30000);check(done.size()==2&&done[0].id==2&&done[1].id==4&&done[1].completedUs==30000,"FIFO without eviction");
        check(finite.MaximumBytes()==2000&&finite.OccupiedBytes()==0,"occupancy accounting");
        LinkModel up({1000,{{0,0}}}),down({1000,{{0,800000}}});up.Advance(0);down.Advance(0);up.Offer(1,0,packet(1000));down.Offer(1,0,packet(1000));
        check(up.Advance(10000).empty()&&down.Advance(10000).size()==1,"independent directions");
        check(up.Cancel().size()==1&&up.Pending()==0,"bounded cancellation");
        LinkModel fractional({16384,{{0,3000000}}});fractional.Advance(0);fractional.Offer(1,0,packet(116));
        check(fractional.Advance(309).empty()&&fractional.Advance(310).size()==1,"round up microseconds");
        int invalid=0;
        for(LinkConfig c:std::vector<LinkConfig>{{0,{{0,1}}},{100,{}},{100,{{1,1}}},{100,{{0,1},{0,2}}},{100,{{0,1000000001}}}}){try{LinkModel bad(c);}catch(const std::exception&){++invalid;}}
        check(invalid==5,"invalid configurations rejected");
        try{fractional.Advance(0);check(false,"clock reversal accepted");}catch(const std::runtime_error&){check(true,"clock reversal rejected");}
        LinkModel a({16384,{{0,700003},{5000,170001},{20000,0},{30000,600007}}}),b({16384,{{0,700003},{5000,170001},{20000,0},{30000,600007}}});
        a.Advance(0);b.Advance(0);for(int i=0;i<8;++i){a.Offer(i,0,packet(1000));b.Offer(i,0,packet(1000));}
        auto bulk=a.Advance(200000);std::vector<LinkPacket> split;
        for(uint64_t t=1;t<=200000;t+=17){auto out=b.Advance(t);for(auto& p:out)split.push_back(std::move(p));}
        auto last=b.Advance(200000);for(auto& p:last)split.push_back(std::move(p));
        check(bulk.size()==8&&split.size()==8,"all segmentation probes complete");
        for(size_t i=0;i<8;++i)check(bulk[i].completedUs==split[i].completedUs,"service independent of polling granularity");
        LinkConfig trace{16384,{{0,800000}},{{0,false,10000},{10000,true,0},{20000,false,2000},{30000,false,0}}};
        trace.Validate();
        check(trace.StateIndex(9999)==0&&trace.StateIndex(10000)==1&&trace.StateIndex(19999)==1&&trace.StateIndex(20000)==2,"left-closed time intervals");
        check(trace.StateIndex(3600000000ULL)==3,"final state held");
        PropagationModel channel(trace);
        auto serialized=[&](uint64_t id,uint64_t end){LinkPacket p;p.id=id;p.completedUs=end;p.ipBytes=1000;p.payload=packet(1000);return p;};
        auto lostPacket=serialized(1,10000);check(!channel.Schedule(lostPacket)&&channel.Pending()==0,"blackout sampled at completion boundary");
        auto p1=serialized(2,9999),p2=serialized(3,30000),p3=serialized(4,20000);
        check(channel.Schedule(p1)&&channel.Schedule(p2)&&channel.Schedule(p3),"out-of-order scheduling accepted");
        check(channel.Advance(19998).empty(),"propagation never early");
        done=channel.Advance(30000);check(done.size()==3&&done[0].id==2&&done[1].id==4&&done[2].id==3,"delivery priority by due time");
        LinkConfig reorderTrace{16384,{{0,800000}},{{0,false,100000},{10000,false,0}}};
        PropagationModel reorder(reorderTrace);p1=serialized(1,9999);p2=serialized(2,10000);
        reorder.Schedule(p1);reorder.Schedule(p2);
        done=reorder.Advance(10000);check(done.size()==1&&done[0].id==2,"shorter delay overtakes earlier packet");
        check(reorder.Cancel().size()==1&&reorder.Pending()==0,"propagation cancellation explicit");
        PropagationModel tie(trace);p1=serialized(9,30000);p2=serialized(8,30000);tie.Schedule(p1);tie.Schedule(p2);
        done=tie.Advance(30000);check(done[0].id==8&&done[1].id==9,"equal due time stable packet-id order");
        // Sparse/dense schedules query identical exogenous states at common times.
        // Extra packet queries and reverse query order cannot consume randomness.
        PropagationModel sparse(trace),dense(trace);size_t sparseN=0,denseN=0;
        for(uint64_t t=0;t<40000;t+=100){auto p=serialized(t+1,t);dense.Schedule(p);++denseN;
            if(t%1000==0){auto q=serialized(t+1,t);sparse.Schedule(q);++sparseN;
                check(p.traceIndex==q.traceIndex&&p.delayUs==q.delayUs&&p.dueUs==q.dueUs,"same time state despite tenfold packet count");}}
        check(sparseN==40&&denseN==400,"fairness workload counts");
        for(uint64_t t=40000;t>0;t-=1000)check(trace.StateIndex(t)==(t>=30000?3:t>=20000?2:t>=10000?1:0),"stateless reverse query");
        PropagationModel bounded(trace);p1=serialized(1,0);p1.ipBytes=16777217;
        bool rejected=false;try{bounded.Schedule(p1);}catch(const std::runtime_error&){rejected=true;}
        check(rejected&&bounded.Pending()==0,"resource overrun invalidates without silently dropping");
        for(int kind=0;kind<4;++kind){auto bad=trace;if(kind==0)bad.impairment.clear();if(kind==1)bad.impairment[0].atUs=1;if(kind==2)bad.impairment[1].atUs=0;if(kind==3)bad.impairment[0].delayUs=1000001;
            rejected=false;try{bad.Validate();}catch(const std::runtime_error&){rejected=true;}check(rejected,"invalid impairment rejected");}
        IpBudgetConfig budgets;budgets.total={0,1000,1000};budgets.up={0,1000,1000};budgets.down={0,1000,1000};
        IpBudget common(budgets);
        check(common.Admit(0,true,600)=="allowed"&&common.Admit(0,false,400)=="allowed","directions debit same total");
        check(common.Admit(1000000,true,28)=="total_byte_limit"&&common.Used(0)==1000,"no refund for loss or idle time");
        budgets.total={928,116,10000};budgets.up=budgets.down=budgets.total;IpBudget rate(budgets);
        check(rate.Admit(0,true,116)=="allowed","initial burst");
        check(rate.Admit(999999,false,116)=="total_rate_limit","one microsecond before refill");
        check(rate.Admit(1000000,false,116)=="allowed","exact byte refill boundary");
        check(rate.Admit(100000000,true,116)=="allowed"&&rate.Admit(100000000,false,116)=="total_rate_limit","idle credit bounded by burst");
        budgets.total={0,116,1000};budgets.up={0,28,1000};budgets.down={0,116,1000};IpBudget atomic(budgets);
        check(atomic.Admit(0,true,116)=="direction_rate_limit"&&atomic.Used(0)==0,"direction rejection cannot consume shared budget");
        check(atomic.Admit(0,false,116)=="allowed"&&atomic.Used(2)==116,"other direction retains credit after rejection");
        budgets.total={0,1000,1000};budgets.up={0,1000,100};budgets.down={0,1000,1000};IpBudget cap(budgets);
        check(cap.Admit(0,true,116)=="direction_byte_limit"&&cap.Admit(0,false,116)=="allowed","direction cumulative limit independent");
        budgets.total={0,200,200};budgets.up=budgets.down=budgets.total;IpBudget boundary(budgets);
        check(boundary.Admit(0,true,116)=="allowed"&&boundary.Admit(0,false,84)=="allowed"&&boundary.Used(0)==200,"exact cumulative byte boundary");
        net::RnvpHeaderV1 header{};header.magic=net::kRnvpMagic;header.version=net::kRnvpVersion;header.headerSize=net::kRnvpHeaderV1Size;header.chunkCount=1;header.payloadSize=0;std::vector<uint8_t> wire(net::kRnvpHeaderV1Size);
        header.packetType=static_cast<uint8_t>(net::PacketType::Data);net::EncodeRnvpHeaderV1(wire.data(),header);
        check(IpPacketClass(wire)=="video_delta","delta class");header.flags=net::PacketFlag_KeyFrame;net::EncodeRnvpHeaderV1(wire.data(),header);
        check(IpPacketClass(wire)=="video_idr","IDR belongs to budget");header.flags|=net::PacketFlag_Retransmit;net::EncodeRnvpHeaderV1(wire.data(),header);
        check(IpPacketClass(wire)=="retransmission","IDR retransmission counted once");header.flags=0;header.packetType=static_cast<uint8_t>(net::PacketType::Fec);net::EncodeRnvpHeaderV1(wire.data(),header);
        check(IpPacketClass(wire)=="fec","FEC class");
        for(auto type:{net::PacketType::Control,net::PacketType::TransportFeedback,net::PacketType::Ping,net::PacketType::Pong}){header.packetType=static_cast<uint8_t>(type);net::EncodeRnvpHeaderV1(wire.data(),header);check(IpPacketClass(wire)!="unknown","feedback class");}
        for(bool missing:{false,true}){net::AckPayload ack{};ack.missingChunkCount=missing?1:0;if(missing)ack.missingChunkIndices={0};header.packetType=static_cast<uint8_t>(net::PacketType::Ack);header.payloadSize=static_cast<uint32_t>(net::CalculateAckPayloadSize(ack));wire.resize(net::kRnvpHeaderV1Size+header.payloadSize);net::EncodeRnvpHeaderV1(wire.data(),header);net::EncodeAckPayload(wire.data()+net::kRnvpHeaderV1Size,ack);check(IpPacketClass(wire)==(missing?"nack":"ack"),"ACK/NACK distinction");}
        check(IpPacketClass(std::vector<uint8_t>(20,0))=="unknown","unknown protocol explicit");
        if(argc>1){std::ofstream out(argv[1]);out<<"{\"passed\":true,\"assertions\":"<<checks<<"}\n";}
        std::cout<<"PASS "<<checks<<" link checks\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
}
