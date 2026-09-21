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
        if(argc>1){std::ofstream out(argv[1]);out<<"{\"passed\":true,\"assertions\":"<<checks<<"}\n";}
        std::cout<<"PASS "<<checks<<" link checks\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
}
