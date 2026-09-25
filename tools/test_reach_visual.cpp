#include "../research/ReachVisualControl.h"
#include "../research/ReachRobotWorld.h"
#include "../research/ReachFoundation.h"
#include "../network/AsyncInputCredits.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>

int main(int argc,char** argv){
    int checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
    try{
        using namespace reach;
        net::AsyncInputCredits credits;
        check(!credits.Take(),"no asynchronous input without event");
        check(credits.Add()&&credits.Add()&&credits.pending==2,"retain two NeedInput events");
        check(credits.Take()&&credits.pending==1,"one submission consumes only one credit");
        check(credits.Take()&&credits.pending==0&&credits.events==credits.submitted,"all credits consumed exactly once");
        credits.Add();credits.EndStream();check(credits.pending==0&&credits.maximum==2,"end of stream clears pending credits");
        std::ofstream calibration(argc>1?argv[1]:"visual_calibration.csv");calibration.precision(12);
        calibration<<"task,initial_y,initial_yaw,sample,true_x,true_y,true_yaw,valid,reason,estimated_x,estimated_y,estimated_yaw,position_error,yaw_error,reprojection_rms,min_side\n";
        int valid=0,total=0;double maxPosition=0,maxYaw=0;
        for(std::string task:{"T1","T2"})for(double y:{-1.,0.,1.})for(double yaw:{-1.,0.,1.}){
            RobotWorld world(task,y*(task=="T1"?.05:.15),yaw*(task=="T1"?5:10)*3.141592653589793/180);
            MarkerRecognizer recognizer(task);
            for(int sample=0;sample<3;++sample){
                if(sample){world.Command(.15,0);for(int i=0;i<500;++i)world.Step(.01);}
                auto pixels=world.CaptureNv12(sample+1,5);uint64_t capture=1000000+sample*5000000;
                auto o=recognizer.Process({pixels.data(),pixels.size(),640,360,640,uint32_t(sample+1),5,capture,capture+1000,true});
                const auto& t=world.Truth();const double pe=std::hypot(o.x-t.x,o.y-t.y),ye=std::abs(o.yaw-t.yaw);
                ++total;if(o.valid){++valid;maxPosition=std::max(maxPosition,pe);maxYaw=std::max(maxYaw,ye);}
                calibration<<task<<','<<y<<','<<yaw<<','<<sample<<','<<t.x<<','<<t.y<<','<<t.yaw<<','<<o.valid<<','<<o.reason<<','<<o.x<<','<<o.y<<','<<o.yaw<<','<<pe<<','<<ye<<','<<o.reprojectionRms<<','<<o.minSidePx<<'\n';
            }
        }
        RobotWorld world("T2");auto pixels=world.CaptureNv12(1,10);MarkerRecognizer detector("T2");
        auto input=ReceivedLuma{pixels.data(),pixels.size(),640,360,640,1,10,1000000,1001000,true};
        auto o=detector.Process(input);check(o.valid,"central marker detected");
        check(std::abs(o.x)<.12&&std::abs(o.y)<.12&&std::abs(o.yaw)<.08,"central known pose estimated from pixels");
        check(detector.Process(input).reason=="out_of_order","duplicate image rejected");
        input.frameId=2;input.captureUs+=33333;input.receivedUs+=33333;
        auto again=detector.Process(input);check(again.valid&&again.speedValid&&again.speed<.001,"stationary image speed");
        input.frameId=3;input.captureUs+=33333;input.receivedUs=input.captureUs+200001;
        check(detector.Process(input).reason=="stale_at_receive","stale pixel input rejected");
        input.streamId=11;input.frameId=1;input.receivedUs=input.captureUs+1000;
        check(detector.Process(input).valid,"stream resets history");
        input.identityMatched=false;check(detector.Process(input).reason=="identity_unmatched","unmatched pixels rejected");
        input.identityMatched=true;input.streamId=12;input.bytes=20;check(detector.Process(input).reason=="invalid_pixels","short pixel buffer rejected");input.bytes=pixels.size();
        std::fill(pixels.begin()+40*640,pixels.begin()+360*640,uint8_t{180});input.streamId=13;
        check(!detector.Process(input).valid,"barcode alone is not a marker");
        pixels=world.CaptureNv12(1,10);input.data=pixels.data();input.streamId=14;
        // Invert the known centre pattern while preserving the outer black border.
        for(int v=174;v<180;++v)for(int u=314;u<320;++u)pixels[v*640+u]=16;
        check(!detector.Process(input).valid,"changed internal pattern rejected");
        pixels=world.CaptureNv12(1,10);input.data=pixels.data();input.streamId=15;
        for(int v=145;v<215;++v)for(int u=285;u<355;++u)pixels[v*640+u+100]=pixels[v*640+u];
        check(detector.Process(input).reason=="ambiguous_markers","multiple matching markers rejected");
        RemoteTaskController control("T2",.75);VisualObservation obs;auto cmd=control.Update(obs,1000000);
        check(cmd.v==0&&cmd.w==0&&cmd.state=="OBSERVE","no observation stops");
        check(!cmd.observationUsed,"no image is not counted as control use");
        obs=o;obs.receivedUs=1001000;cmd=control.Update(obs,1050000);
        check(cmd.v>0&&cmd.sourceFrameId==o.frameId&&cmd.validUntilUs==1150000,"valid image creates bounded forward command with provenance");
        check(cmd.observationUsed,"verified fresh image counted as control use");
        check(control.Update(obs,1200000).observationUsed,"capture deadline equality remains usable");
        check(control.Update(obs,1200001).reason=="observation_expired","deadline uses capture not receive");
        check(!control.Update(obs,1200002).observationUsed,"expired source ID does not imply control use");
        obs.captureUs=1250000;obs.receivedUs=1251000;obs.frameId=2;obs.x=2;obs.y=0;obs.yaw=.2;
        cmd=control.Update(obs,1300000);check(cmd.v==0&&cmd.w<0&&cmd.state=="ALIGN","align rotates toward target yaw");
        obs.yaw=0;obs.speedValid=true;obs.speed=0;cmd=control.Update(obs,1300001);
        check(cmd.state=="HOLD"&&cmd.estimatedComplete,"image completion event separate from truth");
        obs.x=1.8;cmd=control.Update(obs,1300002);check(cmd.state=="APPROACH","align hysteresis exits outside 10cm");
        obs.yaw=1;cmd=control.Update(obs,1300003);check(cmd.v==0&&cmd.w<0,"large heading error rotates without translation");
        obs.positionErrorM=.3;cmd=control.Update(obs,1300004);check(cmd.v==0&&cmd.w==0&&cmd.reason=="uncertainty_stop","large uncertainty stops");
        check(cmd.observationUsed,"uncertainty braking still uses the valid image estimate");
        obs.positionErrorM=.04;obs.x=std::numeric_limits<double>::quiet_NaN();check(control.Update(obs,1300005).reason=="invalid_observation","nonfinite estimate rejected");
        obs.x=0;obs.captureUs=1400000;obs.receivedUs=1400001;obs.frameId=3;obs.y=.14;obs.yaw=.1;
        RemoteTaskController corridor("T1",.75);cmd=corridor.Update(obs,1450000);
        check(cmd.v==0&&cmd.reason=="wall_margin_stop"&&cmd.w<0,"estimated corridor margin brakes and aligns");
        obs.valid=false;obs.reason="marker_missing";check(corridor.Update(obs,1450001).v==0,"latest missing image invalidates old valid observation");
        check(std::abs(corridor.Update(obs,1450002).ageMs-50.002)<1e-8,"rejected observation still reports actual capture age");
        obs=o;obs.captureUs=1500000;obs.receivedUs=1500001;obs.frameId=5;control.Update(obs,1550000);
        obs.frameId=4;obs.captureUs=1499999;check(control.Update(obs,1550001).reason=="out_of_order","controller rejects older image");
        check(control.Update(obs,1500000).reason=="clock_reversed","clock reversal stops");
        // Regression: stopping 8 cm short is valid for T2 but not beyond the T1 exit.
        obs=o;obs.captureUs=1600000;obs.receivedUs=1601000;obs.frameId=6;
        obs.x=3.175;obs.y=0;obs.yaw=0;obs.positionErrorM=.02;obs.yawErrorRad=.03;
        obs.speedValid=true;obs.speed=0;
        RemoteTaskController exitControl("T1",.75);
        check(exitControl.Update(obs,1650000).v>0,"T1 must advance beyond exit instead of stopping 8 cm short");
        obs.x=3.23;check(exitControl.Update(obs,1650001).estimatedComplete,"T1 holds inside narrower exit target");
        obs.x=3.215;check(exitControl.Update(obs,1650002).state=="ALIGN","T1 approach hysteresis avoids chatter");
        obs.x=3.20;check(exitControl.Update(obs,1650003).state=="APPROACH","T1 leaves align when outside 4 cm");
        VisualHistoryGate history;obs=o;obs.valid=true;obs.y=0;obs.yaw=0;obs.yawErrorRad=.09;obs.captureUs=2000000;
        check(history.Accept(obs),"history starts without truth");
        obs.captureUs+=33333;obs.y=.01;check(history.Accept(obs),"history allows bounded image jitter");
        obs.captureUs+=33333;obs.y=.02;check(history.Accept(obs),"history allows bounded motion");
        obs.captureUs+=33333;obs.y=.03;check(!history.Accept(obs),"multi-frame gate rejects gradual lateral drift");
        obs.captureUs+=600000;check(history.Accept(obs),"expired history permits reacquisition");
        check(!history.Accept(obs),"history rejects duplicate time");
        obs.streamId++;obs.captureUs=1;check(history.Accept(obs),"new stream resets image history");
        history.Clear();obs.yaw=.17;obs.y=0;obs.captureUs=3000000;
        for(int i=0;i<30;++i){obs.captureUs+=33333;obs.y+=.3*std::sin(.17)*.033333;check(history.Accept(obs),"history admits feasible lateral travel");}
        if(argc>2){
            std::ifstream replay(argv[2]);check(bool(replay),"history replay fixture exists");std::string line,group,previousGroup;
            std::getline(replay,line);int accepted=0,rejected=0,oldViolations=0,remainingViolations=0;
            while(std::getline(replay,line)){
                std::istringstream row(line);std::vector<std::string> fields;std::string value;
                while(std::getline(row,value,','))fields.push_back(value);check(fields.size()==11,"replay CSV shape");
                group=fields[0];if(group!=previousGroup){history.Clear();previousGroup=group;}
                VisualObservation sample;sample.valid=true;sample.streamId=1;sample.captureUs=std::stoull(fields[1]);
                sample.x=std::stod(fields[2]);sample.y=std::stod(fields[3]);sample.yaw=std::stod(fields[4]);
                sample.positionErrorM=std::stod(fields[5]);sample.yawErrorRad=std::stod(fields[6]);
                const double error=std::hypot(sample.x-std::stod(fields[7]),sample.y-std::stod(fields[8]));
                oldViolations+=error>sample.positionErrorM;
                if(history.Accept(sample)){++accepted;remainingViolations+=error>sample.positionErrorM+.005;}else ++rejected;
            }
            check(oldViolations==3&&remainingViolations==0,"history excludes prior under-covered estimates");
            check(accepted>29000&&rejected>0,"history replay retains useful observations");
            std::cout<<"History replay accepted "<<accepted<<", rejected "<<rejected<<", old violations "<<oldViolations<<", remaining "<<remainingViolations<<'\n';
        }
        auto cfg=FoundationConfig::Load("config/reach_rt_g1_robot.json");cfg.stage="visual_control";
        check(FoundationConfig::Parse(cfg.EffectiveJson()).stage=="visual_control","visual config roundtrip");
        check(valid>10,"calibration produced usable estimates");
        check(valid==total&&maxPosition<.01&&maxYaw<.005,"uncompressed pose calibration error stays below 1cm and 0.005rad");
        pixels=world.CaptureNv12(1,10);input.data=pixels.data();input.streamId=16;
        for(int v=150;v<210;++v)for(int u=285;u<320;++u)pixels[v*640+u]=180;
        check(!detector.Process(input).valid,"partially occluded marker rejected");
        pixels=world.CaptureNv12(1,10);auto original=pixels;
        // Affine squashing removes the sign cue in the perspective while retaining the ID.
        for(int v=145;v<215;++v)for(int u=285;u<355;++u){int source=320+int((u-320)/.9);pixels[v*640+u]=original[v*640+std::clamp(source,0,639)];}
        input.data=pixels.data();input.streamId=17;auto ambiguous=detector.Process(input);
        check(!ambiguous.valid,"ambiguous or inconsistent perspective rejected");
        std::cout<<"PASS "<<checks<<" assertions; calibration "<<valid<<'/'<<total<<" accepted; max position error "<<maxPosition<<" m; max yaw error "<<maxYaw<<" rad\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
}
