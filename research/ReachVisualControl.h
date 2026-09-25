#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <deque>

namespace reach {
// Receiver-owned values only. Deliberately no simulator/world header or pointer.
struct ReceivedLuma {
    const uint8_t* data=nullptr;
    size_t bytes=0;
    uint32_t width=640,height=360,pitch=640,frameId=0,streamId=0;
    uint64_t captureUs=0,receivedUs=0;
    bool identityMatched=false;
};
struct ImagePoint { double u=0,v=0; };
struct VisualObservation {
    uint32_t frameId=0,streamId=0;
    uint64_t captureUs=0,receivedUs=0;
    bool valid=false,speedValid=false;
    std::string reason="no_image";
    double x=0,y=0,yaw=0,speed=0,reprojectionRms=0,minSidePx=0;
    // INITIAL engineering allowances, not a confidence interval/safety guarantee.
    double positionErrorM=0,yawErrorRad=0;
    std::array<ImagePoint,4> corners{};
};
// Image-only history gate. No simulator pose or controller command is available.
class VisualHistoryGate {
public:
    bool Accept(const VisualObservation& candidate);
    void Clear() { history_.clear(); }
private:
    std::deque<VisualObservation> history_;
};
class MarkerRecognizer {
public:
    explicit MarkerRecognizer(const std::string& task);
    VisualObservation Process(const ReceivedLuma& image);
    static std::string ModelJson();
private:
    double markerX_;
    VisualObservation previous_;
    VisualHistoryGate history_;
    uint32_t stream_=0,lastFrame_=0;
    uint64_t lastCapture_=0;
};
struct MotionCommand {
    uint64_t sequence=0,generatedUs=0,validUntilUs=0,sourceCaptureUs=0;
    uint32_t sourceFrameId=0,sourceStreamId=0;
    double v=0,w=0,ageMs=0,distanceM=0,wallMarginM=0;
    std::string state="OBSERVE",reason="no_image";
    bool estimatedComplete=false;
    // Local audit only; not part of the RCMD wire representation.
    bool observationUsed=false;
};
class RemoteTaskController {
public:
    RemoteTaskController(const std::string& task,double corridorWidth);
    MotionCommand Update(const VisualObservation& observation,uint64_t nowUs);
private:
    bool corridor_,align_=false;
    double goalX_,corridorWidth_;
    uint64_t sequence_=0,lastNow_=0,lastCapture_=0;
    uint32_t stream_=0,lastFrame_=0;
};
}
