#include "../research/ReachScheduler.h"
#include <chrono>
#include <iomanip>
#include <iostream>
using namespace reach;
SchedulerOptions Options(int mode,double lambda){SchedulerOptions o;o.lambda=lambda;o.scalar=mode==4;o.mask=mode==0?PredictionMask::Common:mode==1?PredictionMask::Decode:mode==2?PredictionMask::Task:PredictionMask::Both;return o;}
int main(int argc,char** argv){try{std::cout<<std::setprecision(17);
    if(argc==2&&std::string(argv[1])=="rank"){
        size_t n;double lambda,scale;
        while(std::cin>>n>>lambda>>scale){size_t best=0;double bestScore=0;std::vector<double> scores;
            for(size_t i=0;i<n;++i){double p,ip,value;std::cin>>p>>ip>>value;const auto score=ReachShortScore(p,ip,lambda,scale,value);scores.push_back(score);if(score>bestScore){best=i;bestScore=score;}}
            if(!std::cin)throw std::runtime_error("truncated rank input");std::cout<<best;for(auto s:scores)std::cout<<','<<s;std::cout<<'\n';}return 0;
    }
    if(argc==2){PathPredictor model;model.Load(argv[1]);uint64_t label;int mode;double lambda;size_t count;
        while(std::cin>>label>>mode>>lambda>>count){if(mode<0||mode>4||count>64||count<1)throw std::runtime_error("invalid replay header");std::vector<SchedulerCandidate> cs(count);
            for(auto& c:cs){int kind;std::cin>>c.local>>kind>>c.action.group>>c.action.ip>>c.action.eligible>>c.snapshot.frame.id>>c.snapshot.frame.capture;
                c.action.kind=static_cast<ActionKind>(kind);auto& q=c.input;q.kind=kind;q.group=c.action.group;q.eligible=c.action.eligible;
                std::cin>>q.idr>>q.reference>>q.stage>>q.task>>q.live>>q.causal;for(auto& v:q.x)std::cin>>v;
            }
            if(!std::cin)throw std::runtime_error("truncated replay input");const auto start=std::chrono::steady_clock::now();const auto result=SelectReachAction(cs,model,[](){return false;},Options(mode,lambda));
            const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
            std::cout<<"{\"id\":"<<label<<",\"mode\":"<<mode<<",\"lambda\":"<<lambda<<",\"selected\":"<<result.index<<",\"fallback\":"<<result.fallback<<",\"reason\":\""<<result.reason<<"\",\"elapsed_us\":"<<us<<",\"candidates\":[";
            for(size_t i=0;i<cs.size();++i){const auto& c=cs[i];if(i)std::cout<<',';std::cout<<"{\"raw\":"<<c.prediction.raw[3]<<",\"value\":"<<c.value<<",\"score\":";
                if(std::isfinite(c.score))std::cout<<c.score;else std::cout<<"null";std::cout<<",\"support\":"<<c.prediction.support<<",\"runs\":"<<c.prediction.runs<<"}";}
            std::cout<<"]}\n";
        }return 0;
    }
    int checks=0;auto check=[&](bool p){++checks;if(!p)throw std::runtime_error("comparison test "+std::to_string(checks));};
    PathPredictor model;PredictionInput q;q.kind=1;q.task=1;q.eligible=q.live=true;q.reference=1;q.stage=1;
    for(int r=0;r<2;++r)for(int s=0;s<2;++s)for(int j=0;j<16;++j){PredictionSample row;row.run=1+j%2;row.input=q;row.input.reference=r;row.input.stage=s;row.y.fill(r&&s);model.Add(row);}
    for(int mode=0;mode<4;++mode){const auto m=Options(mode,.1).mask;auto projected=ProjectPrediction(q,m);const auto before=model.Predict(projected,{},m);
        check(before.support>=8);
        for(int k=0;k<18;++k){auto changed=q;
            if(k==16){if(DecodeVisible(m))continue;changed.reference=997;}else if(k==17){if(TaskVisible(m))continue;changed.stage=999;}
            else{if(FeatureVisible(m,k))continue;changed.x[k]=123456;}
            const auto after=model.Predict(ProjectPrediction(changed,m),{},m);check(after.raw==before.raw&&after.support==before.support&&after.runs==before.runs);
        }
    }
    check(model.Predict(ProjectPrediction(q,PredictionMask::Common),{},PredictionMask::Common).raw[3]==.25);
    check(model.Predict(q).raw[3]==1);
    for(int p=0;p<=10;++p)for(int cost=0;cost<=8;++cost){const double value=.5+p*.3;const auto a=ReachShortScore(p/10.,cost*1272.,.1,65536.,value);const auto b=value*ReachShortScore(p/10.,cost*1272.,.1/value);check(std::abs(a-b)<1e-12);}
    for(auto m:{PredictionMask::Common,PredictionMask::Decode,PredictionMask::Task,PredictionMask::Both}){auto a=ProjectPrediction(q,m);check(a.kind==q.kind&&a.group==q.group&&a.task==q.task&&a.live==q.live&&a.causal==q.causal&&a.eligible==q.eligible);}
    bool rejected=false;try{ReachShortScore(-1,1,.1);}catch(...){rejected=true;}check(rejected);
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"scope\":\"masked feature noninterference, marginalized training labels, scalar effective lambda equivalence, shared native rank\"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
