#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace reach {
// Empirical JOINT 200 ms paths under a fixed continuation policy. No hidden state
// or future trace enters this interface. This is not a causal treatment effect.
constexpr size_t PredictionFeatures=16,PredictionOutcomes=6;
using PredictionVector=std::array<double,PredictionFeatures>;
inline constexpr PredictionVector PredictionScales{60,12,50,50,.12,.08,150,100,150,20,1,.4,1,.15,60,8};
struct PredictionInput {
    int kind=0,group=0,idr=0,reference=0,stage=0,task=0;
    PredictionVector x{};
    bool live=false,eligible=false,causal=true;
};
enum class PredictionMask {Common,Decode,Task,Both};
inline bool DecodeVisible(PredictionMask m){return m==PredictionMask::Decode||m==PredictionMask::Both;}
inline bool TaskVisible(PredictionMask m){return m==PredictionMask::Task||m==PredictionMask::Both;}
inline bool FeatureVisible(PredictionMask m,size_t j){return (j!=14||DecodeVisible(m))&&((j!=8&&(j<10||j>13))||TaskVisible(m));}
inline PredictionInput ProjectPrediction(PredictionInput q,PredictionMask mask){
    if(!DecodeVisible(mask))q.reference=-1;if(!TaskVisible(mask))q.stage=-1;
    for(size_t j=0;j<q.x.size();++j)if(!FeatureVisible(mask,j))q.x[j]=0;
    return q;
}
struct PredictionSample {int run=0;PredictionInput input;std::array<double,6> y{};std::array<double,3> delay{999,999,999};};
struct PredictionResult {
    std::string status="calibration_not_loaded";size_t support=0,runs=0;
    std::array<double,6> raw{-1,-1,-1,-1,-1,-1},probability{-1,-1,-1,-1,-1,-1};
    std::array<double,3> p50{-1,-1,-1},p90{-1,-1,-1};
    double distance=-1;bool terminalSupported=false;
};
class PathPredictor {
    std::vector<PredictionSample> samples_;
    // 6 outcomes x 5 fixed bins; -1 means no independent calibration support.
    std::array<double,30> calibrated_{};
    bool calibratedLoaded_=false;
public:
    PathPredictor(){calibrated_.fill(-1);}
    const auto& Samples()const{return samples_;}
    void Add(PredictionSample s){
        if(samples_.size()>=16000||s.run<1||s.input.kind<0||s.input.kind>4||s.input.task<1||s.input.task>2)throw std::runtime_error("invalid prediction model");
        for(auto v:s.input.x)if(!std::isfinite(v))throw std::runtime_error("nonfinite prediction feature");
        for(auto v:s.y)if(v!=0&&v!=1)throw std::runtime_error("invalid path label");
        if(s.y[1]>s.y[0]||s.y[3]>s.y[1])throw std::runtime_error("incoherent path labels");
        if(s.input.kind==0&&(s.y[0]||s.y[1]||s.y[3]))throw std::runtime_error("DEFER cannot create a target AU");
        for(auto v:s.delay)if(!std::isfinite(v)||v<0||v>999)throw std::runtime_error("invalid path delay");
        samples_.push_back(s);
    }
    void Load(const std::string& file){
        std::ifstream in(file);if(!in)throw std::runtime_error("prediction model unavailable");
        std::string line;std::getline(in,line);if(line!="reach_paths_v2")throw std::runtime_error("prediction schema mismatch");
        while(std::getline(in,line)){
            if(line.empty())continue;std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);PredictionSample s;
            row>>s.run>>s.input.kind>>s.input.group>>s.input.idr>>s.input.reference>>s.input.stage>>s.input.task;
            for(auto& v:s.input.x)row>>v;for(auto& v:s.y)row>>v;for(auto& v:s.delay)row>>v;
            if(!row)throw std::runtime_error("truncated prediction row");std::string extra;if(row>>extra)throw std::runtime_error("extra prediction column");Add(s);
        }
        std::ifstream cal(file+".cal");if(cal){for(auto& v:calibrated_){if(!(cal>>v)||!std::isfinite(v)||v< -1||v>1)throw std::runtime_error("invalid calibration");}std::string extra;if(cal>>extra)throw std::runtime_error("extra calibration value");calibratedLoaded_=true;}
    }
    PredictionResult Predict(const PredictionInput& q,const std::function<bool()>& stop={},PredictionMask mask=PredictionMask::Both)const{
        PredictionResult out;
        auto expired=[&](){return stop&&stop();};
        auto cancelled=[](){PredictionResult p;p.status="compute_budget";return p;};
        if(expired())return cancelled();
        if(!q.eligible){out.status="ineligible";return out;}
        if(!q.causal){out.status="future_input";return out;}
        if(!q.live){out.status="notification_unavailable";return out;}
        for(auto v:q.x)if(!std::isfinite(v)){out.status="invalid_input";return out;}
        if(samples_.empty())return out;
        std::vector<std::pair<double,size_t>> neighbors;
        for(size_t i=0;i<samples_.size();++i){const auto& s=samples_[i];const auto& p=s.input;
            if((i%64)==0&&expired())return cancelled();
            if(p.kind!=q.kind||p.group!=q.group||p.idr!=q.idr||p.task!=q.task||
                (DecodeVisible(mask)&&p.reference!=q.reference)||(TaskVisible(mask)&&p.stage!=q.stage))continue;
            double d=0,maxd=0;for(size_t j=0;j<q.x.size();++j){if(!FeatureVisible(mask,j))continue;const double z=std::abs(q.x[j]-p.x[j])/PredictionScales[j];d+=z*z;maxd=std::max(maxd,z);}
            // Explicit local support, never silently extrapolate through large gaps.
            if(maxd<=2&&d<=9)neighbors.push_back({d,i});
        }
        if(expired())return cancelled();
        const auto count=std::min<size_t>(64,neighbors.size());
        std::partial_sort(neighbors.begin(),neighbors.begin()+count,neighbors.end());neighbors.resize(count);
        if(expired())return cancelled();
        std::set<int> runs;for(auto [d,i]:neighbors)runs.insert(samples_[i].run);
        out.support=neighbors.size();out.runs=runs.size();out.distance=neighbors.empty()?-1:std::sqrt(neighbors.front().first);
        if(out.support<8||out.runs<2){out.status="out_of_domain";return out;}
        out.status=calibratedLoaded_?"calibrated":"uncalibrated";out.raw.fill(0);
        for(auto [d,i]:neighbors)for(size_t j=0;j<6;++j)out.raw[j]+=samples_[i].y[j]/out.support;
        out.terminalSupported=out.raw[5]>0&&out.raw[5]<1;
        for(size_t j=0;j<6;++j){const size_t bin=std::min<size_t>(4,static_cast<size_t>(out.raw[j]*5));out.probability[j]=calibrated_[j*5+bin];}
        // Preserve event nesting after marginal histogram calibration. Missing
        // calibration stays unknown; structural zeros do not require fitting.
        if(q.kind==0)for(auto j:{0,1,3})out.probability[j]=0;
        if(out.probability[0]>=0&&out.probability[1]>=0)out.probability[1]=std::min(out.probability[1],out.probability[0]);
        if(out.probability[1]>=0&&out.probability[3]>=0)out.probability[3]=std::min(out.probability[3],out.probability[1]);
        if(calibratedLoaded_&&std::any_of(out.probability.begin(),out.probability.end(),[](double p){return p<0;}))out.status="calibration_partial";
        for(size_t j=0;j<3;++j){std::vector<double> delays;for(auto [d,i]:neighbors)delays.push_back(samples_[i].delay[j]);std::sort(delays.begin(),delays.end());
            out.p50[j]=delays[static_cast<size_t>(std::ceil(.5*delays.size()))-1];out.p90[j]=delays[static_cast<size_t>(std::ceil(.9*delays.size()))-1];}
        if(expired())return cancelled();return out;
    }
};
}
