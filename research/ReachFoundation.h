#pragma once
#include "ReachBufferedLog.h"
#include "ReachLinkModel.h"
#include "ReachIpBudget.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace reach {
struct FoundationConfig {
    std::string stage = "foundation", encoder = "auto";
    std::string commandScenario="normal";
    bool boundedLink=false;
    bool budgeted=false;IpBudgetConfig ipBudget;
    bool stateFeedback=false;
    std::string baseline;double baselineLambda=.1;
    LinkConfig uplink,downlink;
    bool HasVisualControl() const {return stage=="visual_control"||stage=="command_udp";}
    double initialY = 0, initialYaw = 0, corridorWidth = 0.75;
    std::string task;
    uint32_t seed = 0;
    uint64_t durationUs = 0;
    uint32_t physicsHz = 0, cameraHz = 0, controlHz = 0;
    uint32_t maxCatchupSteps = 0;
    std::string raw;
    static FoundationConfig Parse(const std::string& text);
    static FoundationConfig Load(const std::filesystem::path& path);
    std::string EffectiveJson() const;
};

// Same steady_clock epoch and microsecond unit as the existing RNVP timestamps.
uint64_t MonotonicUs();
std::string Sha256(const std::string& bytes);
std::string ReadFile(const std::filesystem::path& path, uint64_t maxBytes);
std::string PathUtf8(const std::filesystem::path& path);

// Rational deadlines avoid the cumulative drift caused by rounding 30 Hz to 33 ms.
class PeriodicDeadline {
public:
    PeriodicDeadline(uint64_t startUs, uint32_t rateHz, uint32_t maxCatchup);
    uint32_t Poll(uint64_t nowUs); // Throws on excessive lateness, never silently skips work.
    uint64_t NextUs() const;
    uint64_t Count() const { return nextIndex_ - 1; }
private:
    uint64_t startUs_, nextIndex_ = 1;
    uint32_t rateHz_, maxCatchup_;
};

class ResearchSession {
public:
    ResearchSession(const FoundationConfig& config, const std::filesystem::path& outputRoot,
                    const std::filesystem::path& executable, bool headless);
    ~ResearchSession();
    ResearchSession(const ResearchSession&) = delete;
    ResearchSession& operator=(const ResearchSession&) = delete;
    const std::string& Id() const { return id_; }
    const std::filesystem::path& Directory() const { return directory_; }
    uint64_t StartUs() const { return startUs_; }
    void Event(const std::string& type, const std::string& detail, uint64_t count = 0);
    void Finish(const std::string& status, const std::string& reason,
                uint64_t physicsTicks, uint64_t cameraTicks, uint64_t controlTicks);
private:
    void Manifest(const FoundationConfig& config, const std::filesystem::path& executable, bool headless);
    std::string id_;
    std::filesystem::path directory_;
    BufferedLog events_;
    std::mutex mutex_;
    uint64_t startUs_ = 0, sequence_ = 0;
    bool finished_ = false;
};

// Returns false only when research mode was not requested. Invalid mode requests
// return an error instead of silently launching the legacy experiment.
struct ResearchUiNavigation {
    bool returnToLegacy=false;
    // Internal lifecycle smoke checks; never enabled by the live-view button.
    bool diagnosticAutoReturn=false;
    uint64_t diagnosticReturnAfterUs=0;
};
bool RunResearchModeFromEnvironment(int& exitCode, ResearchUiNavigation* navigation=nullptr);
}
