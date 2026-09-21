#pragma once
#include "ReachLinkModel.h"
#include <filesystem>
#include <memory>
#include <string>
namespace reach {
// Loopback-only UDP relay. Each direction owns its socket, FIFO and worker.
class DatagramLink {
public:
    DatagramLink(const LinkConfig& config,const std::filesystem::path& directory,const std::string& direction,uint16_t destinationPort);
    ~DatagramLink();
    uint16_t Port() const;
    uint16_t SourcePort() const;
    void Start(uint64_t originUs);
    void Check() const;
    void Finish(); // Cancels remaining packets explicitly, never unbounded drain.
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
