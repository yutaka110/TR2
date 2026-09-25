#pragma once
#include "ReachStateProtocol.h"
#include <filesystem>
#include <memory>
namespace reach {
class BudgetTransport;
class StateFeedbackUdp {
public:
    StateFeedbackUdp(const std::string& session,const std::filesystem::path& directory,std::shared_ptr<BudgetTransport> budget);
    ~StateFeedbackUdp();
    uint16_t Port() const;
    void Start(uint64_t origin,uint64_t duration,uint16_t ingress,uint16_t sourcePort);
    void Send(StateReport report); // Receiver-side serialization only.
    void Poll(); // Sender-side recvfrom only. No snapshot argument.
    SenderStateEstimate Estimate(); // Uses accepted datagrams and local clock only.
    void Finish(uint64_t expectedDelivered);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
