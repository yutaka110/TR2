#pragma once
#include "ReachFoundation.h"
#include "ReachRobotWorld.h"
#include "ReachVisualControl.h"
#include <memory>

namespace reach {
struct RobotVideoView {
    uint64_t captured=0,encoded=0,sent=0,decoded=0,matched=0,errors=0;
    uint32_t frameId=0,streamId=0;
    double ageMs=0;
    std::vector<uint8_t> bgra;
    VisualObservation observation;
    uint64_t recognized=0,rejected=0;
};
class RobotVideo {
public:
    RobotVideo(const FoundationConfig& config,ResearchSession& session);
    ~RobotVideo();
    void StartLink(uint64_t originUs);
    void Capture(const RobotWorld& world);
    RobotVideoView View();
    VisualObservation Observation();
    void Check();
    bool Finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
