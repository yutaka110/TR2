#pragma once
#include "ReachVisualControl.h"
#include <array>
#include <span>

namespace reach {
using CommandSessionId=std::array<uint8_t,16>;
constexpr size_t CommandPacketBytes=88;
constexpr uint64_t CommandLifetimeUs=100000,CommandWatchdogUs=250000;
using CommandPacket=std::array<uint8_t,CommandPacketBytes>;
CommandSessionId ParseCommandSession(const std::string& guid);
CommandPacket EncodeCommand(const CommandSessionId& session,const MotionCommand& command);
bool DecodeCommand(std::span<const uint8_t> bytes,CommandSessionId& session,MotionCommand& command,std::string& reason);
struct CommandApplication {
    MotionCommand command;
    uint64_t acceptedUs=0;
    bool live=false;
    std::string reason="awaiting_command";
};
// Robot-side receiver state. No controller, world, or shared observation input.
class CommandGuard {
public:
    CommandGuard(CommandSessionId session,uint64_t originUs);
    std::string Receive(std::span<const uint8_t> bytes,uint64_t nowUs);
    CommandApplication At(uint64_t nowUs);
    uint64_t HighestSequence() const {return highest_;}
    uint64_t AcceptedSequence() const {return accepted_.sequence;}
    uint64_t LastAcceptedUs() const {return acceptedUs_;}
private:
    CommandSessionId session_;
    uint64_t origin_,lastNow_,highest_=0,acceptedUs_=0;
    MotionCommand accepted_;
};
}
