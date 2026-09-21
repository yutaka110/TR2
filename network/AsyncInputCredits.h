#pragma once
#include <cstdint>
namespace net {
// One input sample per asynchronous NeedInput event. A boolean loses credits
// when several events arrive before the next frame is available.
struct AsyncInputCredits {
    uint32_t pending=0,maximum=0;
    uint64_t events=0,submitted=0;
    bool Add(){if(pending>=256)return false;++events;++pending;if(pending>maximum)maximum=pending;return true;}
    bool Take(){if(!pending)return false;--pending;++submitted;return true;}
    void EndStream(){pending=0;}
};
}
