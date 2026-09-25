#pragma once
#include <cstdint>
#include <functional>

namespace net {
// Local observation only. Never a sender-visible/shared-memory feedback channel.
struct DecodeTraceEvent {
    const char* event="";
    uint32_t frameId=0,streamId=0;
    uint64_t timeUs=0,captureUs=0,generation=0,inputUs=0;
    bool idr=false,trusted=false;
    const char* reason="";
};
using DecodeTraceObserver=std::function<void(const DecodeTraceEvent&)>;
}
