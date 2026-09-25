#pragma once
#include "ReachFoundation.h"
#include "ReachRobotWorld.h"
#include "ReachVisualControl.h"
#include "ReachStateProtocol.h"
#include <memory>

namespace reach {
class BudgetTransport;
struct RobotVideoView {
    uint64_t captured=0,encoded=0,sent=0,decoded=0,matched=0,errors=0;
    uint32_t frameId=0,streamId=0;
    uint32_t lastDecodedFrameId=0;
    double ageMs=0;
    std::vector<uint8_t> bgra;
    VisualObservation observation;
    uint64_t recognized=0,rejected=0;
};
class RobotVideo {
public:
    RobotVideo(const FoundationConfig& config,ResearchSession& session,std::shared_ptr<BudgetTransport> budget=nullptr);
    uint16_t SenderPort() const;
    void SetFeedbackIngress(uint16_t port);
    void FinishFeedback(uint64_t expected);
    ~RobotVideo();
    void StartLink(uint64_t originUs);
    void Capture(const RobotWorld& world);
    void RecordControl(const MotionCommand& command);
    StateReport ReceiverNotification(); // Receiver-side producer only; sender uses received RSTA bytes.
    void PublishBaselineState(const SenderStateEstimate&); // Sender-side, masked before planner access.
    RobotVideoView View();
    VisualObservation Observation();
    void Check();
    bool Finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
