#pragma once
#include "ReachCommandProtocol.h"
#include "ReachLinkModel.h"
#include <filesystem>
#include <memory>
namespace reach {
class BudgetTransport;
class CommandUdp {
public:
    CommandUdp(const std::string& session,const std::filesystem::path& directory,const std::string& scenario,const LinkConfig* link=nullptr,std::shared_ptr<BudgetTransport> budget=nullptr);
    uint16_t IngressPort() const;
    void SetFeedbackDestination(uint16_t port);
    uint64_t FeedbackDelivered() const;
    void SetStateDestination(uint16_t port);
    uint16_t RelaySourcePort() const;
    uint64_t StateDelivered() const;
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
