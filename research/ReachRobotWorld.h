#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace reach {
struct RobotTruth {
    double x=0,y=0,yaw=0,v=0,w=0,elapsed=0,hold=0;
    bool collision=false,outOfBounds=false,success=false,timeout=false;
};
// Evaluator/simulator data. A future image controller must never receive this type.
class RobotWorld {
public:
    static constexpr double Radius=.20, MaxV=.30, MaxW=.8, Accel=.30, Brake=.60, AngularAccel=1.6;
    static constexpr uint32_t Width=640, Height=360;
    RobotWorld(std::string task,double initialY=0,double initialYaw=0,double corridorWidth=.75);
    void Command(double v,double w);
    void Step(double dt);
    const RobotTruth& Truth() const { return truth_; }
    const std::string& Task() const { return task_; }
    double CorridorWidth() const { return corridorWidth_; }
    std::vector<uint8_t> CaptureNv12(uint32_t frameId,uint32_t streamId) const;
    static std::string ModelJson(bool visualControl=false,bool commandUdp=false);
private:
    RobotTruth truth_;
    std::string task_;
    double corridorWidth_,commandV_=0,commandW_=0;
};
struct PixelIdentity { uint32_t frameId=0,streamId=0; bool valid=false; };
// Diagnostic overlay is checked from decoded pixels, independently of codec PTS.
PixelIdentity ReadPixelIdentity(const uint8_t* y,uint32_t pitch,uint32_t width,uint32_t height);
}
