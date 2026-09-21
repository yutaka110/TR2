#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>

namespace net {
struct FrameIdentity {
    uint32_t frameId=0, streamId=0, width=0, height=0;
    uint64_t ptsUs=0, captureUs=0, encoderOutputUs=0, sendUs=0, receiveUs=0;
};
// Owned by one codec worker. Entries exist only for accepted input samples.
// A missing/duplicate identity is an error, never a reason to use current input.
class FrameIdentityLedger {
public:
    bool Insert(int64_t pts100ns, const FrameIdentity& identity) {
        if(pts100ns<0||entries_.size()>=256||identity.frameId==0) return false;
        return entries_.emplace(pts100ns,identity).second;
    }
    std::optional<FrameIdentity> Take(int64_t pts100ns) {
        const auto found=entries_.find(pts100ns);
        if(found==entries_.end()) return {};
        auto identity=found->second; entries_.erase(found); return identity;
    }
    void Clear() { entries_.clear(); }
    size_t Size() const { return entries_.size(); }
private:
    std::map<int64_t,FrameIdentity> entries_;
};
}
