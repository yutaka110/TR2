#include "../research/ReachPrediction.h"
#include <iostream>
#include <iomanip>
using namespace reach;
int main(int argc,char** argv){try{
    if(argc==3){PathPredictor p,q;p.Load(argv[1]);q.Load(argv[2]);std::cout<<std::setprecision(17);
        size_t i=0;for(auto s:q.Samples()){s.input.live=s.input.eligible=true;const auto r=p.Predict(s.input);
            std::cout<<i++<<','<<r.status<<','<<r.support<<','<<r.runs<<','<<r.distance<<','<<r.terminalSupported;
            for(size_t j=0;j<6;++j)std::cout<<','<<r.raw[j]<<','<<r.probability[j];
            for(size_t j=0;j<3;++j)std::cout<<','<<r.p50[j]<<','<<r.p90[j];std::cout<<'\n';}return 0;}
    int checks=0;auto check=[&](bool b){++checks;if(!b)throw std::runtime_error("prediction test "+std::to_string(checks));};
    PredictionInput q;q.eligible=q.live=true;q.task=1;q.kind=1;PathPredictor p;
    check(p.Predict(q).status=="calibration_not_loaded");
    for(int i=0;i<16;++i){PredictionSample s;s.run=1+i%2;s.input=q;s.y.fill(i%2);s.delay.fill(i%2?50:999);p.Add(s);}
    auto r=p.Predict(q);check(r.status=="uncalibrated"&&r.support==16&&r.runs==2);
    for(int j=0;j<6;++j)check(r.raw[j]==.5&&r.probability[j]==-1);
    for(int j=0;j<3;++j)check(r.p50[j]==50&&r.p90[j]==999);
    q.live=false;check(p.Predict(q).status=="notification_unavailable");q.live=true;q.causal=false;check(p.Predict(q).status=="future_input");q.causal=true;
    q.eligible=false;check(p.Predict(q).status=="ineligible");q.eligible=true;
    for(int j=0;j<16;++j){q.x[j]=PredictionScales[j]*2.01;check(p.Predict(q).status=="out_of_domain");q.x[j]=0;}
    q.reference=4;check(p.Predict(q).status=="out_of_domain");q.reference=0;q.stage=2;check(p.Predict(q).status=="out_of_domain");q.stage=0;
    PathPredictor one;for(int i=0;i<70;++i){PredictionSample s;s.run=1;s.input=q;one.Add(s);}check(one.Predict(q).status=="out_of_domain"&&one.Predict(q).support==64);
    PathPredictor capped;for(int i=0;i<90;++i){PredictionSample s;s.run=1+i%2;s.input=q;capped.Add(s);}check(capped.Predict(q).support==64);
    PredictionSample bad;bad.run=1;bad.input=q;bad.y[0]=.1;bool rejected=false;try{p.Add(bad);}catch(const std::runtime_error&){rejected=true;}check(rejected);
    bad.y[0]=0;bad.y[1]=1;rejected=false;try{p.Add(bad);}catch(const std::runtime_error&){rejected=true;}check(rejected);
    bad.y.fill(1);bad.input.kind=0;rejected=false;try{p.Add(bad);}catch(const std::runtime_error&){rejected=true;}check(rejected);
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
