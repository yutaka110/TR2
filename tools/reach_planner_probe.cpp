#include "../research/ReachBaselineState.h"
#include <iostream>
#include <iomanip>
int main(){
    using namespace reach;
    std::cout<<std::setprecision(17);
    // stdin: bytes,loss,burst,lambda,rate,rtt,age,queue,value
    uint64_t bytes,age,queue,rtt;uint32_t rate;double loss,burst,lambda,value;
    while(std::cin>>bytes>>loss>>burst>>lambda>>rate>>rtt>>age>>queue>>value){
        try{
            BaselineInput in{1000000+age,1000000,bytes,queue,rtt,rate,loss,burst,lambda};
            auto plans=StateBaselineCandidates(in,value);
            std::cout<<"{\"selected\":"<<ChooseBaseline(plans)<<",\"candidates\":[";
            for(size_t i=0;i<plans.size();++i){if(i)std::cout<<',';const auto& c=plans[i];
                std::cout<<"{\"group\":"<<c.group<<",\"rounds\":"<<c.rounds<<",\"ip\":"<<c.initialIp<<",\"expected_ip\":"<<c.expectedIp<<",\"success\":"<<c.success<<",\"cost\":"<<c.cost<<",\"feasible\":"<<(c.feasible?"true":"false")<<'}';}
            std::cout<<"]}\n";
        }catch(const std::exception& e){std::cerr<<e.what();return 1;}
    }
}
