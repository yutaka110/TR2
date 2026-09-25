#pragma once
#include "ReachDeferredLog.h"

namespace reach {
// Research CSV/event logs stay in bounded memory until explicit close. Standard
// filebuf buffering could still flush while holding a real-time control lock.
// A forced termination loses these logs and can never certify a trial.
class BufferedLog : public DeferredLog {
public:
    BufferedLog():DeferredLog(32ull*1024*1024){}
};
}
