#include "ReachVisualControl.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace reach {
namespace {
constexpr double Pi=3.14159265358979323846;
const double F=640/(2*std::tan(70*Pi/360));
double Wrap(double a){return std::remainder(a,2*Pi);}
bool Solve(double a[8][9],int n,double* result) {
    for(int i=0;i<n;++i){
        int pivot=i;for(int j=i+1;j<n;++j)if(std::abs(a[j][i])>std::abs(a[pivot][i]))pivot=j;
        if(std::abs(a[pivot][i])<1e-12)return false;
        for(int k=i;k<=n;++k)std::swap(a[i][k],a[pivot][k]);
        const double divisor=a[i][i];for(int k=i;k<=n;++k)a[i][k]/=divisor;
        for(int j=0;j<n;++j)if(j!=i){const double factor=a[j][i];for(int k=i;k<=n;++k)a[j][k]-=factor*a[i][k];}
    }
    for(int i=0;i<n;++i)result[i]=a[i][n];return true;
}
struct Line {
    double a=0,b=0;
    bool Fit(const std::vector<ImagePoint>& p){
        if(p.size()<8)return false;
        double x=0,y=0,xx=0,xy=0;for(auto q:p){x+=q.u;y+=q.v;xx+=q.u*q.u;xy+=q.u*q.v;}
        const double n=static_cast<double>(p.size()),d=n*xx-x*x;if(d<1e-6)return false;
        a=(n*xy-x*y)/d;b=(y-a*x)/n;return true;
    }
};
ImagePoint Intersect(const Line& horizontal,const Line& vertical){
    const double v=(horizontal.a*vertical.b+horizontal.b)/(1-horizontal.a*vertical.a);
    return {vertical.a*v+vertical.b,v};
}
bool Homography(const std::array<ImagePoint,4>& c,double* h){
    double a[8][9]{};
    for(int i=0;i<4;++i){
        const double x=(i==0||i==3)?-.12:.12,y=i<2?-.12:.12,u=c[i].u,v=c[i].v;
        double* r=a[2*i];r[0]=x;r[1]=y;r[2]=1;r[6]=-u*x;r[7]=-u*y;r[8]=u;
        r=a[2*i+1];r[3]=x;r[4]=y;r[5]=1;r[6]=-v*x;r[7]=-v*y;r[8]=v;
    }
    return Solve(a,8,h);
}
ImagePoint Map(const double* h,double x,double y){const double z=h[6]*x+h[7]*y+1;return {(h[0]*x+h[1]*y+h[2])/z,(h[3]*x+h[4]*y+h[5])/z};}
std::array<ImagePoint,4> Project(const double* p){
    std::array<ImagePoint,4> c;
    for(int i=0;i<4;++i){const double x=(i==0||i==3)?-.12:.12,y=i<2?-.12:.12;
        const double z=p[1]-std::sin(p[2])*x;
        c[i]={320+F*(p[0]+std::cos(p[2])*x)/z,180+F*y/z};}
    return c;
}
double Cost(const double* p,const std::array<ImagePoint,4>& c){
    if(p[1]<.2||p[1]>12||std::abs(p[2])>1.2)return 1e20;
    auto q=Project(p);double sum=0;for(int i=0;i<4;++i)sum+=std::pow(q[i].u-c[i].u,2)+std::pow(q[i].v-c[i].v,2);return sum;
}
// Calibrated planar fit: camera height/pitch/roll are fixed by the research model.
double Refine(double* p,const std::array<ImagePoint,4>& c){
    double lambda=.001,cost=Cost(p,c);
    for(int iteration=0;iteration<50;++iteration){
        const auto q=Project(p);double j[8][3]{},r[8]{};
        for(int i=0;i<4;++i){r[2*i]=q[i].u-c[i].u;r[2*i+1]=q[i].v-c[i].v;}
        for(int k=0;k<3;++k){double t[3]={p[0],p[1],p[2]};t[k]+=1e-5;auto d=Project(t);
            for(int i=0;i<4;++i){j[2*i][k]=(d[i].u-q[i].u)/1e-5;j[2*i+1][k]=(d[i].v-q[i].v)/1e-5;}}
        double a[8][9]{},step[3]{};
        for(int k=0;k<3;++k){for(int l=0;l<3;++l)for(int i=0;i<8;++i)a[k][l]+=j[i][k]*j[i][l];
            for(int i=0;i<8;++i)a[k][3]-=j[i][k]*r[i];a[k][k]+=lambda;}
        if(!Solve(a,3,step))break;
        double next[3]={p[0]+step[0],p[1]+step[1],p[2]+step[2]};const double candidate=Cost(next,c);
        if(candidate<cost){std::copy_n(next,3,p);if(cost-candidate<1e-10)break;cost=candidate;lambda*=.3;}else lambda*=10;
    }
    return Cost(p,c);
}
bool Finite(const VisualObservation& o){return std::isfinite(o.x)&&std::isfinite(o.y)&&std::isfinite(o.yaw)&&std::isfinite(o.speed)&&
    std::isfinite(o.positionErrorM)&&o.positionErrorM>=0&&std::isfinite(o.yawErrorRad)&&o.yawErrorRad>=0;}
}
bool VisualHistoryGate::Accept(const VisualObservation& o){
    if(!o.valid||!Finite(o))return false;
    if(!history_.empty()&&o.streamId!=history_.back().streamId)history_.clear();
    if(!history_.empty()&&o.captureUs<=history_.back().captureUs)return false;
    while(!history_.empty()&&o.captureUs-history_.front().captureUs>500000)history_.pop_front();
    for(const auto& old:history_){
        const double dt=(o.captureUs-old.captureUs)/1000000.;
        // Heading between two endpoints can deviate by at most max_w * dt / 2
        // beyond their uncertainty intervals. 2 cm is an INITIAL pixel-jitter
        // tolerance, calibrated on prior runs, not a guaranteed noise bound.
        const double angle=std::min(Pi/2,std::max(std::abs(o.yaw)+o.yawErrorRad,
            std::abs(old.yaw)+old.yawErrorRad)+.8*dt/2);
        if(std::abs(o.y-old.y)>.30*dt*std::sin(angle)+.02)return false;
    }
    history_.push_back(o);if(history_.size()>16)history_.pop_front();return true;
}
MarkerRecognizer::MarkerRecognizer(const std::string& task):markerX_(task=="T1"?4:2.8){if(task!="T1"&&task!="T2")throw std::runtime_error("unknown visual task");}
VisualObservation MarkerRecognizer::Process(const ReceivedLuma& im){
    VisualObservation o;o.frameId=im.frameId;o.streamId=im.streamId;o.captureUs=im.captureUs;o.receivedUs=im.receivedUs;
    auto reject=[&](const char* why){o.reason=why;return o;};
    if(!im.identityMatched)return reject("identity_unmatched");
    if(!im.frameId||!im.streamId||!im.captureUs||im.receivedUs<im.captureUs)return reject("invalid_metadata");
    if(im.streamId!=stream_){previous_={};history_.Clear();stream_=im.streamId;lastFrame_=0;lastCapture_=0;}
    if(im.frameId<=lastFrame_||im.captureUs<=lastCapture_)return reject("out_of_order");
    lastFrame_=im.frameId;lastCapture_=im.captureUs;
    if(!im.data||im.width!=640||im.height!=360||im.pitch<640||im.pitch>8192||im.bytes<size_t(im.pitch)*360)return reject("invalid_pixels");
    if(im.receivedUs-im.captureUs>200000)return reject("stale_at_receive");
    auto pixel=[&](int x,int y){return im.data[size_t(y)*im.pitch+x];};
    // The diagnostic identity band [0,39] is never searched or sampled.
    std::vector<uint8_t> visited(640*360);std::vector<int> component;std::vector<VisualObservation> candidates;
    bool small=false,patternFailure=false,geometryFailure=false,poseAmbiguity=false;
    for(int y=41;y<359;++y)for(int x=1;x<639;++x){
        const int index=y*640+x;if(visited[index]||pixel(x,y)>=80)continue;
        component.clear();component.push_back(index);visited[index]=1;int left=x,right=x,top=y,bottom=y;
        for(size_t cursor=0;cursor<component.size();++cursor){const int at=component[cursor],px=at%640,py=at/640;
            left=std::min(left,px);right=std::max(right,px);top=std::min(top,py);bottom=std::max(bottom,py);
            for(int n:{at-1,at+1,at-640,at+640}){const int nx=n%640,ny=n/640;if(nx<1||nx>638||ny<41||ny>358||visited[n]||pixel(nx,ny)>=80)continue;visited[n]=1;component.push_back(n);}}
        const int w=right-left+1,h=bottom-top+1;if(w<20||h<20){if(w>8&&h>8)small=true;continue;}
        if(left<=1||right>=638||top<=41||bottom>=358||w>400||h>300||w>h*3||h>w*3)continue;
        std::vector<ImagePoint> upper,lower,west,east;
        // Integrate pixel coverage across each edge, then fit lines using many pixels.
        auto whiteFraction=[](double value){return std::clamp((value-16)/219,0.,1.);};
        for(int u=left+w/6;u<=right-w/6;++u){
            int a=std::max(42,top-2),b=std::min(357,bottom+2);while(a<=bottom&&pixel(u,a)>=125.5)++a;while(b>=top&&pixel(u,b)>=125.5)--b;
            if(a<b){upper.push_back({double(u)+.5,double(a)-1+whiteFraction(pixel(u,a-1))+whiteFraction(pixel(u,a))+whiteFraction(pixel(u,a+1))});
                lower.push_back({double(u)+.5,double(b)+2-whiteFraction(pixel(u,b-1))-whiteFraction(pixel(u,b))-whiteFraction(pixel(u,b+1))});}}
        for(int v=top+h/6;v<=bottom-h/6;++v){
            int a=std::max(2,left-2),b=std::min(637,right+2);while(a<=right&&pixel(a,v)>=125.5)++a;while(b>=left&&pixel(b,v)>=125.5)--b;
            if(a<b){west.push_back({double(v)+.5,double(a)-1+whiteFraction(pixel(a-1,v))+whiteFraction(pixel(a,v))+whiteFraction(pixel(a+1,v))});
                east.push_back({double(v)+.5,double(b)+2-whiteFraction(pixel(b-1,v))-whiteFraction(pixel(b,v))-whiteFraction(pixel(b+1,v))});}}
        Line a,b,c,d;if(!a.Fit(upper)||!b.Fit(lower)||!c.Fit(west)||!d.Fit(east))continue;
        auto candidate=o;candidate.corners={Intersect(a,c),Intersect(a,d),Intersect(b,d),Intersect(b,c)};
        double minSide=1e9;bool inside=true;for(int i=0;i<4;++i){auto q=candidate.corners[i],r=candidate.corners[(i+1)%4];minSide=std::min(minSide,std::hypot(q.u-r.u,q.v-r.v));inside&=q.u>1&&q.u<639&&q.v>41&&q.v<359;}
        if(!inside||minSide<20){small=true;continue;}candidate.minSidePx=minSide;
        double homography[8];if(!Homography(candidate.corners,homography)){geometryFailure=true;continue;}
        bool pattern=true;
        for(int row=0;row<6;++row)for(int col=0;col<6;++col){
            const bool white=row>0&&row<5&&col>0&&col<5&&((0x8B35>>((row-1)*4+col-1))&1);
            int sum=0;for(double oy:{-.15,0.,.15})for(double ox:{-.15,0.,.15}){
                auto q=Map(homography,-.12+(col+.5+ox)*.04,-.12+(row+.5+oy)*.04);
                if(q.u<1||q.u>=639||q.v<41||q.v>=359){pattern=false;continue;}sum+=pixel(int(q.u),int(q.v));}
            if(white?sum<9*165:sum>9*100)pattern=false;
        }
        if(!pattern){patternFailure=true;continue;}
        const double depth=F/(homography[4]-180*homography[7]);
        double pose[3]={(homography[2]-320)*depth/F,depth,0};
        double best=1e20;std::array<std::array<double,4>,3> solutions{};int si=0;
        for(double start:{-.6,0.,.6}){double trial[3]={pose[0],pose[1],start};double cost=Refine(trial,candidate.corners);
            solutions[si++]={trial[0],trial[1],trial[2],cost};if(cost<best){best=cost;std::copy_n(trial,3,pose);}}
        if(!std::isfinite(best)||best>16||pose[1]<=.2){geometryFailure=true;continue;}
        bool ambiguous=false;
        for(auto solution:solutions)if(solution[3]<best+.08&&std::abs(solution[2]-pose[2])>.12){
            // Previous image estimate may select the branch; never simulator truth.
            if(previous_.valid&&im.captureUs-previous_.captureUs<=200000){
                const double oldDistance=std::abs(Wrap(pose[2]-previous_.yaw)),newDistance=std::abs(Wrap(solution[2]-previous_.yaw));
                if(std::abs(oldDistance-newDistance)<.05)ambiguous=true;
                else if(newDistance<oldDistance){std::copy_n(solution.data(),3,pose);best=solution[3];}
            }else ambiguous=true;
        }
        if(ambiguous){poseAmbiguity=true;continue;}
        const double s=std::sin(pose[2]),co=std::cos(pose[2]);
        candidate.x=markerX_-s*pose[0]-co*pose[1]-.1*co;
        candidate.y=co*pose[0]-s*pose[1]-.1*s;candidate.yaw=pose[2];candidate.reprojectionRms=std::sqrt(best/4);
        // Quantisation grows rapidly for a distant single marker. Pilot values are logged.
        const double conditioning=2*candidate.reprojectionRms*pose[1]*pose[1]/(F*.24*.24);
        candidate.positionErrorM=.025+pose[1]*.015+conditioning*pose[1];
        candidate.yawErrorRad=.03+pose[1]*.015+conditioning;
        candidate.valid=Finite(candidate);candidate.reason=candidate.valid?"ok":"nonfinite_pose";
        if(candidate.valid)candidates.push_back(candidate);
    }
    if(candidates.size()>1)return reject("ambiguous_markers");
    if(candidates.empty())return reject(poseAmbiguity?"ambiguous_pose":patternFailure?"pattern_mismatch":geometryFailure?"pose_rejected":small?"marker_too_small":"marker_missing");
    o=candidates[0];
    if(previous_.valid&&im.captureUs>previous_.captureUs&&im.captureUs-previous_.captureUs<=200000){
        const double dt=(im.captureUs-previous_.captureUs)/1000000.;
        const double distance=std::hypot(o.x-previous_.x,o.y-previous_.y);
        // Reject physically implausible innovations; tolerate 3 cm / 0.04 rad
        // frame-to-frame jitter. These are common INITIAL thresholds, not truth tests.
        if(distance>.30*dt+.03||std::abs(Wrap(o.yaw-previous_.yaw))>.8*dt+.04){o.valid=false;return reject("pose_jump");}
        o.speed=distance/dt;o.speedValid=true;
    }
    if(!history_.Accept(o)){o.valid=false;return reject("pose_history_inconsistent");}
    previous_=o;return o;
}
RemoteTaskController::RemoteTaskController(const std::string& task,double width):corridor_(task=="T1"),goalX_(corridor_?3.25:2),corridorWidth_(width){
    if((task!="T1"&&task!="T2")||!std::isfinite(width)||width<.65||width>.9)throw std::runtime_error("invalid controller task geometry");
}
MotionCommand RemoteTaskController::Update(const VisualObservation& o,uint64_t now){
    MotionCommand c;c.sequence=++sequence_;c.generatedUs=now;c.validUntilUs=now+100000;
    c.sourceFrameId=o.frameId;c.sourceStreamId=o.streamId;c.sourceCaptureUs=o.captureUs;
    if(o.captureUs&&now>=o.captureUs)c.ageMs=(now-o.captureUs)/1000.;
    auto stop=[&](const char* reason){c.reason=reason;align_=false;return c;};
    if(now<lastNow_)return stop("clock_reversed");lastNow_=now;
    if(!o.valid)return stop(o.reason.c_str());
    if(!Finite(o)||!o.frameId||!o.streamId||!o.captureUs||o.receivedUs<o.captureUs||o.receivedUs>now||o.captureUs>now)return stop("invalid_observation");
    c.ageMs=(now-o.captureUs)/1000.;
    if(now-o.captureUs>200000)return stop("observation_expired");
    if(stream_!=o.streamId){stream_=o.streamId;lastFrame_=0;lastCapture_=0;align_=false;}
    if(o.frameId<lastFrame_||o.captureUs<lastCapture_||(o.frameId==lastFrame_&&o.captureUs!=lastCapture_)||
        (o.frameId>lastFrame_&&o.captureUs<=lastCapture_))return stop("out_of_order");
    lastFrame_=o.frameId;lastCapture_=o.captureUs;
    c.observationUsed=true;
    const double dx=goalX_-o.x,dy=-o.y,rho=std::hypot(dx,dy),alpha=Wrap(std::atan2(dy,dx)-o.yaw);
    c.distanceM=rho;c.reason="image_feedback";
    // T1 must clear x=3.20 before stopping: the goal at 3.25 leaves only
    // 5 cm longitudinal tolerance. T2 has a 10 cm radial goal region.
    const double alignEnter=corridor_?.025:.08,alignLeave=corridor_?.04:.10;
    if(rho<=alignEnter)align_=true;else if(rho>alignLeave)align_=false;
    if(align_){c.state="ALIGN";c.w=std::clamp(1.5*Wrap(-o.yaw),-.8,.8);
        if(rho<=alignEnter&&std::abs(o.yaw)<=3*Pi/180&&o.speedValid&&o.speed<=.02){c.state="HOLD";c.w=0;c.estimatedComplete=true;}}
    else {c.state="APPROACH";c.w=std::clamp(1.5*alpha,-.8,.8);c.v=std::min(.30,.5*rho)*std::max(0.,std::cos(alpha));if(std::abs(alpha)>Pi/6)c.v=0;}
    const double age=(now-o.captureUs)/1000000.;
    const double error=o.positionErrorM+.30*age+.15*age*age;
    if(error>.25||o.yawErrorRad>.3){c.v=c.w=0;c.state="OBSERVE";c.reason="uncertainty_stop";c.estimatedComplete=false;return c;}
    // INITIAL worst speed, pending 100 ms command lifetime. No true velocity access.
    const double travel=.30*.1+.30*.30/(2*.60);
    // Only lateral travel consumes corridor width. Retain the full radial age
    // allowance for general uncertainty checks, and project its bounded heading
    // envelope for the wall check (including angular-rate and yaw uncertainty).
    const double ageTravel=.30*age+.15*age*age;
    const double ageHeading=std::min(Pi/2,std::abs(o.yaw)+o.yawErrorRad+.8*age);
    const double lateralError=o.positionErrorM+ageTravel*std::sin(ageHeading);
    c.wallMarginM=corridorWidth_/2-.20-std::abs(o.y)-lateralError-travel*std::abs(std::sin(o.yaw));
    // Retain 2 cm after subtracting pose/age uncertainty and pending/braking travel.
    // The former 5 cm reserve made the prescribed off-centre starts immobile.
    if(corridor_&&o.x<3.20&&c.wallMarginM<.02){c.v=0;c.reason="wall_margin_stop";
        c.w=std::clamp(-1.5*o.yaw,-.8,.8);c.estimatedComplete=false;}
    return c;
}
std::string MarkerRecognizer::ModelJson(){return R"json({
  "recognizer": "known_marker_planar_v2_history_gate",
  "pattern_hex": "8B35",
  "marker_side_m": 0.24,
  "camera": "640x360_hfov70_forward0.1_height0.35_pitch0_roll0",
  "diagnostic_rows_excluded": [
    0,
    39
  ],
  "dark_threshold": 80,
  "edge_threshold": 125.5,
  "edge_refinement": "three_pixel_coverage_integral",
  "minimum_side_px": 20,
  "maximum_reprojection_rms_px": 2,
  "planar_pose_multistart_rad": [
    -0.6,
    0,
    0.6
  ],
  "ambiguity_cost_delta_px2": 0.08,
  "ambiguity_angle_rad": 0.12,
  "image_deadline_us": 200000,
  "command_lifetime_us": 100000,
  "controller": "fixed_observe_approach_align_hold_v2",
  "gains": {
    "heading": 1.5,
    "distance": 0.5
  },
  "maximum_v_m_s": 0.3,
  "maximum_w_rad_s": 0.8,
  "rotate_only_above_deg": 30,
  "align_enter_m": {"T1": 0.025, "T2": 0.08},
  "align_leave_m": {"T1": 0.04, "T2": 0.1},
  "hold_yaw_deg": 3,
  "hold_speed_m_s": 0.02,
  "minimum_wall_margin_m": 0.02,
  "uncertainty_stop_m": 0.25,
  "uncertainty_stop_rad": 0.3,
  "marker_x_m": {
    "T1": 4.0,
    "T2": 2.8
  },
  "target_x_m": {
    "T1": 3.25,
    "T2": 2.0
  },
  "known_target_y_m": 0,
  "known_target_yaw_rad": 0,
  "conditioning": "k=2*rms*depth^2/(fx*0.24^2)",
  "position_allowance": "0.025+depth*0.015+k*depth m; INITIAL",
  "history_window_us": 500000,
  "history_maximum_observations": 16,
  "history_lateral_jitter_m": 0.02,
  "history_lateral_limit": "0.30*dt*sin(min(pi/2,max(abs(yaw)+yaw_allowance,abs(old_yaw)+old_yaw_allowance)+0.8*dt/2))+0.02",
  "yaw_allowance": "0.03+depth*0.015+k rad; INITIAL",
  "innovation_position_allowance_m": 0.03,
  "innovation_yaw_allowance_rad": 0.04,
  "age_allowance": "0.30*age+0.15*age^2 m",
  "lateral_age_allowance": "age_allowance*sin(min(pi/2,abs(yaw)+yaw_allowance+0.8*age))",
  "confidence_interval": false,
  "truth_input": false,
  "command_transport": "local_diagnostic_adapter",
  "command_udp": false,
  "closed_loop_validated": false
})json";}
}
