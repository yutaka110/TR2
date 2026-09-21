#pragma once
#include "ReachCommandProtocol.h"
#include "ReachLinkModel.h"
#include <filesystem>
#include <memory>
namespace reach {
class CommandUdp {
public:
    CommandUdp(const std::string& session,const std::filesystem::path& directory,const std::string& scenario,const LinkConfig* link=nullptr);
    ~CommandUdp();
    void Start(uint64_t originUs);
    void Offer(const MotionCommand& command);
    void Pump();
    CommandApplication Application(uint64_t nowUs);
    void Finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
