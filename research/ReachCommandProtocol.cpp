#include "ReachCommandProtocol.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace reach {
namespace {
constexpr std::string_view States[]={"OBSERVE","APPROACH","ALIGN","HOLD"};
constexpr std::string_view Reasons[]={"no_image","image_feedback","identity_unmatched","invalid_metadata","out_of_order","invalid_pixels",
    "stale_at_receive","nonfinite_pose","ambiguous_markers","ambiguous_pose","pattern_mismatch","pose_rejected","marker_too_small",
    "marker_missing","pose_jump","clock_reversed","invalid_observation","observation_expired","uncertainty_stop","wall_margin_stop","ok","pose_history_inconsistent"};
template<size_t N> uint16_t Code(const std::string& text,const std::string_view (&values)[N]){
    for(uint16_t i=0;i<N;++i)if(text==values[i])return i;throw std::runtime_error("unknown command enum");
}
void Put(CommandPacket& b,size_t at,uint64_t n,size_t size){for(size_t i=0;i<size;++i)b[at+size-i-1]=static_cast<uint8_t>(n>>(i*8));}
uint64_t Get(std::span<const uint8_t> b,size_t at,size_t size){uint64_t n=0;for(size_t i=0;i<size;++i)n=(n<<8)|b[at+i];return n;}
uint32_t Crc(std::span<const uint8_t> b){uint32_t crc=0xffffffff;for(uint8_t v:b){crc^=v;for(int i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320:0);}return ~crc;}
bool Values(const MotionCommand& c){
    if(!c.sequence||!c.generatedUs||c.generatedUs>UINT64_MAX-CommandLifetimeUs||c.validUntilUs!=c.generatedUs+CommandLifetimeUs||
        !std::isfinite(c.v)||!std::isfinite(c.w)||c.v<0||c.v>.30||std::abs(c.w)>.8)return false;
    if(c.sourceCaptureUs>c.generatedUs)return false;
    if((c.sourceFrameId==0)!=(c.sourceStreamId==0)||(c.sourceFrameId==0)!=(c.sourceCaptureUs==0))return false;
    if((c.v!=0||c.w!=0||c.estimatedComplete)&&(!c.sourceFrameId||c.generatedUs-c.sourceCaptureUs>200000))return false;
    if(c.state=="OBSERVE"&&(c.v!=0||c.w!=0))return false;
    if(c.estimatedComplete&&(c.state!="HOLD"||c.v!=0||c.w!=0))return false;
    return true;
}
}
CommandSessionId ParseCommandSession(const std::string& guid){
    if(guid.size()!=36||guid[8]!='-'||guid[13]!='-'||guid[18]!='-'||guid[23]!='-')throw std::runtime_error("invalid session GUID");
    auto nibble=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;throw std::runtime_error("invalid GUID hex");};
    CommandSessionId id{};size_t n=0;int high=-1;for(char c:guid){if(c=='-')continue;int v=nibble(c);if(high<0)high=v;else{id[n++]=static_cast<uint8_t>(high*16+v);high=-1;}}
    if(n!=16||high!=-1)throw std::runtime_error("invalid session hex count");
    if(std::all_of(id.begin(),id.end(),[](auto x){return x==0;}))throw std::runtime_error("nil command session");return id;
}
CommandPacket EncodeCommand(const CommandSessionId& session,const MotionCommand& c){
    if(!Values(c)||std::all_of(session.begin(),session.end(),[](auto x){return x==0;}))throw std::runtime_error("invalid command values");
    CommandPacket b{};Put(b,0,0x52434d44,4);Put(b,4,1,2);Put(b,6,b.size(),2);std::copy(session.begin(),session.end(),b.begin()+8);
    Put(b,24,c.sequence,8);Put(b,32,c.generatedUs,8);Put(b,40,c.validUntilUs,8);Put(b,48,c.sourceCaptureUs,8);
    Put(b,56,c.sourceFrameId,4);Put(b,60,c.sourceStreamId,4);
    Put(b,64,std::bit_cast<uint32_t>(static_cast<int32_t>(std::llround(c.v*1000000))),4);
    Put(b,68,std::bit_cast<uint32_t>(static_cast<int32_t>(std::llround(c.w*1000000))),4);
    Put(b,72,Code(c.state,States),2);Put(b,74,Code(c.reason,Reasons),2);Put(b,76,c.estimatedComplete?1:0,4);
    Put(b,84,Crc(std::span(b).first(84)),4);return b;
}
bool DecodeCommand(std::span<const uint8_t> b,CommandSessionId& session,MotionCommand& command,std::string& reason){
    auto reject=[&](const char* why){reason=why;return false;};
    if(b.size()!=CommandPacketBytes)return reject("invalid_length");
    if(Get(b,0,4)!=0x52434d44)return reject("invalid_magic");
    if(Get(b,4,2)!=1)return reject("unknown_version");
    if(Get(b,6,2)!=CommandPacketBytes)return reject("invalid_length");
    if(Get(b,80,4)!=0||Get(b,76,4)>1)return reject("reserved_bits");
    if(Get(b,84,4)!=Crc(b.first(84)))return reject("invalid_crc");
    const auto state=Get(b,72,2),why=Get(b,74,2);
    if(state>=std::size(States)||why>=std::size(Reasons))return reject("unknown_enum");
    MotionCommand c;c.sequence=Get(b,24,8);c.generatedUs=Get(b,32,8);c.validUntilUs=Get(b,40,8);c.sourceCaptureUs=Get(b,48,8);
    c.sourceFrameId=static_cast<uint32_t>(Get(b,56,4));c.sourceStreamId=static_cast<uint32_t>(Get(b,60,4));
    c.v=std::bit_cast<int32_t>(static_cast<uint32_t>(Get(b,64,4)))/1000000.;
    c.w=std::bit_cast<int32_t>(static_cast<uint32_t>(Get(b,68,4)))/1000000.;
    c.state=States[state];c.reason=Reasons[why];c.estimatedComplete=Get(b,76,4)==1;
    if(!Values(c))return reject("invalid_values");
    std::copy_n(b.begin()+8,16,session.begin());command=std::move(c);reason="decoded";return true;
}
CommandGuard::CommandGuard(CommandSessionId session,uint64_t origin):session_(session),origin_(origin),lastNow_(origin){}
std::string CommandGuard::Receive(std::span<const uint8_t> bytes,uint64_t now){
    if(now<lastNow_)return "clock_reversed";lastNow_=now;
    MotionCommand c;CommandSessionId session{};std::string reason;
    if(!DecodeCommand(bytes,session,c,reason))return reason;
    if(session!=session_)return "wrong_session";
    if(c.generatedUs>now)return "future_command";
    if(c.generatedUs<origin_)return "before_session";
    if(c.sequence==highest_)return "duplicate";
    if(c.sequence<highest_)return "reordered";
    if(accepted_.sequence&&c.generatedUs<accepted_.generatedUs)return "generation_reversed";
    // Expired, structurally valid packets advance ordering but never renew liveness.
    highest_=c.sequence;
    if(now>=c.validUntilUs)return "expired_on_arrival";
    accepted_=std::move(c);acceptedUs_=now;return "accepted";
}
CommandApplication CommandGuard::At(uint64_t now){
    CommandApplication out;out.command=accepted_;out.acceptedUs=acceptedUs_;
    if(now<lastNow_)out.reason="clock_reversed";
    else {
        lastNow_=now;const uint64_t heartbeat=accepted_.sequence?acceptedUs_:origin_;
        if(now-heartbeat>=CommandWatchdogUs)out.reason="watchdog_timeout";
        else if(!accepted_.sequence)out.reason="awaiting_command";
        else if(now>=accepted_.validUntilUs)out.reason="command_expired";
        else {out.live=true;out.reason="active";return out;}
    }
    out.command.v=out.command.w=0;out.command.estimatedComplete=false;return out;
}
}
