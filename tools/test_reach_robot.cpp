#include "../research/ReachRobotWorld.h"
#include "../research/ReachFoundation.h"
#include "../network/FrameIdentityLedger.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
int main() {
    int checks=0;
    auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
    try {
        using reach::RobotWorld;
        RobotWorld robot("T2");robot.Command(.3,0);
        for(int i=0;i<100;++i)robot.Step(.01);
        check(std::abs(robot.Truth().x-.15)<1e-8,"acceleration distance");
        check(std::abs(robot.Truth().v-.3)<1e-8,"speed limit");
        robot.Command(0,0);robot.Step(.01);
        check(robot.Truth().v>.29,"stop must not teleport velocity");
        for(int i=1;i<50;++i)robot.Step(.01);
        check(std::abs(robot.Truth().x-.225)<1e-8&&robot.Truth().v<1e-8,"braking distance");
        RobotWorld turn("T2");turn.Command(0,20);
        for(int i=0;i<100;++i)turn.Step(.01);
        check(turn.Truth().w<=.8&&turn.Truth().x==0,"turn clamp/no translation");
        RobotWorld wall("T1",.05,.17);wall.Command(.3,0);
        for(int i=0;i<1500;++i)wall.Step(.01);
        check(wall.Truth().collision&&wall.Truth().v==0&&!wall.Truth().success,"wall collision freezes robot");
        const auto x=wall.Truth().x;wall.Command(.3,0);wall.Step(.01);
        check(wall.Truth().x==x,"terminal collision stays frozen");
        for(const char* task:{"T1","T2"}) {
            RobotWorld goal(task);goal.Command(.3,0);
            const double stopX=std::string(task)=="T1"?3.175:1.925;
            while(goal.Truth().x<stopX)goal.Step(.01);
            goal.Command(0,0);
            for(int i=0;i<200;++i)goal.Step(.01);
            check(goal.Truth().success&&!goal.Truth().collision,"goal evaluator hold");
        }
        RobotWorld timeout("T2");for(int i=0;i<6000;++i)timeout.Step(.01);
        check(timeout.Truth().timeout&&!timeout.Truth().success,"60s deadline");
        RobotWorld bounds("T2",0,1.57079632679);bounds.Command(.3,0);
        for(int i=0;i<1000;++i)bounds.Step(.01);
        check(bounds.Truth().outOfBounds,"world boundary");
        bool rejected=false;try{robot.Step(.1);}catch(...){rejected=true;}check(rejected,"large physics step rejected");
        RobotWorld camera("T1");auto image=camera.CaptureNv12(0x12345678,0x87654321);
        auto identity=reach::ReadPixelIdentity(image.data(),640,640,360);
        check(identity.valid&&identity.frameId==0x12345678&&identity.streamId==0x87654321,"pixel identity roundtrip");
        camera.Command(.3,0);for(int i=0;i<100;++i)camera.Step(.01);
        auto moved=camera.CaptureNv12(0x12345678,0x87654321);
        check(image!=moved,"camera sees physical movement");
        for(int j=6;j<34;++j)for(int i=32;i<38;++i)image[j*640+i]=128;
        check(!reach::ReadPixelIdentity(image.data(),640,640,360).valid,"corrupt pixels rejected");
        net::FrameIdentityLedger ledger;
        check(ledger.Insert(100,{1,7,640,360,10,10})&&ledger.Insert(200,{2,7,640,360,20,20}),"register two inputs");
        check(!ledger.Insert(100,{3}),"duplicate PTS rejected");
        check(!ledger.Take(999),"missing PTS not matched to current input");
        auto second=ledger.Take(200),first=ledger.Take(100);
        check(second&&second->frameId==2&&first&&first->frameId==1&&first->captureUs==10,"out of order and delayed output identity");
        check(!ledger.Take(100),"duplicate output rejected");
        ledger.Insert(100,{3});ledger.Clear();check(!ledger.Take(100),"flush clears stale identity");
        check(ledger.Insert(100,{4,8}),"new stream can reuse timestamps after reset");ledger.Clear();
        for(int i=0;i<256;++i)check(ledger.Insert(i,{uint32_t(i+1)}),"bounded ledger input");
        check(!ledger.Insert(257,{258}),"ledger overflow fails closed");
        const auto cfg=reach::FoundationConfig::Load("config/reach_rt_g1_robot.json");
        check(reach::FoundationConfig::Parse(cfg.EffectiveJson()).stage=="robot_video","robot config roundtrip");
        std::cout<<"PASS "<<checks<<" assertions\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}
}
