#include "NetworkCsvLogger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <unordered_map>
#include <utility>
#include <vector>

namespace net {
namespace {

    std::string MakeTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);

        std::tm localTime{};
        localtime_s(&localTime, &time);

        std::ostringstream oss;
        oss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
        return oss.str();
    }

    bool NearlyEqual(double lhs, double rhs, double epsilon = 0.0001) {
        return std::fabs(lhs - rhs) <= epsilon;
    }

    bool MatchesCondition(
        const NetworkCondition& condition,
        bool enabled,
        double lossRate,
        uint32_t minDelayMs,
        uint32_t maxDelayMs,
        uint32_t burstLossLength,
        double duplicateRate = 0.0,
        double reorderRate = 0.0) {
        return condition.enabled == enabled &&
            NearlyEqual(condition.lossRate, lossRate) &&
            NearlyEqual(condition.duplicateRate, duplicateRate) &&
            NearlyEqual(condition.reorderRate, reorderRate) &&
            condition.minDelayMs == minDelayMs &&
            condition.maxDelayMs == maxDelayMs &&
            condition.burstLossLength == burstLossLength;
    }

    std::string EscapeCsv(std::string value) {
        const bool needsQuote =
            value.find_first_of(",\"\r\n") != std::string::npos;

        if (!needsQuote) {
            return value;
        }

        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped.push_back('"');

        for (char ch : value) {
            if (ch == '"') {
                escaped.push_back('"');
            }
            escaped.push_back(ch);
        }

        escaped.push_back('"');
        return escaped;
    }

    int Percent(double ratio) {
        return static_cast<int>(std::lround(ratio * 100.0));
    }

    double StartupWarmupSecFromEnv() {
        char* text = nullptr;
        size_t textLength = 0;
        if (_dupenv_s(&text, &textLength, "RNVP_STARTUP_WARMUP_SEC") != 0 ||
            text == nullptr ||
            textLength == 0) {
            if (text != nullptr) {
                std::free(text);
            }
            return 5.0;
        }

        char* end = nullptr;
        const double value = std::strtod(text, &end);
        const bool parsed = end != text;
        std::free(text);
        if (!parsed || !std::isfinite(value)) {
            return 5.0;
        }

        return (std::max)(0.0, (std::min)(value, 30.0));
    }

    uint64_t CounterDelta(uint64_t current, uint64_t previous) {
        return current >= previous ? current - previous : current;
    }

    bool IsTruthyEnvValue(const char* value) {
        if (value == nullptr || value[0] == '\0') {
            return true;
        }
        return _stricmp(value, "0") != 0 &&
            _stricmp(value, "false") != 0 &&
            _stricmp(value, "off") != 0 &&
            _stricmp(value, "no") != 0;
    }

    bool LogRotationEnabled() {
        char* text = nullptr;
        size_t textLength = 0;
        if (_dupenv_s(
                &text,
                &textLength,
                "RNVP_LOG_ROTATE_ENABLED") != 0 ||
            text == nullptr ||
            textLength == 0) {
            if (text != nullptr) {
                std::free(text);
            }
            return true;
        }

        const bool enabled = IsTruthyEnvValue(text);
        std::free(text);
        return enabled;
    }

    uint32_t EnvUint(
        const char* name,
        uint32_t defaultValue,
        uint32_t minValue,
        uint32_t maxValue) {
        char* text = nullptr;
        size_t textLength = 0;
        if (_dupenv_s(&text, &textLength, name) != 0 ||
            text == nullptr ||
            textLength == 0) {
            if (text != nullptr) {
                std::free(text);
            }
            return defaultValue;
        }

        char* end = nullptr;
        const unsigned long value = std::strtoul(text, &end, 10);
        const bool parsed = end != text;
        std::free(text);
        if (!parsed) {
            return defaultValue;
        }

        return (std::max)(
            minValue,
            (std::min)(static_cast<uint32_t>(value), maxValue));
    }

    uint64_t EnvBytesFromMb(
        const char* name,
        uint32_t defaultMb,
        uint32_t minMb,
        uint32_t maxMb) {
        const uint32_t mb = EnvUint(name, defaultMb, minMb, maxMb);
        return static_cast<uint64_t>(mb) * 1024ull * 1024ull;
    }

    std::vector<std::string> ParseCsvLine(const std::string& line) {
        std::vector<std::string> fields;
        std::string field;
        bool inQuotes = false;

        for (size_t i = 0; i < line.size(); ++i) {
            const char ch = line[i];
            if (inQuotes) {
                if (ch == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        field.push_back('"');
                        ++i;
                    }
                    else {
                        inQuotes = false;
                    }
                }
                else {
                    field.push_back(ch);
                }
            }
            else if (ch == '"') {
                inQuotes = true;
            }
            else if (ch == ',') {
                fields.push_back(field);
                field.clear();
            }
            else {
                field.push_back(ch);
            }
        }

        fields.push_back(field);
        return fields;
    }

    std::unordered_map<std::string, size_t> BuildColumnIndex(
        const std::vector<std::string>& header) {
        std::unordered_map<std::string, size_t> columns;
        for (size_t i = 0; i < header.size(); ++i) {
            columns.emplace(header[i], i);
        }
        return columns;
    }

    std::string JsonEscape(std::string value) {
        std::string escaped;
        escaped.reserve(value.size() + 8);
        for (char ch : value) {
            switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
            }
        }
        return escaped;
    }

    double CsvDouble(
        const std::vector<std::string>& fields,
        const std::unordered_map<std::string, size_t>& columns,
        const char* name) {
        const auto it = columns.find(name);
        if (it == columns.end() || it->second >= fields.size()) {
            return 0.0;
        }

        char* end = nullptr;
        const double value = std::strtod(fields[it->second].c_str(), &end);
        return end != fields[it->second].c_str() ? value : 0.0;
    }

    uint64_t CsvUint64(
        const std::vector<std::string>& fields,
        const std::unordered_map<std::string, size_t>& columns,
        const char* name) {
        const auto it = columns.find(name);
        if (it == columns.end() || it->second >= fields.size()) {
            return 0;
        }

        char* end = nullptr;
        const unsigned long long value =
            std::strtoull(fields[it->second].c_str(), &end, 10);
        return end != fields[it->second].c_str()
            ? static_cast<uint64_t>(value)
            : 0;
    }

    bool CsvBool(
        const std::vector<std::string>& fields,
        const std::unordered_map<std::string, size_t>& columns,
        const char* name) {
        return CsvUint64(fields, columns, name) != 0;
    }

    std::filesystem::path EventCsvPathFor(
        const std::filesystem::path& networkCsvPath) {
        const std::string stem = networkCsvPath.stem().string();
        const std::string suffix =
            stem.rfind("network_", 0) == 0
            ? stem.substr(std::string("network_").size())
            : stem;
        return networkCsvPath.parent_path() /
            ("network_events_" + suffix + ".csv");
    }

    std::filesystem::path RunSummaryPathFor(
        const std::filesystem::path& networkCsvPath) {
        const std::string stem = networkCsvPath.stem().string();
        const std::string suffix =
            stem.rfind("network_", 0) == 0
            ? stem.substr(std::string("network_").size())
            : stem;
        return networkCsvPath.parent_path() /
            ("network_run_summary_" + suffix + ".json");
    }

    void ExtractNetworkEventArtifacts(
        const std::filesystem::path& networkCsvPath) {
        if (networkCsvPath.empty() ||
            !std::filesystem::exists(networkCsvPath) ||
            networkCsvPath.filename().string().rfind("network_", 0) != 0 ||
            networkCsvPath.extension() != ".csv") {
            return;
        }

        const std::filesystem::path eventCsvPath =
            EventCsvPathFor(networkCsvPath);
        const std::filesystem::path summaryPath =
            RunSummaryPathFor(networkCsvPath);

        if (std::filesystem::exists(eventCsvPath) &&
            std::filesystem::exists(summaryPath)) {
            return;
        }

        std::ifstream input(networkCsvPath);
        if (!input) {
            return;
        }

        std::string headerLine;
        if (!std::getline(input, headerLine)) {
            return;
        }

        const std::vector<std::string> header = ParseCsvLine(headerLine);
        const std::unordered_map<std::string, size_t> columns =
            BuildColumnIndex(header);

        std::ofstream eventCsv(eventCsvPath, std::ios::out | std::ios::trunc);
        if (!eventCsv) {
            return;
        }
        eventCsv << headerLine << '\n';

        uint64_t totalRows = 0;
        uint64_t eventRows = 0;
        uint64_t qoe2Rows = 0;
        uint64_t qoe3Rows = 0;
        uint64_t recoveryEffectiveRows = 0;
        uint64_t retransmitEffectiveRows = 0;
        uint64_t recoveryRawRows = 0;
        uint64_t syncRiskRows = 0;
        uint64_t hardSyncLossRows = 0;
        uint64_t outputDropRows = 0;
        uint64_t freshnessDropRows = 0;
        uint64_t arrivalGapJitterRows = 0;

        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }

            ++totalRows;
            const std::vector<std::string> fields = ParseCsvLine(line);
            const double qoeScore =
                CsvDouble(fields, columns, "adaptiveQoeScore");
            const uint64_t recoveryRaw =
                CsvUint64(fields, columns, "adaptiveRecoveryDeadlineRawDelta");
            const uint64_t recoveryEffective =
                CsvUint64(
                    fields,
                    columns,
                    "adaptiveRecoveryDeadlineEffectiveDelta");
            const uint64_t staleRaw =
                CsvUint64(fields, columns, "adaptiveRetransmitStaleRawDelta");
            const uint64_t staleEffective =
                CsvUint64(
                    fields,
                    columns,
                    "adaptiveRetransmitStaleEffectiveDelta");
            const uint64_t recoveryNoise =
                CsvUint64(
                    fields,
                    columns,
                    "adaptiveRecoveryDeadlineNoiseDelta");
            const uint64_t syncRisk =
                CsvUint64(
                    fields,
                    columns,
                    "adaptiveRecoveryDeadlineSyncRiskDelta");
            const uint64_t hardSyncLoss =
                CsvUint64(
                    fields,
                    columns,
                    "adaptiveRecoveryDeadlineHardSyncLossDelta");
            const bool arrivalGapJitter =
                CsvBool(
                    fields,
                    columns,
                    "adaptiveArrivalGapJitterSpikeActive");
            const uint64_t outputDrops =
                CsvUint64(fields, columns, "outputQueueDroppedFramesDelta") +
                CsvUint64(fields, columns, "outputQueueDropEventsDelta");
            const uint64_t freshnessDrops =
                CsvUint64(
                    fields,
                    columns,
                    "receiveFreshnessDroppedFramesDelta");

            qoe2Rows += qoeScore >= 2.0 && qoeScore < 3.0 ? 1 : 0;
            qoe3Rows += qoeScore >= 3.0 ? 1 : 0;
            recoveryEffectiveRows += recoveryEffective > 0 ? 1 : 0;
            retransmitEffectiveRows += staleEffective > 0 ? 1 : 0;
            recoveryRawRows +=
                recoveryRaw > 0 || staleRaw > 0 || recoveryNoise > 0
                ? 1
                : 0;
            syncRiskRows += syncRisk > 0 ? 1 : 0;
            hardSyncLossRows += hardSyncLoss > 0 ? 1 : 0;
            outputDropRows += outputDrops > 0 ? 1 : 0;
            freshnessDropRows += freshnessDrops > 0 ? 1 : 0;
            arrivalGapJitterRows += arrivalGapJitter ? 1 : 0;

            const bool important =
                qoeScore >= 2.0 ||
                recoveryRaw > 0 ||
                recoveryEffective > 0 ||
                staleRaw > 0 ||
                staleEffective > 0 ||
                recoveryNoise > 0 ||
                syncRisk > 0 ||
                hardSyncLoss > 0 ||
                arrivalGapJitter ||
                outputDrops > 0 ||
                freshnessDrops > 0;
            if (important) {
                eventCsv << line << '\n';
                ++eventRows;
            }
        }

        eventCsv.flush();

        std::ofstream summary(summaryPath, std::ios::out | std::ios::trunc);
        if (summary) {
            summary
                << "{\n"
                << "  \"sourceCsv\": \""
                << JsonEscape(networkCsvPath.string()) << "\",\n"
                << "  \"eventCsv\": \""
                << JsonEscape(eventCsvPath.string()) << "\",\n"
                << "  \"totalRows\": " << totalRows << ",\n"
                << "  \"eventRows\": " << eventRows << ",\n"
                << "  \"qoe2Rows\": " << qoe2Rows << ",\n"
                << "  \"qoe3Rows\": " << qoe3Rows << ",\n"
                << "  \"recoveryDeadlineRawRows\": "
                << recoveryRawRows << ",\n"
                << "  \"recoveryDeadlineEffectiveRows\": "
                << recoveryEffectiveRows << ",\n"
                << "  \"retransmitStaleEffectiveRows\": "
                << retransmitEffectiveRows << ",\n"
                << "  \"syncRiskRows\": " << syncRiskRows << ",\n"
                << "  \"hardSyncLossRows\": " << hardSyncLossRows << ",\n"
                << "  \"outputDropRows\": " << outputDropRows << ",\n"
                << "  \"freshnessDropRows\": " << freshnessDropRows << ",\n"
                << "  \"arrivalGapJitterRows\": "
                << arrivalGapJitterRows << "\n"
                << "}\n";
        }
    }

    struct LogFileInfo {
        std::filesystem::path path;
        std::filesystem::file_time_type writeTime{};
        uint64_t sizeBytes = 0;
    };

    void RotatePattern(
        const std::filesystem::path& directory,
        const char* patternPrefix,
        const char* extension,
        uint32_t keepCount,
        uint64_t budgetBytes,
        bool preserveNetworkEvents) {
        std::error_code ec;
        if (!std::filesystem::exists(directory, ec)) {
            return;
        }

        std::vector<LogFileInfo> files;
        uint64_t totalBytes = 0;
        for (const auto& entry :
            std::filesystem::directory_iterator(directory, ec)) {
            if (ec || !entry.is_regular_file(ec)) {
                continue;
            }

            const std::filesystem::path path = entry.path();
            const std::string filename = path.filename().string();
            if (filename.rfind(patternPrefix, 0) != 0 ||
                path.extension() != extension) {
                continue;
            }
            if (preserveNetworkEvents &&
                filename.rfind("network_events_", 0) == 0) {
                continue;
            }

            const uint64_t size =
                static_cast<uint64_t>(entry.file_size(ec));
            if (ec) {
                ec.clear();
                continue;
            }

            files.push_back({ path, entry.last_write_time(ec), size });
            if (ec) {
                files.back().writeTime =
                    std::filesystem::file_time_type::min();
                ec.clear();
            }
            totalBytes += size;
        }

        std::sort(
            files.begin(),
            files.end(),
            [](const LogFileInfo& lhs, const LogFileInfo& rhs) {
                return lhs.writeTime > rhs.writeTime;
            });

        for (size_t i = files.size(); i > 0; --i) {
            const size_t index = i - 1;
            const bool overCount = files.size() > keepCount &&
                index >= keepCount;
            const bool overBudget = budgetBytes > 0 &&
                totalBytes > budgetBytes &&
                index > 0;
            if (!overCount && !overBudget) {
                continue;
            }

            std::filesystem::remove(files[index].path, ec);
            if (!ec && totalBytes >= files[index].sizeBytes) {
                totalBytes -= files[index].sizeBytes;
            }
            ec.clear();
            files.erase(files.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    void MaintainNetworkLogDirectory(const std::filesystem::path& directory) {
        if (!LogRotationEnabled()) {
            return;
        }

        const uint32_t keepNetworkCsv =
            EnvUint("RNVP_LOG_KEEP_NETWORK_CSV", 20, 1, 1000);
        const uint32_t keepFrameTrace =
            EnvUint("RNVP_LOG_KEEP_FRAME_TRACE", 10, 1, 1000);
        const uint32_t keepRetransmitTrace =
            EnvUint("RNVP_LOG_KEEP_RETRANSMIT_TRACE", 10, 1, 1000);
        const uint64_t rawBudgetBytes =
            EnvBytesFromMb("RNVP_LOG_RAW_BUDGET_MB", 1024, 16, 102400);

        RotatePattern(
            directory,
            "network_",
            ".csv",
            keepNetworkCsv,
            rawBudgetBytes,
            true);
        RotatePattern(
            directory,
            "frame_recovery_trace_",
            ".csv",
            keepFrameTrace,
            rawBudgetBytes,
            false);
        RotatePattern(
            directory,
            "retransmit_trace_",
            ".csv",
            keepRetransmitTrace,
            rawBudgetBytes,
            false);
    }

    bool StartsWith(const std::string& value, const char* prefix) {
        const std::string prefixText(prefix != nullptr ? prefix : "");
        return value.size() >= prefixText.size() &&
            value.compare(0, prefixText.size(), prefixText) == 0;
    }

    std::pair<std::string, std::string> ClassifyFreshnessDrop(
        const NetworkStatsSnapshot& stats,
        uint64_t freshnessDropDelta) {
        if (freshnessDropDelta == 0) {
            return { "", "" };
        }

        const double observedFrameAgeMs = (std::max)(
            stats.receiveDecodeInputFrameAgeMs,
            stats.receiveLatestDecodedFrameAgeMs);
        const double freshnessAgeMs =
            stats.receiveLastFreshnessDropAgeMs > 0.0
            ? stats.receiveLastFreshnessDropAgeMs
            : observedFrameAgeMs;
        const bool staleAgeEvidence =
            stats.receiveFreshnessDropThresholdMs > 0.0 &&
            freshnessAgeMs >= stats.receiveFreshnessDropThresholdMs;
        const bool staleDropReason =
            StartsWith(stats.receiveDecodeLastDropReason, "stale-");

        const bool activeQueuePressure =
            stats.pacingCurrentQueueDelayMs >= 30.0 ||
            stats.fixedPacingRecentMaxQueueDelayMs >= 60.0;
        if (activeQueuePressure) {
            return {
                "queue-active freshness",
                "pacing-queue-delay"
            };
        }

        const bool latencyPressure = stats.currentLatencyMs >= 80.0;
        if (latencyPressure) {
            return {
                "latency-pressure freshness",
                "latency-over-80ms"
            };
        }

        const bool repairBudgetPressure =
            stats.adaptiveRepairBudgetUtilization >= 3.0 ||
            stats.adaptiveRepairDecisionReason ==
                "fixed-pacing-queue-pressure" ||
            stats.adaptiveRepairDecisionReason ==
                "fixed-pacing-queue-hard-pressure" ||
            stats.adaptiveRepairDecisionReason ==
                "fixed-pacing-receive-queue-hold";
        if (repairBudgetPressure) {
            return {
                "repair-budget-pressure freshness",
                "repair-budget-or-fixed-pacing-reason"
            };
        }

        const bool cadenceLimited =
            (stats.h264InputCadenceFps > 0 &&
                stats.h264InputCadenceFps <= 24) ||
            stats.sendH264VideoBudgetScale <= 0.85 ||
            stats.adaptiveRepairDecisionReason ==
                "fixed-pacing-queue-hold" ||
            stats.adaptiveRepairDecisionReason ==
                "fixed-pacing-queue-release";
        if (cadenceLimited) {
            return {
                "cadence-limited freshness",
                "input-cadence-or-video-budget"
            };
        }

        if (staleAgeEvidence || staleDropReason) {
            return {
                "true-stale-frame freshness",
                staleAgeEvidence ? "frame-age-threshold" : "receiver-stale-drop"
            };
        }

        return { "unclassified freshness", "missing-freshness-context" };
    }

} // namespace

    bool NetworkCsvLogger::Start(const std::string& directory) {
        Stop();

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return false;
        }
        MaintainNetworkLogDirectory(directory);

        const std::filesystem::path path =
            std::filesystem::path(directory) /
            ("network_" + MakeTimestamp() + ".csv");

        file_.open(path, std::ios::out | std::ios::trunc);
        if (!file_) {
            filePath_.clear();
            return false;
        }

        filePath_ = path.string();
        const std::filesystem::path eventPath = EventCsvPathFor(path);
        eventFile_.open(eventPath, std::ios::out | std::ios::trunc);
        if (eventFile_) {
            eventFilePath_ = eventPath.string();
        }
        else {
            eventFilePath_.clear();
            eventFile_.clear();
        }
        headerWritten_ = false;
        eventHeaderWritten_ = false;
        startupWarmupSec_ = StartupWarmupSecFromEnv();
        summaryTotalRows_ = 0;
        summaryEventRows_ = 0;
        summaryQoe2Rows_ = 0;
        summaryQoe3Rows_ = 0;
        summaryRecoveryDeadlineRawRows_ = 0;
        summaryRecoveryDeadlineEffectiveRows_ = 0;
        summaryRetransmitStaleEffectiveRows_ = 0;
        summarySyncRiskRows_ = 0;
        summaryHardSyncLossRows_ = 0;
        summaryOutputDropRows_ = 0;
        summaryFreshnessDropRows_ = 0;
        summaryArrivalGapJitterRows_ = 0;
        previousOutputQueueDroppedFrames_ = 0;
        previousOutputQueueDropEvents_ = 0;
        previousOutputQueueDropBurstEvents_ = 0;
        previousReceiveFreshnessDroppedFrames_ = 0;
        startupOutputQueueDroppedFrames_ = 0;
        startupOutputQueueDropEvents_ = 0;
        startupOutputQueueDropBurstEvents_ = 0;
        steadyOutputQueueDroppedFrames_ = 0;
        steadyOutputQueueDropEvents_ = 0;
        steadyOutputQueueDropBurstEvents_ = 0;
        WriteHeader();
        WriteEventHeader();
        return true;
    }

    void NetworkCsvLogger::Stop() {
        const std::string completedFilePath = filePath_;
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        if (eventFile_.is_open()) {
            eventFile_.flush();
            eventFile_.close();
        }

        WriteRunSummary();

        if (!completedFilePath.empty()) {
            const std::filesystem::path completedPath(completedFilePath);
            if (eventFilePath_.empty()) {
                ExtractNetworkEventArtifacts(completedPath);
            }
            MaintainNetworkLogDirectory(completedPath.parent_path());
        }

        headerWritten_ = false;
        eventHeaderWritten_ = false;
    }

    void NetworkCsvLogger::SetScenarioName(std::string name) {
        if (name.empty()) {
            scenarioName_ = "Auto";
            return;
        }

        scenarioName_ = std::move(name);
    }

    void NetworkCsvLogger::WriteSample(
        const NetworkStatsSnapshot& stats,
        double appTimeSec) {
        if (!file_.is_open()) {
            return;
        }

        if (!headerWritten_) {
            WriteHeader();
        }

        const bool startupWarmupActive = appTimeSec < startupWarmupSec_;
        const uint64_t outputQueueDroppedFramesDelta =
            CounterDelta(
                stats.outputQueueDroppedFrames,
                previousOutputQueueDroppedFrames_);
        const uint64_t outputQueueDropEventsDelta =
            CounterDelta(
                stats.outputQueueDropEvents,
                previousOutputQueueDropEvents_);
        const uint64_t outputQueueDropBurstEventsDelta =
            CounterDelta(
                stats.outputQueueDropBurstEvents,
                previousOutputQueueDropBurstEvents_);
        const uint64_t receiveFreshnessDroppedFramesDelta =
            CounterDelta(
                stats.receiveFreshnessDroppedFrames,
                previousReceiveFreshnessDroppedFrames_);
        const auto freshnessDropClassification =
            ClassifyFreshnessDrop(stats, receiveFreshnessDroppedFramesDelta);
        previousOutputQueueDroppedFrames_ =
            stats.outputQueueDroppedFrames;
        previousOutputQueueDropEvents_ =
            stats.outputQueueDropEvents;
        previousOutputQueueDropBurstEvents_ =
            stats.outputQueueDropBurstEvents;
        previousReceiveFreshnessDroppedFrames_ =
            stats.receiveFreshnessDroppedFrames;

        if (startupWarmupActive) {
            startupOutputQueueDroppedFrames_ +=
                outputQueueDroppedFramesDelta;
            startupOutputQueueDropEvents_ +=
                outputQueueDropEventsDelta;
            startupOutputQueueDropBurstEvents_ +=
                outputQueueDropBurstEventsDelta;
        }
        else {
            steadyOutputQueueDroppedFrames_ +=
                outputQueueDroppedFramesDelta;
            steadyOutputQueueDropEvents_ +=
                outputQueueDropEventsDelta;
            steadyOutputQueueDropBurstEvents_ +=
                outputQueueDropBurstEventsDelta;
        }

        file_ << std::fixed << std::setprecision(3)
            << appTimeSec << ','
            << EscapeCsv(ResolveScenarioName(stats)) << ','
            << EscapeCsv(stats.networkRuntimeModeName) << ','
            << (stats.networkModeSendingEnabled ? 1 : 0) << ','
            << (stats.networkModeReceivingEnabled ? 1 : 0) << ','
            << (stats.networkExperimentActive ? 1 : 0) << ','
            << EscapeCsv(stats.networkExperimentScenarioName) << ','
            << EscapeCsv(stats.networkExperimentAdaptiveMode) << ','
            << stats.networkExperimentRemainingSec << ','
            << stats.networkExperimentStepIndex << ','
            << stats.networkExperimentStepCount << ','
            << stats.latestFrameId << ','
            << stats.receiveFps << ','
            << stats.decodeFps << ','
            << stats.displayFps << ','
            << stats.currentLatencyMs << ','
            << stats.averageLatencyMs << ','
            << stats.currentRttMs << ','
            << stats.averageRttMs << ','
            << stats.currentJitterMs << ','
            << stats.packetLossRate << ','
            << stats.frameDropRate << ','
            << stats.receivedPackets << ','
            << stats.missingPackets << ','
            << stats.sequenceGapPackets << ','
            << stats.sequenceGapRecoveredPackets << ','
            << stats.duplicatePackets << ','
            << stats.duplicateOriginalPackets << ','
            << stats.duplicateRetransmitPackets << ','
            << stats.duplicateLateAfterCompletedPackets << ','
            << stats.duplicateLateAfterCompletedOriginalPackets << ','
            << stats.duplicateLateAfterCompletedRetransmitPackets << ','
            << stats.duplicateLateAfterExpiredPackets << ','
            << stats.duplicateLateAfterRejectedPackets << ','
            << stats.reorderedPackets << ','
            << stats.completedFrames << ','
            << stats.droppedFrames << ','
            << stats.deadlineDroppedFrames << ','
            << stats.outputQueueDroppedFrames << ','
            << stats.outputQueueDropEvents << ','
            << stats.outputQueueDropBurstEvents << ','
            << startupWarmupSec_ << ','
            << (startupWarmupActive ? 1 : 0) << ','
            << outputQueueDroppedFramesDelta << ','
            << outputQueueDropEventsDelta << ','
            << outputQueueDropBurstEventsDelta << ','
            << startupOutputQueueDroppedFrames_ << ','
            << startupOutputQueueDropEvents_ << ','
            << startupOutputQueueDropBurstEvents_ << ','
            << steadyOutputQueueDroppedFrames_ << ','
            << steadyOutputQueueDropEvents_ << ','
            << steadyOutputQueueDropBurstEvents_ << ','
            << steadyOutputQueueDroppedFrames_ << ','
            << steadyOutputQueueDropEvents_ << ','
            << steadyOutputQueueDropBurstEvents_ << ','
            << stats.lastOutputQueueDropFrameCount << ','
            << stats.lastOutputQueueDropQueueSize << ','
            << stats.lastOutputQueueDropOldestAgeMs << ','
            << stats.lastOutputQueueDropNewestAgeMs << ','
            << stats.maxOutputQueueDropOldestAgeMs << ','
            << EscapeCsv(stats.lastOutputQueueDropReason) << ','
            << stats.completedQueuePushes << ','
            << stats.completedQueuePops << ','
            << stats.completedQueueEmptyPolls << ','
            << stats.completedQueueSize << ','
            << stats.maxCompletedQueueSize << ','
            << stats.completedQueueLastPopAgeMs << ','
            << stats.completedQueueMaxPopAgeMs << ','
            << stats.completedQueueLastPushIntervalMs << ','
            << stats.completedQueueMaxPushIntervalMs << ','
            << stats.lastOutputQueueDropPopAgeMs << ','
            << stats.lastOutputQueueDropPushIntervalMs << ','
            << stats.decodedFrames << ','
            << stats.displayedFrames << ','
            << stats.frameRecoveryOutcomeEvents << ','
            << stats.frameRecoveryCompletedFrames << ','
            << stats.frameRecoveryExpiredFrames << ','
            << stats.frameRecoveryRejectedFrames << ','
            << stats.frameRecoveryNackSentFrames << ','
            << stats.frameRecoveryFecRecoveredFrames << ','
            << stats.frameRecoveryLastFrameId << ','
            << stats.frameRecoveryLastStreamId << ','
            << EscapeCsv(stats.frameRecoveryLastEvent) << ','
            << EscapeCsv(stats.frameRecoveryLastOutcome) << ','
            << EscapeCsv(stats.frameRecoveryLastCodec) << ','
            << (stats.frameRecoveryLastKeyFrame ? 1 : 0) << ','
            << (stats.frameRecoveryLastLargeFrame ? 1 : 0) << ','
            << stats.frameRecoveryLastChunkCount << ','
            << stats.frameRecoveryLastReceivedChunks << ','
            << stats.frameRecoveryLastMissingChunks << ','
            << stats.frameRecoveryLastFecParityPackets << ','
            << stats.frameRecoveryLastFecRecoveredChunks << ','
            << stats.frameRecoveryLastNackCount << ','
            << stats.frameRecoveryLastPostNackReceivedChunks << ','
            << stats.frameRecoveryLastNackRequestedChunks << ','
            << stats.frameRecoveryLastRetransmitReceivedChunks << ','
            << stats.frameRecoveryLastRetransmitDuplicatePackets << ','
            << stats.frameRecoveryLastPacketSequence << ','
            << stats.frameRecoveryLastPacketChunkIndex << ','
            << stats.frameRecoveryLastRetransmitSequence << ','
            << stats.frameRecoveryLastRetransmitChunkIndex << ','
            << stats.frameRecoveryLastEventPacketSequence << ','
            << stats.frameRecoveryLastEventPacketChunkIndex << ','
            << (stats.frameRecoveryLastEventWasRetransmit ? 1 : 0) << ','
            << stats.frameRecoveryLastAgeMs << ','
            << stats.retransmitUsefulChunks << ','
            << stats.retransmitDuplicatePackets << ','
            << stats.retransmitLateAfterCompletedPackets << ','
            << stats.retransmitLateAfterCompletedLargePackets << ','
            << stats.retransmitLateAfterCompletedSentBeforeCompletePackets << ','
            << stats.retransmitLateAfterCompletedSentAfterCompletePackets << ','
            << stats.retransmitLateAfterCompletedAvgSendToCompleteMs << ','
            << stats.retransmitLateAfterCompletedAvgDelayMs << ','
            << stats.retransmitLateAfterCompletedMaxDelayMs << ','
            << stats.retransmitLateAfterExpiredPackets << ','
            << stats.retransmitLateAfterRejectedPackets << ','
            << stats.retransmitClassifiedPackets << ','
            << stats.retransmitNotArrivedPackets << ','
            << stats.retransmitAccountedPackets << ','
            << stats.retransmitUnclassifiedPackets << ','
            << stats.retransmitCompletedFrames << ','
            << stats.retransmitExpiredFrames << ','
            << stats.retransmitUsefulnessRatio << ','
            << stats.retransmitDuplicateRatio << ','
            << stats.retransmitFinalClassificationRatio << ','
            << stats.retransmitFinalAccountingRatio << ','
            << stats.retransmitExpiredAfterUsefulRatio << ','
            << stats.dynamicNackDeadlineMs << ','
            << stats.dynamicNackUsefulnessRatio << ','
            << stats.dynamicNackDuplicateRatio << ','
            << stats.dynamicNackExpiredAfterRetransmitRatio << ','
            << EscapeCsv(stats.dynamicNackDecisionReason) << ','
            << stats.nackSuppressedFrames << ','
            << stats.nackSuppressedMissingChunks << ','
            << stats.nackPreflightSuppressedFrames << ','
            << stats.nackPreflightSuppressedChunks << ','
            << stats.nackFecGraceSuppressedFrames << ','
            << stats.nackFecGraceSuppressedChunks << ','
            << stats.nackPredictedUsefulFrames << ','
            << stats.nackPredictedUsefulChunks << ','
            << stats.nackDeferredForLikelyArrivalFrames << ','
            << stats.nackDeferredForLikelyArrivalChunks << ','
            << stats.nackSkippedTooLateFrames << ','
            << stats.nackSkippedTooLateChunks << ','
            << stats.nackRequestedChunkBudget << ','
            << EscapeCsv(stats.nackLastShapingReason) << ','
            << EscapeCsv(stats.nackLastSuppressionReason) << ','
            << stats.ackCount << ','
            << stats.lastAckMissingRate << ','
            << stats.ackRetransmittedFrames << ','
            << stats.ackRetransmittedChunks << ','
            << stats.repairCanceledByCompleteAckPackets << ','
            << stats.repairSkippedByTtlPackets << ','
            << stats.repairQueuedButCanceledPackets << ','
            << stats.repairSentAfterCompleteAckPackets << ','
            << stats.repairSentAfterCompleteAckLargePackets << ','
            << stats.repairSuppressedByFecLikelyFrames << ','
            << stats.repairSuppressedByFecLikelyPackets << ','
            << stats.repairSuppressedByFecLikelyLargeFrames << ','
            << stats.repairSuppressedByFecLikelyLargePackets << ','
            << stats.repairBudgetSuppressedFrames << ','
            << stats.repairBudgetSuppressedPackets << ','
            << stats.repairBudgetSuppressedLargeFrames << ','
            << stats.repairBudgetSuppressedLargePackets << ','
            << stats.repairRaceGuardSuppressedFrames << ','
            << stats.repairRaceGuardSuppressedPackets << ','
            << stats.repairRaceGuardSuppressedLargePackets << ','
            << EscapeCsv(stats.repairBudgetProfile) << ','
            << stats.repairBudgetProfileSwitches << ','
            << stats.repairBudgetSmoothedMissingRate << ','
            << stats.repairBudgetSmoothedPacingQueueDelayMs << ','
            << stats.repairBudgetSmoothedDeliveryMs << ','
            << stats.repairFecLikelySuppressedCompletedFrames << ','
            << stats.repairFecLikelySuppressedCompletedPackets << ','
            << stats.repairFecLikelySuppressedExpiredFrames << ','
            << stats.repairFecLikelySuppressedExpiredPackets << ','
            << stats.repairFecLikelySuppressedPendingFrames << ','
            << stats.repairFecLikelySuppressedPendingPackets << ','
            << stats.repairFecLikelySuppressionRescueFrames << ','
            << stats.repairFecLikelySuppressionRescuePackets << ','
            << stats.h264KeyTinyMissingCriticalFrames << ','
            << stats.h264KeyTinyMissingCriticalPackets << ','
            << stats.h264KeyTinyMissingCriticalSentPackets << ','
            << stats.h264KeyTinyMissingCriticalSkippedPackets << ','
            << stats.h264KeyTinyMissingCriticalFeasibilitySuppressedFrames << ','
            << stats.h264KeyTinyMissingCriticalFeasibilitySuppressedPackets << ','
            << stats.h264KeyTinyMissingCriticalFeasibilityBypassedFrames << ','
            << stats.h264KeyTinyMissingCriticalFeasibilityBypassedPackets << ','
            << stats.h264KeyTinyMissingCriticalLastPredictedDeliveryMs << ','
            << stats.h264KeyTinyMissingCriticalLastRemainingSlackMs << ','
            << stats.h264KeyTinyMissingCriticalLastFrameId << ','
            << stats.h264KeyTinyMissingCriticalLastAckMissingChunks << ','
            << stats.h264KeyTinyMissingCriticalLastRequestedChunks << ','
            << EscapeCsv(stats.h264KeyTinyMissingCriticalLastEvent) << ','
            << stats.h264KeyTinyEarlyNackFrames << ','
            << stats.h264KeyTinyEarlyNackMissingChunks << ','
            << stats.h264KeySmallMissingAckFrames << ','
            << stats.h264KeySmallMissingAckMissingChunks << ','
            << stats.h264KeySmallMissingAckHistoryMissingFrames << ','
            << stats.h264KeySmallMissingAckStaleFrameLagFrames << ','
            << stats.h264KeySmallMissingAckStaleAgeFrames << ','
            << stats.h264KeySmallMissingAckRetransmitBudgetExhaustedFrames << ','
            << stats.h264KeySmallMissingAckDynamicBudgetSuppressedFrames << ','
            << stats.h264KeySmallMissingAckDynamicBudgetSuppressedPackets << ','
            << stats.h264KeySmallMissingAckSelectedRepairFrames << ','
            << stats.h264KeySmallMissingAckSelectedRepairPackets << ','
            << stats.h264KeySelectedRepair1To2Frames << ','
            << stats.h264KeySelectedRepair1To2Packets << ','
            << stats.h264KeySelectedRepair3To4Frames << ','
            << stats.h264KeySelectedRepair3To4Packets << ','
            << stats.h264KeySmallMissingAckLastFrameId << ','
            << stats.h264KeySmallMissingAckLastMissingChunks << ','
            << EscapeCsv(stats.h264KeySmallMissingAckLastGate) << ','
            << stats.h264KeyRepair1To2CompletedFrames << ','
            << stats.h264KeyRepair1To2ExpiredFrames << ','
            << stats.h264KeyRepair1To2ArrivedPackets << ','
            << stats.h264KeyRepair1To2DuplicatePackets << ','
            << stats.h264KeyRepair1To2LateCompletedPackets << ','
            << stats.h264KeyRepair1To2LateExpiredPackets << ','
            << stats.h264KeyRepair1To2LateRejectedPackets << ','
            << stats.h264KeyTinyEmergencyCompletedFrames << ','
            << stats.h264KeyTinyEmergencyArrivedBeforeRetirePackets << ','
            << stats.h264KeyTinyEmergencyDuplicateBeforeRetirePackets << ','
            << stats.h264KeyTinyEmergencyArrivedAfterCompletePackets << ','
            << stats.h264KeyTinyEmergencyArrivedAfterExpirePackets << ','
            << stats.h264KeyTinyEmergencyArrivedAfterRejectedPackets << ','
            << stats.h264KeyTinyEmergencyNotArrivedPackets << ','
            << stats.h264KeyTinyEmergencySendToArrivalAvgMs << ','
            << stats.h264KeyTinyEmergencySendToArrivalMaxMs << ','
            << stats.h264KeyTinyLastChanceNackFrames << ','
            << stats.h264KeyTinyLastChanceMissing1Frames << ','
            << stats.h264KeyTinyLastChanceCompletedFrames << ','
            << stats.h264KeyTinyLastChanceExpiredFrames << ','
            << stats.h264KeyTinyLastChanceArrivedBeforeRetirePackets << ','
            << stats.h264KeyTinyLastChanceDuplicateBeforeRetirePackets << ','
            << stats.h264KeyTinyLastChanceLateCompletedPackets << ','
            << stats.h264KeyTinyLastChanceLateExpiredPackets << ','
            << stats.h264KeyTinyLastChanceLateRejectedPackets << ','
            << stats.h264KeyTinyLastChanceSlackAvgMs << ','
            << stats.h264KeyTinyLastChanceSlackMinMs << ','
            << stats.h264KeyTinyLastChanceSlackMaxMs << ','
            << stats.h264KeyRepair3To4CompletedFrames << ','
            << stats.h264KeyRepair3To4ExpiredFrames << ','
            << stats.h264KeyRepair3To4ArrivedPackets << ','
            << stats.h264KeyRepair3To4DuplicatePackets << ','
            << stats.h264KeyRepair3To4LateCompletedPackets << ','
            << stats.h264KeyRepair3To4LateExpiredPackets << ','
            << stats.h264KeyRepair3To4LateRejectedPackets << ','
            << stats.lateRepairSavedPackets << ','
            << stats.ackStaleDroppedFrames << ','
            << stats.ackKeyFrameRequests << ','
            << stats.h264KeyFrameRequestAckHistoryMissing << ','
            << stats.h264KeyFrameRequestAckStaleFrameLag << ','
            << stats.h264KeyFrameRequestAckStaleAge << ','
            << stats.h264KeyFrameRequestAckRetransmitBudgetExhausted << ','
            << stats.h264KeyFrameRequestAckHighMissingRate << ','
            << stats.h264KeyFrameRequestAckCooldownSuppressed << ','
            << stats.h264KeyFrameRequestAckAlreadyPending << ','
            << stats.h264KeyFrameRequestAckCooldownNoise << ','
            << stats.h264KeyFrameRequestAckCooldownSyncRisk << ','
            << stats.h264KeyFrameRequestAckStaleAgeCooldownNoise << ','
            << stats.h264KeyFrameRequestAckStaleAgeCooldownSyncRisk << ','
            << EscapeCsv(stats.h264KeyFrameRequestAckLastReason) << ','
            << stats.h264KeyFrameRequestReceiverRequests << ','
            << stats.h264KeyFrameRequestReceiverCooldownSuppressed << ','
            << stats.h264KeyFrameRequestReceiverCooldownNoise << ','
            << stats.h264KeyFrameRequestReceiverCooldownSyncRisk << ','
            << stats.h264KeyFrameRequestReceiverTrueSyncLoss << ','
            << stats.h264KeyFrameRequestReceiverMissingAck << ','
            << stats.h264KeyFrameRequestReceiverDeadlineExpired << ','
            << stats.h264KeyFrameRequestReceiverDeadlineNackMissing << ','
            << stats.h264KeyFrameRequestReceiverDeadlineNackMissingCooldownNoise << ','
            << stats.h264KeyFrameRequestReceiverDeadlineNackMissingCooldownSyncRisk << ','
            << stats.h264KeyFrameRequestReceiverPayloadHeaderFailure << ','
            << stats.h264KeyFrameRequestReceiverAuInvalid << ','
            << stats.h264KeyFrameRequestReceiverInitWaitIdr << ','
            << stats.h264KeyFrameRequestReceiverWaitingForIdr << ','
            << stats.h264KeyFrameRequestReceiverDecodeFailure << ','
            << stats.h264KeyFrameRequestReceiverStaleAfterDecode << ','
            << EscapeCsv(stats.h264KeyFrameRequestReceiverLastReason) << ','
            << (stats.ackKeyFramePending ? 1 : 0) << ','
            << (stats.pacingEnabled ? 1 : 0) << ','
            << stats.pacingTargetBitrateBps << ','
            << stats.pacingRepairTargetBitrateBps << ','
            << stats.pacingQueuedPackets << ','
            << stats.pacingHighPriorityQueuedPackets << ','
            << stats.pacingNormalQueuedPackets << ','
            << stats.pacingEnqueuedPackets << ','
            << stats.pacingSentPackets << ','
            << stats.pacingSentBytes << ','
            << stats.pacingRepairSentPackets << ','
            << stats.pacingRepairSentBytes << ','
            << stats.pacingRepairBorrowedPackets << ','
            << stats.pacingRepairBorrowedBytes << ','
            << stats.pacingEmergencySentPackets << ','
            << stats.pacingEmergencySentBytes << ','
            << stats.pacingEmergencyBorrowedPackets << ','
            << stats.pacingEmergencyBorrowedBytes << ','
            << stats.h264KeyTinyEmergencyLastQueueAgeMs << ','
            << stats.h264KeyTinyEmergencyMaxQueueAgeMs << ','
            << stats.h264KeyTinyEmergencyAvgQueueAgeMs << ','
            << stats.pacingDroppedPackets << ','
            << stats.pacingDeadlineDroppedPackets << ','
            << stats.pacingHighPriorityDeadlineDroppedPackets << ','
            << stats.pacingNormalDeadlineDroppedPackets << ','
            << stats.pacingOverflowDroppedPackets << ','
            << stats.pacingCurrentQueueDelayMs << ','
            << stats.pacingMaxQueueDelayMs << ','
            << stats.pacingVideoCreditBytes << ','
            << stats.pacingRepairCreditBytes << ','
            << stats.transportFeedbackPackets << ','
            << stats.transportFeedbackPacketStatuses << ','
            << stats.transportFeedbackReceivedPackets << ','
            << stats.transportFeedbackMissingPackets << ','
            << stats.transportFeedbackSequenceGapPackets << ','
            << stats.transportFeedbackSequenceGapRecoveredPackets << ','
            << stats.transportFeedbackLossRate << ','
            << stats.transportFeedbackArrivalJitterMs << ','
            << stats.transportFeedbackQueueDelayTrendMs << ','
            << stats.transportFeedbackLastSequence << ','
            << stats.estimatedBandwidthBps << ','
            << stats.deliveryRateBps << ','
            << stats.bandwidthQueueDelayMs << ','
            << stats.bandwidthRttTrendMs << ','
            << stats.bandwidthLossTrend << ','
            << stats.bandwidthJitterTrendMs << ','
            << stats.bandwidthFeedbackSamples << ','
            << stats.deadlineNackSentFrames << ','
            << stats.deadlineNackRecoveredFrames << ','
            << stats.deadlineNackMissingChunks << ','
            << stats.deadlineNackSentH264KeyFrames << ','
            << stats.deadlineNackSentH264LargeFrames << ','
            << stats.deadlineNackSentH264DeltaFrames << ','
            << stats.deadlineNackExpiredDroppedFrames << ','
            << stats.deadlineNackExpiredAfterNackFrames << ','
            << stats.deadlineNackExpiredMissingChunks << ','
            << stats.deadlineNackExpiredH264KeyFrames << ','
            << stats.deadlineNackExpiredH264KeyMissingChunks << ','
            << stats.deadlineNackLastExpiredH264KeyMissingChunks << ','
            << stats.deadlineNackExpiredH264KeyMissingChunks1 << ','
            << stats.deadlineNackExpiredH264KeyMissingChunks2To4 << ','
            << stats.deadlineNackExpiredH264KeyMissingChunks5To8 << ','
            << stats.deadlineNackExpiredH264KeyMissingChunks9To16 << ','
            << stats.deadlineNackExpiredH264KeyMissingChunks17Plus << ','
            << stats.h264KeySmallMissingDeadlineRescueFrames << ','
            << stats.h264KeySmallMissingDeadlineRescueMissingChunks << ','
            << stats.h264KeySmallMissingDeadlineRescueCompletedFrames << ','
            << stats.h264KeySmallMissingDeadlineRescueRejectedFrames << ','
            << stats.h264KeySmallMissingDeadlineRescueExpiredFrames << ','
            << stats.h264KeySmallMissingDeadlineRescueCompletedMissingChunks << ','
            << stats.h264KeySmallMissingDeadlineRescueExpiredMissingChunks << ','
            << stats.deadlineNackExpiredH264LargeFrames << ','
            << stats.deadlineNackExpiredH264DeltaFrames << ','
            << (stats.fecEnabled ? 1 : 0) << ','
            << (stats.adaptiveFecEnabled ? 1 : 0) << ','
            << stats.fecGroupChunkCount << ','
            << stats.fecParityPackets << ','
            << stats.fecRecoveredFrames << ','
            << stats.fecRecoveredChunks << ','
            << stats.h264ReassemblerAuRejectedFrames << ','
            << stats.h264ReassemblerHeaderFailures << ','
            << stats.h264ReassemblerPayloadSizeMismatches << ','
            << stats.h264ReassemblerFrameIdMismatches << ','
            << stats.h264ReassemblerCrcMismatches << ','
            << EscapeCsv(stats.h264ReassemblerLastRejectReason) << ','
            << EscapeCsv(stats.adaptiveFecDecisionReason) << ','
            << EscapeCsv(stats.adaptiveFecHoldReason) << ','
            << (stats.adaptiveFecG8ToG4Recovery ? 1 : 0) << ','
            << (stats.adaptiveFecQualityHoldActive ? 1 : 0) << ','
            << (stats.adaptiveFecQualityHoldCanceled ? 1 : 0) << ','
            << (stats.adaptiveFecEmergencyG2Active ? 1 : 0) << ','
            << EscapeCsv(stats.adaptiveFecEarlyOffReason) << ','
            << (stats.adaptiveEnabled ? 1 : 0) << ','
            << EscapeCsv(stats.adaptiveControlMode) << ','
            << EscapeCsv(stats.adaptiveCongestionControlMode) << ','
            << stats.adaptiveTargetJpegQuality << ','
            << stats.adaptiveTargetFps << ','
            << stats.adaptiveTargetBitrateKbps << ','
            << stats.adaptiveBandwidthCeilingKbps << ','
            << stats.adaptiveTargetWidth << ','
            << stats.adaptiveTargetHeight << ','
            << EscapeCsv(stats.sendActualCodec) << ','
            << stats.sendActualEncodeWidth << ','
            << stats.sendActualEncodeHeight << ','
            << stats.sendActualRawFrameBytes << ','
            << stats.sendActualEncodedFrameBytes << ','
            << stats.adaptiveRawFrameBytes << ','
            << stats.adaptiveEncodedFrameBytes << ','
            << stats.adaptiveCompressionRatio << ','
            << stats.captureFps << ','
            << EscapeCsv(stats.cameraCaptureSubtype) << ','
            << stats.cameraCaptureWidth << ','
            << stats.cameraCaptureHeight << ','
            << stats.cameraCaptureFormatFps << ','
            << stats.encodeMs << ','
            << stats.sendResizeMs << ','
            << stats.sendNv12PrepareMs << ','
            << stats.sendH264EncodeMs << ','
            << stats.sendH264EncoderRequestedBitrateKbps << ','
            << stats.sendH264EncoderTargetBitrateKbps << ','
            << stats.sendH264EncoderAppliedBitrateKbps << ','
            << stats.h264DynamicBitrateUpdateRequests << ','
            << stats.h264DynamicBitrateUpdateSuccesses << ','
            << stats.h264DynamicBitrateUpdateFailures << ','
            << stats.h264EncoderReinitializations << ','
            << stats.sendPacingTargetBitrateKbps << ','
            << stats.sendH264VideoBudgetScale << ','
            << (stats.pacingBurstGuardActive ? 1 : 0) << ','
            << stats.fixedPacingRecentMaxQueueDelayMs << ','
            << stats.fixedPacingQueueReleaseStableSec << ','
            << (stats.fixedPacingQueueRecovered ? 1 : 0) << ','
            << (stats.fixedPacingQueueReleaseEligible ? 1 : 0) << ','
            << stats.adaptiveRepairBudgetUtilization << ','
            << stats.adaptiveRepairBorrowedRatio << ','
            << stats.adaptiveRepairSentBytesDelta << ','
            << stats.adaptiveRepairBorrowedBytesDelta << ','
            << (stats.adaptiveRepairBudgetGuardActive ? 1 : 0) << ','
            << (stats.adaptiveRepairVideoBudgetPressure ? 1 : 0) << ','
            << stats.adaptiveRetransmitUsefulRatio << ','
            << stats.adaptiveLateRepairWasteRatio << ','
            << stats.adaptiveRetransmitNotArrivedRatio << ','
            << (stats.adaptiveRetransmitAccountingComplete ? 1 : 0) << ','
            << (stats.adaptiveLateRepairWastePressure ? 1 : 0) << ','
            << (stats.adaptiveRetransmitNotArrivedPressure ? 1 : 0) << ','
            << EscapeCsv(stats.adaptiveRepairDecisionReason) << ','
            << stats.h264AuChunkCount << ','
            << (stats.h264AuIsIdr ? 1 : 0) << ','
            << (stats.h264AuIsDecoderSync ? 1 : 0) << ','
            << stats.h264RequestedKeyFrameConsumedFrames << ','
            << stats.h264EncoderKeyFrameRequests << ','
            << stats.h264RecoveryKeyFrameRequests << ','
            << stats.h264AwaitingSyncKeyFrameRequests << ','
            << stats.h264PeriodicIdrRequests << ','
            << stats.h264IdrFrames << ','
            << stats.h264DecoderSyncFrames << ','
            << stats.h264ConsecutiveIdrFrames << ','
            << stats.h264MaxConsecutiveIdrFrames << ','
            << EscapeCsv(stats.h264AuProtectionLevel) << ','
            << (stats.h264AuDroppedBeforeSend ? 1 : 0) << ','
            << EscapeCsv(stats.h264AuDropReason) << ','
            << stats.h264AuDroppedBytes << ','
            << stats.h264AuDroppedChunks << ','
            << stats.h264AuPacingQueueDelayMs << ','
            << stats.h264AuEstimatedSendMs << ','
            << (stats.h264InputGatedByPacing ? 1 : 0) << ','
            << EscapeCsv(stats.h264InputGateReason) << ','
            << stats.h264InputGateQueueDelayMs << ','
            << stats.h264InputGateVideoCreditBytes << ','
            << stats.h264InputGateFrameBudgetBytes << ','
            << stats.h264InputGateDurationMs << ','
            << stats.h264InputGateConsecutiveFrames << ','
            << stats.h264InputGateSkippedInputFrames << ','
            << (stats.h264InputGateForcedOpen ? 1 : 0) << ','
            << EscapeCsv(stats.h264InputGateReleaseReason) << ','
            << stats.fecProtectedH264KeyFrames << ','
            << stats.fecProtectedH264LargeFrames << ','
            << stats.h264EncoderDelayFrames << ','
            << stats.h264EncoderDelayMs << ','
            << stats.h264EncoderPendingFrames << ','
            << stats.h264EncodedInputFrameId << ','
            << stats.h264EncoderCallMs << ','
            << stats.h264EncoderSampleCreateMs << ','
            << stats.h264EncoderProcessInputMs << ','
            << stats.h264EncoderPreInputPollMs << ','
            << stats.h264EncoderPostInputWaitMs << ','
            << stats.h264EncoderProcessOutputMs << ','
            << stats.h264EncoderOutputCopyMs << ','
            << stats.h264EncoderProcessOutputAttempts << ','
            << stats.h264EncoderAsyncEventCount << ','
            << (stats.h264EncoderHardware ? 1 : 0) << ','
            << (stats.h264EncoderAsync ? 1 : 0) << ','
            << (stats.h264EncoderNeedInputSignaled ? 1 : 0) << ','
            << (stats.h264EncoderOutputProduced ? 1 : 0) << ','
            << (stats.h264EncoderOutputProducedBeforeInput ? 1 : 0) << ','
            << (stats.h264SubmittedNewInput ? 1 : 0) << ','
            << (stats.h264AsyncSubmittedWithoutOutput ? 1 : 0) << ','
            << (stats.h264AsyncPendingNoOutput ? 1 : 0) << ','
            << (stats.h264AsyncCadenceHoldActive ? 1 : 0) << ','
            << stats.h264AsyncPendingNoOutputStreak << ','
            << stats.h264AsyncCadenceScale << ','
            << stats.h264AsyncCadenceHoldRemainingMs << ','
            << stats.h264AsyncOutputPollBackoffMs << ','
            << stats.h264InputCadenceFps << ','
            << (stats.h264NeedInputSubmitWake ? 1 : 0) << ','
            << stats.h264NeedInputSubmitLeadMs << ','
            << stats.encodedCameraFrameId << ','
            << stats.encodedCameraSourceTimestamp100ns << ','
            << stats.encodedCameraCaptureCompletedTimeUs << ','
            << stats.encodedCameraFrameAgeMs << ','
            << stats.encodedCameraReadSampleMs << ','
            << stats.encodedCameraReadSampleEndToCaptureMs << ','
            << stats.encodedCameraCaptureToPublishMs << ','
            << stats.encodedCameraPublishToAcquireMs << ','
            << stats.encodedCameraAcquireToEncoderInputMs << ','
            << stats.sendJpegEncodeMs << ','
            << stats.sendPacketizeMs << ','
            << stats.sendFrameIntervalMs << ','
            << (stats.cameraFrameReady ? 1 : 0) << ','
            << stats.cameraFrameId << ','
            << stats.cameraSourceTimestamp100ns << ','
            << stats.cameraReadSampleStartTimeUs << ','
            << stats.cameraReadSampleEndTimeUs << ','
            << stats.cameraCaptureCompletedTimeUs << ','
            << stats.cameraFramePublishedTimeUs << ','
            << stats.cameraSenderAcquireTimeUs << ','
            << stats.cameraReadSampleMs << ','
            << stats.cameraReadSampleEndToCaptureMs << ','
            << stats.cameraCaptureToPublishMs << ','
            << stats.cameraPublishToAcquireMs << ','
            << stats.cameraAcquireToSendMs << ','
            << stats.cameraFrameAgeMs << ','
            << (stats.cameraFrameCacheUsed ? 1 : 0) << ','
            << stats.receiveJpegDecodeMs << ','
            << stats.receiveDecodeWorkerFps << ','
            << stats.receiveDecodeWorkerFrames << ','
            << stats.receiveDecodePopSuccesses << ','
            << stats.receiveDecodePopEmptyPolls << ','
            << stats.receiveDecodeLoopLastPopGapMs << ','
            << stats.receiveDecodeLoopMaxPopGapMs << ','
            << stats.receiveStartupDecodeLoopMaxPopGapMs << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapMs << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapAtMs << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapFrameId << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapStreamId << ','
            << EscapeCsv(stats.receiveSteadyDecodeLoopMaxPopGapCodec) << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapInputFrameAgeMs << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapDecodedQueueSize << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapCompletedQueueSize << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapCompletedQueuePopAgeMs << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapCompletedQueuePushIntervalMs << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapArrivalRatio << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapReceiverJitterMs << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapReceiverLatencyMs << ','
            << EscapeCsv(stats.receiveSteadyDecodeLoopMaxPopGapClass) << ','
            << stats.receiveSteadyDecodeLoopMaxPopGapAgeMs << ','
            << (stats.receiveH264StartupActive ? 1 : 0) << ','
            << (stats.receiveH264StartupDecoderSynced ? 1 : 0) << ','
            << (stats.receiveH264StartupFirstDecoded ? 1 : 0) << ','
            << (stats.receiveH264StartupFirstDisplayed ? 1 : 0) << ','
            << (stats.receiveH264StartupReady ? 1 : 0) << ','
            << stats.receiveH264StartupElapsedMs << ','
            << stats.receiveH264StartupDecoderSyncMs << ','
            << stats.receiveH264StartupFirstDecodedMs << ','
            << stats.receiveH264StartupFirstDisplayedMs << ','
            << stats.receiveH264StartupReadyMs << ','
            << stats.receiveH264StartupQueueFlushFrames << ','
            << stats.receiveDecodeOverwrittenFrames << ','
            << stats.receiveDecodeQueueDroppedFrames << ','
            << stats.receiveDecodeRenderOverwriteFrames << ','
            << stats.receiveDecodeFailures << ','
            << stats.receiveFreshnessDroppedFrames << ','
            << receiveFreshnessDroppedFramesDelta << ','
            << EscapeCsv(freshnessDropClassification.first) << ','
            << EscapeCsv(freshnessDropClassification.second) << ','
            << stats.receiveLastFreshnessDropAgeMs << ','
            << stats.receiveMaxFreshnessDropAgeMs << ','
            << stats.receiveLastFreshnessDropFrameId << ','
            << stats.receiveLastFreshnessDropStreamId << ','
            << EscapeCsv(stats.receiveLastFreshnessDropCodec) << ','
            << stats.receiveH264AuInvalidFrames << ','
            << stats.receiveH264AuCrcMismatches << ','
            << stats.receiveH264AuPayloadSizeMismatches << ','
            << stats.receiveH264AuNalCountMismatches << ','
            << stats.receiveH264AuNoAnnexBNals << ','
            << stats.receiveH264AuIdrFlagMismatches << ','
            << stats.receiveH264AuSpsPpsFlagMismatches << ','
            << stats.receiveH264AuSyncWithoutIdr << ','
            << stats.receiveH264AuIdrWithoutSpsPps << ','
            << stats.receiveH264AuForbiddenZeroBit << ','
            << EscapeCsv(stats.receiveH264AuLastInvalidReason) << ','
            << stats.receiveDecodeInputFrameAgeMs << ','
            << stats.receiveDecodeInputCameraFrameAgeMs << ','
            << stats.receiveDecodeInputEncoderOutputAgeMs << ','
            << stats.receiveLatestDecodedFrameAgeMs << ','
            << stats.receiveLatestDecodedCameraFrameAgeMs << ','
            << stats.receiveLatestDecodedEncoderOutputAgeMs << ','
            << stats.receiveFreshnessDropThresholdMs << ','
            << EscapeCsv(stats.receiveDecodeLastDropReason) << ','
            << stats.receiveUploadBufferWaitMs << ','
            << stats.receiveDisplayFrameId << ','
            << stats.receiveDisplayCameraFrameAgeMs << ','
            << stats.receiveDisplayEncoderOutputAgeMs << ','
            << stats.receiveDisplayDecodedFrameAgeMs << ','
            << stats.textureUploadMs << ','
            << stats.presentGpuWaitMs << ','
            << stats.renderFramePacingWaitMs << ','
            << stats.waitableSwapChainWaitMs << ','
            << stats.presentMs << ','
            << stats.presentSyncInterval << ','
            << (stats.lowLatencyPresentMode ? 1 : 0) << ','
            << (stats.waitableSwapChainPacingEnabled ? 1 : 0) << ','
            << (stats.waitableSwapChainAvailable ? 1 : 0) << ','
            << stats.swapChainBufferCount << ','
            << stats.adaptiveLastPacketLossRate << ','
            << stats.adaptiveLastReceiveFps << ','
            << stats.adaptiveLastDecodeFps << ','
            << stats.adaptiveLastJitterMs << ','
            << stats.adaptiveLastDisplayFps << ','
            << stats.adaptiveLastQoeScore << ','
            << EscapeCsv(stats.adaptiveDegradationCause) << ','
            << stats.adaptiveRecoveryDeadlineRawDelta << ','
            << stats.adaptiveRecoveryDeadlineEffectiveDelta << ','
            << stats.adaptiveRetransmitStaleRawDelta << ','
            << stats.adaptiveRetransmitStaleEffectiveDelta << ','
            << stats.adaptiveRecoveryDeadlineNoiseDelta << ','
            << stats.adaptiveRecoveryDeadlineSyncRiskDelta << ','
            << stats.adaptiveRecoveryDeadlineHardSyncLossDelta << ','
            << (stats.adaptiveRecoveryDeadlineSyncEvidenceActive ? 1 : 0) << ','
            << stats.adaptiveRecoveryDeadlineSyncEvidenceAgeMs << ','
            << EscapeCsv(stats.adaptiveRecoveryDeadlineSyncEvidenceSource) << ','
            << (stats.adaptiveArrivalGapJitterSpikeActive ? 1 : 0) << ','
            << (stats.adaptiveFecRecoveryWorking ? 1 : 0) << ','
            << (stats.adaptiveFecGuardActive ? 1 : 0) << ','
            << stats.adaptiveFecRecoveryEfficiency << ','
            << stats.adaptiveFecParityPacketDelta << ','
            << stats.adaptiveFecRecoveredFrameDelta << ','
            << stats.adaptiveFecRecoveredChunkDelta << ','
            << (stats.networkCondition.enabled ? 1 : 0) << ','
            << stats.networkCondition.lossRate << ','
            << stats.networkCondition.duplicateRate << ','
            << stats.networkCondition.reorderRate << ','
            << stats.networkCondition.minDelayMs << ','
            << stats.networkCondition.maxDelayMs << ','
            << stats.networkCondition.burstLossLength << ','
            << stats.networkSimulation.submittedPackets << ','
            << stats.networkSimulation.sentPackets << ','
            << stats.networkSimulation.droppedPackets << ','
            << stats.networkSimulation.duplicatedPackets << ','
            << stats.networkSimulation.reorderedPackets
            << '\n';

        WriteEventSample(
            stats,
            appTimeSec,
            startupWarmupActive,
            outputQueueDroppedFramesDelta,
            outputQueueDropEventsDelta,
            outputQueueDropBurstEventsDelta,
            receiveFreshnessDroppedFramesDelta,
            freshnessDropClassification.first,
            freshnessDropClassification.second);

        file_.flush();
    }

    void NetworkCsvLogger::WriteEventHeader() {
        if (!eventFile_.is_open() || eventHeaderWritten_) {
            return;
        }

        eventFile_
            << "timeSec,"
            << "scenarioName,"
            << "networkRuntimeMode,"
            << "networkExperimentActive,"
            << "networkExperimentScenarioName,"
            << "networkExperimentAdaptiveMode,"
            << "frameId,"
            << "receiveFps,"
            << "decodeFps,"
            << "displayFps,"
            << "currentLatencyMs,"
            << "currentRttMs,"
            << "currentJitterMs,"
            << "adaptiveQoeScore,"
            << "adaptiveDegradationCause,"
            << "adaptiveRecoveryDeadlineRawDelta,"
            << "adaptiveRecoveryDeadlineEffectiveDelta,"
            << "adaptiveRetransmitStaleRawDelta,"
            << "adaptiveRetransmitStaleEffectiveDelta,"
            << "adaptiveRecoveryDeadlineNoiseDelta,"
            << "adaptiveRecoveryDeadlineSyncRiskDelta,"
            << "adaptiveRecoveryDeadlineHardSyncLossDelta,"
            << "adaptiveRecoveryDeadlineSyncEvidenceActive,"
            << "adaptiveRecoveryDeadlineSyncEvidenceAgeMs,"
            << "adaptiveRecoveryDeadlineSyncEvidenceSource,"
            << "adaptiveArrivalGapJitterSpikeActive,"
            << "outputQueueDroppedFramesDelta,"
            << "outputQueueDropEventsDelta,"
            << "outputQueueDropBurstEventsDelta,"
            << "receiveFreshnessDroppedFramesDelta,"
            << "receiveFreshnessDropClass,"
            << "receiveFreshnessDropEvidence,"
            << "lastOutputQueueDropReason,"
            << "receiveDecodeLastDropReason,"
            << "deadlineDroppedFrames,"
            << "outputQueueDroppedFrames,"
            << "receiveFreshnessDroppedFrames,"
            << "deadlineNackExpiredDroppedFrames,"
            << "deadlineNackExpiredH264KeyFrames,"
            << "ackStaleDroppedFrames,"
            << "displayedFrames,"
            << "droppedFrames,"
            << "packetLossRate,"
            << "lastAckMissingRate,"
            << "networkConditionEnabled,"
            << "networkConditionLossRate,"
            << "networkConditionDuplicateRate,"
            << "networkConditionReorderRate,"
            << "networkConditionMinDelayMs,"
            << "networkConditionMaxDelayMs,"
            << "networkConditionBurstLossLength\n";

        eventHeaderWritten_ = true;
    }

    void NetworkCsvLogger::WriteEventSample(
        const NetworkStatsSnapshot& stats,
        double appTimeSec,
        bool startupWarmupActive,
        uint64_t outputQueueDroppedFramesDelta,
        uint64_t outputQueueDropEventsDelta,
        uint64_t outputQueueDropBurstEventsDelta,
        uint64_t receiveFreshnessDroppedFramesDelta,
        const std::string& freshnessDropClass,
        const std::string& freshnessDropEvidence
    ) {
        (void)startupWarmupActive;

        ++summaryTotalRows_;

        const bool qoe2 = stats.adaptiveLastQoeScore >= 2.0 &&
            stats.adaptiveLastQoeScore < 3.0;
        const bool qoe3 = stats.adaptiveLastQoeScore >= 3.0;
        const bool recoveryRaw =
            stats.adaptiveRecoveryDeadlineRawDelta > 0 ||
            stats.adaptiveRetransmitStaleRawDelta > 0 ||
            stats.adaptiveRecoveryDeadlineNoiseDelta > 0;
        const bool recoveryEffective =
            stats.adaptiveRecoveryDeadlineEffectiveDelta > 0;
        const bool retransmitEffective =
            stats.adaptiveRetransmitStaleEffectiveDelta > 0;
        const bool syncRisk =
            stats.adaptiveRecoveryDeadlineSyncRiskDelta > 0;
        const bool hardSyncLoss =
            stats.adaptiveRecoveryDeadlineHardSyncLossDelta > 0;
        const bool outputDrop =
            outputQueueDroppedFramesDelta > 0 ||
            outputQueueDropEventsDelta > 0 ||
            outputQueueDropBurstEventsDelta > 0;
        const bool freshnessDrop =
            receiveFreshnessDroppedFramesDelta > 0;
        const bool arrivalGapJitter =
            stats.adaptiveArrivalGapJitterSpikeActive;

        summaryQoe2Rows_ += qoe2 ? 1 : 0;
        summaryQoe3Rows_ += qoe3 ? 1 : 0;
        summaryRecoveryDeadlineRawRows_ += recoveryRaw ? 1 : 0;
        summaryRecoveryDeadlineEffectiveRows_ +=
            recoveryEffective ? 1 : 0;
        summaryRetransmitStaleEffectiveRows_ +=
            retransmitEffective ? 1 : 0;
        summarySyncRiskRows_ += syncRisk ? 1 : 0;
        summaryHardSyncLossRows_ += hardSyncLoss ? 1 : 0;
        summaryOutputDropRows_ += outputDrop ? 1 : 0;
        summaryFreshnessDropRows_ += freshnessDrop ? 1 : 0;
        summaryArrivalGapJitterRows_ += arrivalGapJitter ? 1 : 0;

        const bool important =
            qoe2 ||
            qoe3 ||
            recoveryRaw ||
            recoveryEffective ||
            retransmitEffective ||
            syncRisk ||
            hardSyncLoss ||
            outputDrop ||
            freshnessDrop ||
            arrivalGapJitter;
        if (!important) {
            return;
        }

        ++summaryEventRows_;
        if (!eventFile_.is_open()) {
            return;
        }

        if (!eventHeaderWritten_) {
            WriteEventHeader();
        }

        eventFile_ << std::fixed << std::setprecision(3)
            << appTimeSec << ','
            << EscapeCsv(ResolveScenarioName(stats)) << ','
            << EscapeCsv(stats.networkRuntimeModeName) << ','
            << (stats.networkExperimentActive ? 1 : 0) << ','
            << EscapeCsv(stats.networkExperimentScenarioName) << ','
            << EscapeCsv(stats.networkExperimentAdaptiveMode) << ','
            << stats.latestFrameId << ','
            << stats.receiveFps << ','
            << stats.decodeFps << ','
            << stats.displayFps << ','
            << stats.currentLatencyMs << ','
            << stats.currentRttMs << ','
            << stats.currentJitterMs << ','
            << stats.adaptiveLastQoeScore << ','
            << EscapeCsv(stats.adaptiveDegradationCause) << ','
            << stats.adaptiveRecoveryDeadlineRawDelta << ','
            << stats.adaptiveRecoveryDeadlineEffectiveDelta << ','
            << stats.adaptiveRetransmitStaleRawDelta << ','
            << stats.adaptiveRetransmitStaleEffectiveDelta << ','
            << stats.adaptiveRecoveryDeadlineNoiseDelta << ','
            << stats.adaptiveRecoveryDeadlineSyncRiskDelta << ','
            << stats.adaptiveRecoveryDeadlineHardSyncLossDelta << ','
            << (stats.adaptiveRecoveryDeadlineSyncEvidenceActive ? 1 : 0)
            << ','
            << stats.adaptiveRecoveryDeadlineSyncEvidenceAgeMs << ','
            << EscapeCsv(stats.adaptiveRecoveryDeadlineSyncEvidenceSource)
            << ','
            << (stats.adaptiveArrivalGapJitterSpikeActive ? 1 : 0)
            << ','
            << outputQueueDroppedFramesDelta << ','
            << outputQueueDropEventsDelta << ','
            << outputQueueDropBurstEventsDelta << ','
            << receiveFreshnessDroppedFramesDelta << ','
            << EscapeCsv(freshnessDropClass) << ','
            << EscapeCsv(freshnessDropEvidence) << ','
            << EscapeCsv(stats.lastOutputQueueDropReason) << ','
            << EscapeCsv(stats.receiveDecodeLastDropReason) << ','
            << stats.deadlineDroppedFrames << ','
            << stats.outputQueueDroppedFrames << ','
            << stats.receiveFreshnessDroppedFrames << ','
            << stats.deadlineNackExpiredDroppedFrames << ','
            << stats.deadlineNackExpiredH264KeyFrames << ','
            << stats.ackStaleDroppedFrames << ','
            << stats.displayedFrames << ','
            << stats.droppedFrames << ','
            << stats.packetLossRate << ','
            << stats.lastAckMissingRate << ','
            << (stats.networkCondition.enabled ? 1 : 0) << ','
            << stats.networkCondition.lossRate << ','
            << stats.networkCondition.duplicateRate << ','
            << stats.networkCondition.reorderRate << ','
            << stats.networkCondition.minDelayMs << ','
            << stats.networkCondition.maxDelayMs << ','
            << stats.networkCondition.burstLossLength
            << '\n';

        eventFile_.flush();
    }

    void NetworkCsvLogger::WriteRunSummary() {
        if (filePath_.empty()) {
            return;
        }

        const std::filesystem::path summaryPath =
            RunSummaryPathFor(std::filesystem::path(filePath_));
        std::ofstream summary(summaryPath, std::ios::out | std::ios::trunc);
        if (!summary) {
            return;
        }

        summary
            << "{\n"
            << "  \"sourceCsv\": \"" << JsonEscape(filePath_) << "\",\n"
            << "  \"eventCsv\": \"" << JsonEscape(eventFilePath_) << "\",\n"
            << "  \"eventMode\": \"streaming\",\n"
            << "  \"totalRows\": " << summaryTotalRows_ << ",\n"
            << "  \"eventRows\": " << summaryEventRows_ << ",\n"
            << "  \"qoe2Rows\": " << summaryQoe2Rows_ << ",\n"
            << "  \"qoe3Rows\": " << summaryQoe3Rows_ << ",\n"
            << "  \"recoveryDeadlineRawRows\": "
            << summaryRecoveryDeadlineRawRows_ << ",\n"
            << "  \"recoveryDeadlineEffectiveRows\": "
            << summaryRecoveryDeadlineEffectiveRows_ << ",\n"
            << "  \"retransmitStaleEffectiveRows\": "
            << summaryRetransmitStaleEffectiveRows_ << ",\n"
            << "  \"syncRiskRows\": " << summarySyncRiskRows_ << ",\n"
            << "  \"hardSyncLossRows\": " << summaryHardSyncLossRows_ << ",\n"
            << "  \"outputDropRows\": " << summaryOutputDropRows_ << ",\n"
            << "  \"freshnessDropRows\": " << summaryFreshnessDropRows_
            << ",\n"
            << "  \"arrivalGapJitterRows\": "
            << summaryArrivalGapJitterRows_ << "\n"
            << "}\n";
    }

    bool NetworkCsvLogger::IsRunning() const {
        return file_.is_open();
    }

    const std::string& NetworkCsvLogger::FilePath() const {
        return filePath_;
    }

    void NetworkCsvLogger::WriteHeader() {
        if (!file_.is_open() || headerWritten_) {
            return;
        }

        file_
            << "timeSec,"
            << "scenarioName,"
            << "networkRuntimeMode,"
            << "networkModeSendingEnabled,"
            << "networkModeReceivingEnabled,"
            << "networkExperimentActive,"
            << "networkExperimentScenarioName,"
            << "networkExperimentAdaptiveMode,"
            << "networkExperimentRemainingSec,"
            << "networkExperimentStepIndex,"
            << "networkExperimentStepCount,"
            << "frameId,"
            << "receiveFps,"
            << "decodeFps,"
            << "displayFps,"
            << "currentLatencyMs,"
            << "averageLatencyMs,"
            << "currentRttMs,"
            << "averageRttMs,"
            << "currentJitterMs,"
            << "packetLossRate,"
            << "frameDropRate,"
            << "receivedPackets,"
            << "missingPackets,"
            << "sequenceGapPackets,"
            << "sequenceGapRecoveredPackets,"
            << "duplicatePackets,"
            << "duplicateOriginalPackets,"
            << "duplicateRetransmitPackets,"
            << "duplicateLateAfterCompletedPackets,"
            << "duplicateLateAfterCompletedOriginalPackets,"
            << "duplicateLateAfterCompletedRetransmitPackets,"
            << "duplicateLateAfterExpiredPackets,"
            << "duplicateLateAfterRejectedPackets,"
            << "reorderedPackets,"
            << "completedFrames,"
            << "droppedFrames,"
            << "deadlineDroppedFrames,"
            << "outputQueueDroppedFrames,"
            << "outputQueueDropEvents,"
            << "outputQueueDropBurstEvents,"
            << "startupWarmupSec,"
            << "startupWarmupActive,"
            << "outputQueueDroppedFramesDelta,"
            << "outputQueueDropEventsDelta,"
            << "outputQueueDropBurstEventsDelta,"
            << "startupOutputQueueDroppedFrames,"
            << "startupOutputQueueDropEvents,"
            << "startupOutputQueueDropBurstEvents,"
            << "steadyOutputQueueDroppedFrames,"
            << "steadyOutputQueueDropEvents,"
            << "steadyOutputQueueDropBurstEvents,"
            << "evaluationOutputQueueDroppedFrames,"
            << "evaluationOutputQueueDropEvents,"
            << "evaluationOutputQueueDropBurstEvents,"
            << "lastOutputQueueDropFrameCount,"
            << "lastOutputQueueDropQueueSize,"
            << "lastOutputQueueDropOldestAgeMs,"
            << "lastOutputQueueDropNewestAgeMs,"
            << "maxOutputQueueDropOldestAgeMs,"
            << "lastOutputQueueDropReason,"
            << "completedQueuePushes,"
            << "completedQueuePops,"
            << "completedQueueEmptyPolls,"
            << "completedQueueSize,"
            << "maxCompletedQueueSize,"
            << "completedQueueLastPopAgeMs,"
            << "completedQueueMaxPopAgeMs,"
            << "completedQueueLastPushIntervalMs,"
            << "completedQueueMaxPushIntervalMs,"
            << "lastOutputQueueDropPopAgeMs,"
            << "lastOutputQueueDropPushIntervalMs,"
            << "decodedFrames,"
            << "displayedFrames,"
            << "frameRecoveryOutcomeEvents,"
            << "frameRecoveryCompletedFrames,"
            << "frameRecoveryExpiredFrames,"
            << "frameRecoveryRejectedFrames,"
            << "frameRecoveryNackSentFrames,"
            << "frameRecoveryFecRecoveredFrames,"
            << "frameRecoveryLastFrameId,"
            << "frameRecoveryLastStreamId,"
            << "frameRecoveryLastEvent,"
            << "frameRecoveryLastOutcome,"
            << "frameRecoveryLastCodec,"
            << "frameRecoveryLastKeyFrame,"
            << "frameRecoveryLastLargeFrame,"
            << "frameRecoveryLastChunkCount,"
            << "frameRecoveryLastReceivedChunks,"
            << "frameRecoveryLastMissingChunks,"
            << "frameRecoveryLastFecParityPackets,"
            << "frameRecoveryLastFecRecoveredChunks,"
            << "frameRecoveryLastNackCount,"
            << "frameRecoveryLastPostNackReceivedChunks,"
            << "frameRecoveryLastNackRequestedChunks,"
            << "frameRecoveryLastRetransmitReceivedChunks,"
            << "frameRecoveryLastRetransmitDuplicatePackets,"
            << "frameRecoveryLastPacketSequence,"
            << "frameRecoveryLastPacketChunkIndex,"
            << "frameRecoveryLastRetransmitSequence,"
            << "frameRecoveryLastRetransmitChunkIndex,"
            << "frameRecoveryLastEventPacketSequence,"
            << "frameRecoveryLastEventPacketChunkIndex,"
            << "frameRecoveryLastEventWasRetransmit,"
            << "frameRecoveryLastAgeMs,"
            << "retransmitUsefulChunks,"
            << "retransmitDuplicatePackets,"
            << "retransmitLateAfterCompletedPackets,"
            << "retransmitLateAfterCompletedLargePackets,"
            << "retransmitLateAfterCompletedSentBeforeCompletePackets,"
            << "retransmitLateAfterCompletedSentAfterCompletePackets,"
            << "retransmitLateAfterCompletedAvgSendToCompleteMs,"
            << "retransmitLateAfterCompletedAvgDelayMs,"
            << "retransmitLateAfterCompletedMaxDelayMs,"
            << "retransmitLateAfterExpiredPackets,"
            << "retransmitLateAfterRejectedPackets,"
            << "retransmitClassifiedPackets,"
            << "retransmitNotArrivedPackets,"
            << "retransmitAccountedPackets,"
            << "retransmitUnclassifiedPackets,"
            << "retransmitCompletedFrames,"
            << "retransmitExpiredFrames,"
            << "retransmitUsefulnessRatio,"
            << "retransmitDuplicateRatio,"
            << "retransmitFinalClassificationRatio,"
            << "retransmitFinalAccountingRatio,"
            << "retransmitExpiredAfterUsefulRatio,"
            << "dynamicNackDeadlineMs,"
            << "dynamicNackUsefulnessRatio,"
            << "dynamicNackDuplicateRatio,"
            << "dynamicNackExpiredAfterRetransmitRatio,"
            << "dynamicNackDecisionReason,"
            << "nackSuppressedFrames,"
            << "nackSuppressedMissingChunks,"
            << "nackPreflightSuppressedFrames,"
            << "nackPreflightSuppressedChunks,"
            << "nackFecGraceSuppressedFrames,"
            << "nackFecGraceSuppressedChunks,"
            << "nackPredictedUsefulFrames,"
            << "nackPredictedUsefulChunks,"
            << "nackDeferredForLikelyArrivalFrames,"
            << "nackDeferredForLikelyArrivalChunks,"
            << "nackSkippedTooLateFrames,"
            << "nackSkippedTooLateChunks,"
            << "nackRequestedChunkBudget,"
            << "nackLastShapingReason,"
            << "nackLastSuppressionReason,"
            << "ackCount,"
            << "lastAckMissingRate,"
            << "ackRetransmittedFrames,"
            << "ackRetransmittedChunks,"
            << "repairCanceledByCompleteAckPackets,"
            << "repairSkippedByTtlPackets,"
            << "repairQueuedButCanceledPackets,"
            << "repairSentAfterCompleteAckPackets,"
            << "repairSentAfterCompleteAckLargePackets,"
            << "repairSuppressedByFecLikelyFrames,"
            << "repairSuppressedByFecLikelyPackets,"
            << "repairSuppressedByFecLikelyLargeFrames,"
            << "repairSuppressedByFecLikelyLargePackets,"
            << "repairBudgetSuppressedFrames,"
            << "repairBudgetSuppressedPackets,"
            << "repairBudgetSuppressedLargeFrames,"
            << "repairBudgetSuppressedLargePackets,"
            << "repairRaceGuardSuppressedFrames,"
            << "repairRaceGuardSuppressedPackets,"
            << "repairRaceGuardSuppressedLargePackets,"
            << "repairBudgetProfile,"
            << "repairBudgetProfileSwitches,"
            << "repairBudgetSmoothedMissingRate,"
            << "repairBudgetSmoothedPacingQueueDelayMs,"
            << "repairBudgetSmoothedDeliveryMs,"
            << "repairFecLikelySuppressedCompletedFrames,"
            << "repairFecLikelySuppressedCompletedPackets,"
            << "repairFecLikelySuppressedExpiredFrames,"
            << "repairFecLikelySuppressedExpiredPackets,"
            << "repairFecLikelySuppressedPendingFrames,"
            << "repairFecLikelySuppressedPendingPackets,"
            << "repairFecLikelySuppressionRescueFrames,"
            << "repairFecLikelySuppressionRescuePackets,"
            << "h264KeyTinyMissingCriticalFrames,"
            << "h264KeyTinyMissingCriticalPackets,"
            << "h264KeyTinyMissingCriticalSentPackets,"
            << "h264KeyTinyMissingCriticalSkippedPackets,"
            << "h264KeyTinyMissingCriticalFeasibilitySuppressedFrames,"
            << "h264KeyTinyMissingCriticalFeasibilitySuppressedPackets,"
            << "h264KeyTinyMissingCriticalFeasibilityBypassedFrames,"
            << "h264KeyTinyMissingCriticalFeasibilityBypassedPackets,"
            << "h264KeyTinyMissingCriticalLastPredictedDeliveryMs,"
            << "h264KeyTinyMissingCriticalLastRemainingSlackMs,"
            << "h264KeyTinyMissingCriticalLastFrameId,"
            << "h264KeyTinyMissingCriticalLastAckMissingChunks,"
            << "h264KeyTinyMissingCriticalLastRequestedChunks,"
            << "h264KeyTinyMissingCriticalLastEvent,"
            << "h264KeyTinyEarlyNackFrames,"
            << "h264KeyTinyEarlyNackMissingChunks,"
            << "h264KeySmallMissingAckFrames,"
            << "h264KeySmallMissingAckMissingChunks,"
            << "h264KeySmallMissingAckHistoryMissingFrames,"
            << "h264KeySmallMissingAckStaleFrameLagFrames,"
            << "h264KeySmallMissingAckStaleAgeFrames,"
            << "h264KeySmallMissingAckRetransmitBudgetExhaustedFrames,"
            << "h264KeySmallMissingAckDynamicBudgetSuppressedFrames,"
            << "h264KeySmallMissingAckDynamicBudgetSuppressedPackets,"
            << "h264KeySmallMissingAckSelectedRepairFrames,"
            << "h264KeySmallMissingAckSelectedRepairPackets,"
            << "h264KeySelectedRepair1To2Frames,"
            << "h264KeySelectedRepair1To2Packets,"
            << "h264KeySelectedRepair3To4Frames,"
            << "h264KeySelectedRepair3To4Packets,"
            << "h264KeySmallMissingAckLastFrameId,"
            << "h264KeySmallMissingAckLastMissingChunks,"
            << "h264KeySmallMissingAckLastGate,"
            << "h264KeyRepair1To2CompletedFrames,"
            << "h264KeyRepair1To2ExpiredFrames,"
            << "h264KeyRepair1To2ArrivedPackets,"
            << "h264KeyRepair1To2DuplicatePackets,"
            << "h264KeyRepair1To2LateCompletedPackets,"
            << "h264KeyRepair1To2LateExpiredPackets,"
            << "h264KeyRepair1To2LateRejectedPackets,"
            << "h264KeyTinyEmergencyCompletedFrames,"
            << "h264KeyTinyEmergencyArrivedBeforeRetirePackets,"
            << "h264KeyTinyEmergencyDuplicateBeforeRetirePackets,"
            << "h264KeyTinyEmergencyArrivedAfterCompletePackets,"
            << "h264KeyTinyEmergencyArrivedAfterExpirePackets,"
            << "h264KeyTinyEmergencyArrivedAfterRejectedPackets,"
            << "h264KeyTinyEmergencyNotArrivedPackets,"
            << "h264KeyTinyEmergencySendToArrivalAvgMs,"
            << "h264KeyTinyEmergencySendToArrivalMaxMs,"
            << "h264KeyTinyLastChanceNackFrames,"
            << "h264KeyTinyLastChanceMissing1Frames,"
            << "h264KeyTinyLastChanceCompletedFrames,"
            << "h264KeyTinyLastChanceExpiredFrames,"
            << "h264KeyTinyLastChanceArrivedBeforeRetirePackets,"
            << "h264KeyTinyLastChanceDuplicateBeforeRetirePackets,"
            << "h264KeyTinyLastChanceLateCompletedPackets,"
            << "h264KeyTinyLastChanceLateExpiredPackets,"
            << "h264KeyTinyLastChanceLateRejectedPackets,"
            << "h264KeyTinyLastChanceSlackAvgMs,"
            << "h264KeyTinyLastChanceSlackMinMs,"
            << "h264KeyTinyLastChanceSlackMaxMs,"
            << "h264KeyRepair3To4CompletedFrames,"
            << "h264KeyRepair3To4ExpiredFrames,"
            << "h264KeyRepair3To4ArrivedPackets,"
            << "h264KeyRepair3To4DuplicatePackets,"
            << "h264KeyRepair3To4LateCompletedPackets,"
            << "h264KeyRepair3To4LateExpiredPackets,"
            << "h264KeyRepair3To4LateRejectedPackets,"
            << "lateRepairSavedPackets,"
            << "ackStaleDroppedFrames,"
            << "ackKeyFrameRequests,"
            << "h264KeyFrameRequestAckHistoryMissing,"
            << "h264KeyFrameRequestAckStaleFrameLag,"
            << "h264KeyFrameRequestAckStaleAge,"
            << "h264KeyFrameRequestAckRetransmitBudgetExhausted,"
            << "h264KeyFrameRequestAckHighMissingRate,"
            << "h264KeyFrameRequestAckCooldownSuppressed,"
            << "h264KeyFrameRequestAckAlreadyPending,"
            << "h264KeyFrameRequestAckCooldownNoise,"
            << "h264KeyFrameRequestAckCooldownSyncRisk,"
            << "h264KeyFrameRequestAckStaleAgeCooldownNoise,"
            << "h264KeyFrameRequestAckStaleAgeCooldownSyncRisk,"
            << "h264KeyFrameRequestAckLastReason,"
            << "h264KeyFrameRequestReceiverRequests,"
            << "h264KeyFrameRequestReceiverCooldownSuppressed,"
            << "h264KeyFrameRequestReceiverCooldownNoise,"
            << "h264KeyFrameRequestReceiverCooldownSyncRisk,"
            << "h264KeyFrameRequestReceiverTrueSyncLoss,"
            << "h264KeyFrameRequestReceiverMissingAck,"
            << "h264KeyFrameRequestReceiverDeadlineExpired,"
            << "h264KeyFrameRequestReceiverDeadlineNackMissing,"
            << "h264KeyFrameRequestReceiverDeadlineNackMissingCooldownNoise,"
            << "h264KeyFrameRequestReceiverDeadlineNackMissingCooldownSyncRisk,"
            << "h264KeyFrameRequestReceiverPayloadHeaderFailure,"
            << "h264KeyFrameRequestReceiverAuInvalid,"
            << "h264KeyFrameRequestReceiverInitWaitIdr,"
            << "h264KeyFrameRequestReceiverWaitingForIdr,"
            << "h264KeyFrameRequestReceiverDecodeFailure,"
            << "h264KeyFrameRequestReceiverStaleAfterDecode,"
            << "h264KeyFrameRequestReceiverLastReason,"
            << "ackKeyFramePending,"
            << "pacingEnabled,"
            << "pacingTargetBitrateBps,"
            << "pacingRepairTargetBitrateBps,"
            << "pacingQueuedPackets,"
            << "pacingHighPriorityQueuedPackets,"
            << "pacingNormalQueuedPackets,"
            << "pacingEnqueuedPackets,"
            << "pacingSentPackets,"
            << "pacingSentBytes,"
            << "pacingRepairSentPackets,"
            << "pacingRepairSentBytes,"
            << "pacingRepairBorrowedPackets,"
            << "pacingRepairBorrowedBytes,"
            << "pacingEmergencySentPackets,"
            << "pacingEmergencySentBytes,"
            << "pacingEmergencyBorrowedPackets,"
            << "pacingEmergencyBorrowedBytes,"
            << "h264KeyTinyEmergencyLastQueueAgeMs,"
            << "h264KeyTinyEmergencyMaxQueueAgeMs,"
            << "h264KeyTinyEmergencyAvgQueueAgeMs,"
            << "pacingDroppedPackets,"
            << "pacingDeadlineDroppedPackets,"
            << "pacingHighPriorityDeadlineDroppedPackets,"
            << "pacingNormalDeadlineDroppedPackets,"
            << "pacingOverflowDroppedPackets,"
            << "pacingCurrentQueueDelayMs,"
            << "pacingMaxQueueDelayMs,"
            << "pacingVideoCreditBytes,"
            << "pacingRepairCreditBytes,"
            << "transportFeedbackPackets,"
            << "transportFeedbackPacketStatuses,"
            << "transportFeedbackReceivedPackets,"
            << "transportFeedbackMissingPackets,"
            << "transportFeedbackSequenceGapPackets,"
            << "transportFeedbackSequenceGapRecoveredPackets,"
            << "transportFeedbackLossRate,"
            << "transportFeedbackArrivalJitterMs,"
            << "transportFeedbackQueueDelayTrendMs,"
            << "transportFeedbackLastSequence,"
            << "estimatedBandwidthBps,"
            << "deliveryRateBps,"
            << "bandwidthQueueDelayMs,"
            << "bandwidthRttTrendMs,"
            << "bandwidthLossTrend,"
            << "bandwidthJitterTrendMs,"
            << "bandwidthFeedbackSamples,"
            << "deadlineNackSentFrames,"
            << "deadlineNackRecoveredFrames,"
            << "deadlineNackMissingChunks,"
            << "deadlineNackSentH264KeyFrames,"
            << "deadlineNackSentH264LargeFrames,"
            << "deadlineNackSentH264DeltaFrames,"
            << "deadlineNackExpiredDroppedFrames,"
            << "deadlineNackExpiredAfterNackFrames,"
            << "deadlineNackExpiredMissingChunks,"
            << "deadlineNackExpiredH264KeyFrames,"
            << "deadlineNackExpiredH264KeyMissingChunks,"
            << "deadlineNackLastExpiredH264KeyMissingChunks,"
            << "deadlineNackExpiredH264KeyMissingChunks1,"
            << "deadlineNackExpiredH264KeyMissingChunks2To4,"
            << "deadlineNackExpiredH264KeyMissingChunks5To8,"
            << "deadlineNackExpiredH264KeyMissingChunks9To16,"
            << "deadlineNackExpiredH264KeyMissingChunks17Plus,"
            << "h264KeySmallMissingDeadlineRescueFrames,"
            << "h264KeySmallMissingDeadlineRescueMissingChunks,"
            << "h264KeySmallMissingDeadlineRescueCompletedFrames,"
            << "h264KeySmallMissingDeadlineRescueRejectedFrames,"
            << "h264KeySmallMissingDeadlineRescueExpiredFrames,"
            << "h264KeySmallMissingDeadlineRescueCompletedMissingChunks,"
            << "h264KeySmallMissingDeadlineRescueExpiredMissingChunks,"
            << "deadlineNackExpiredH264LargeFrames,"
            << "deadlineNackExpiredH264DeltaFrames,"
            << "fecEnabled,"
            << "adaptiveFecEnabled,"
            << "fecGroupChunkCount,"
            << "fecParityPackets,"
            << "fecRecoveredFrames,"
            << "fecRecoveredChunks,"
            << "h264ReassemblerAuRejectedFrames,"
            << "h264ReassemblerHeaderFailures,"
            << "h264ReassemblerPayloadSizeMismatches,"
            << "h264ReassemblerFrameIdMismatches,"
            << "h264ReassemblerCrcMismatches,"
            << "h264ReassemblerLastRejectReason,"
            << "adaptiveFecDecisionReason,"
            << "adaptiveFecHoldReason,"
            << "adaptiveFecG8ToG4Recovery,"
            << "adaptiveFecQualityHoldActive,"
            << "adaptiveFecQualityHoldCanceled,"
            << "adaptiveFecEmergencyG2Active,"
            << "adaptiveFecEarlyOffReason,"
            << "adaptiveEnabled,"
            << "adaptiveControlMode,"
            << "adaptiveCongestionControlMode,"
            << "targetJpegQuality,"
            << "targetFps,"
            << "targetBitrateKbps,"
            << "adaptiveBandwidthCeilingKbps,"
            << "targetWidth,"
            << "targetHeight,"
            << "actualCodec,"
            << "actualEncodeWidth,"
            << "actualEncodeHeight,"
            << "actualRawFrameBytes,"
            << "actualEncodedFrameBytes,"
            << "rawFrameBytes,"
            << "encodedFrameBytes,"
            << "compressionRatio,"
            << "captureFps,"
            << "cameraCaptureSubtype,"
            << "cameraCaptureWidth,"
            << "cameraCaptureHeight,"
            << "cameraCaptureFormatFps,"
            << "encodeMs,"
            << "resizeMs,"
            << "nv12PrepareMs,"
            << "h264EncodeMs,"
            << "h264EncoderRequestedBitrateKbps,"
            << "h264EncoderTargetBitrateKbps,"
            << "h264EncoderAppliedBitrateKbps,"
            << "h264DynamicBitrateUpdateRequests,"
            << "h264DynamicBitrateUpdateSuccesses,"
            << "h264DynamicBitrateUpdateFailures,"
            << "h264EncoderReinitializations,"
            << "pacingBudgetTargetBitrateKbps,"
            << "h264VideoBudgetScale,"
            << "pacingBurstGuardActive,"
            << "fixedPacingRecentMaxQueueDelayMs,"
            << "fixedPacingQueueReleaseStableSec,"
            << "fixedPacingQueueRecovered,"
            << "fixedPacingQueueReleaseEligible,"
            << "adaptiveRepairBudgetUtilization,"
            << "adaptiveRepairBorrowedRatio,"
            << "adaptiveRepairSentBytesDelta,"
            << "adaptiveRepairBorrowedBytesDelta,"
            << "adaptiveRepairBudgetGuardActive,"
            << "adaptiveRepairVideoBudgetPressure,"
            << "adaptiveRetransmitUsefulRatio,"
            << "adaptiveLateRepairWasteRatio,"
            << "adaptiveRetransmitNotArrivedRatio,"
            << "adaptiveRetransmitAccountingComplete,"
            << "adaptiveLateRepairWastePressure,"
            << "adaptiveRetransmitNotArrivedPressure,"
            << "adaptiveRepairDecisionReason,"
            << "h264AuChunkCount,"
            << "h264AuIsIdr,"
            << "h264AuIsDecoderSync,"
            << "h264RequestedKeyFrameConsumedFrames,"
            << "h264EncoderKeyFrameRequests,"
            << "h264RecoveryKeyFrameRequests,"
            << "h264AwaitingSyncKeyFrameRequests,"
            << "h264PeriodicIdrRequests,"
            << "h264IdrFrames,"
            << "h264DecoderSyncFrames,"
            << "h264ConsecutiveIdrFrames,"
            << "h264MaxConsecutiveIdrFrames,"
            << "h264AuProtectionLevel,"
            << "h264AuDroppedBeforeSend,"
            << "h264AuDropReason,"
            << "h264AuDroppedBytes,"
            << "h264AuDroppedChunks,"
            << "h264AuPacingQueueDelayMs,"
            << "h264AuEstimatedSendMs,"
            << "h264InputGatedByPacing,"
            << "h264InputGateReason,"
            << "h264InputGateQueueDelayMs,"
            << "h264InputGateVideoCreditBytes,"
            << "h264InputGateFrameBudgetBytes,"
            << "h264InputGateDurationMs,"
            << "h264InputGateConsecutiveFrames,"
            << "h264InputGateSkippedInputFrames,"
            << "h264InputGateForcedOpen,"
            << "h264InputGateReleaseReason,"
            << "fecProtectedH264KeyFrames,"
            << "fecProtectedH264LargeFrames,"
            << "h264EncoderDelayFrames,"
            << "h264EncoderDelayMs,"
            << "h264EncoderPendingFrames,"
            << "h264EncodedInputFrameId,"
            << "h264EncoderCallMs,"
            << "h264EncoderSampleCreateMs,"
            << "h264EncoderProcessInputMs,"
            << "h264EncoderPreInputPollMs,"
            << "h264EncoderPostInputWaitMs,"
            << "h264EncoderProcessOutputMs,"
            << "h264EncoderOutputCopyMs,"
            << "h264EncoderProcessOutputAttempts,"
            << "h264EncoderAsyncEventCount,"
            << "h264EncoderHardware,"
            << "h264EncoderAsync,"
            << "h264EncoderNeedInputSignaled,"
            << "h264EncoderOutputProduced,"
            << "h264EncoderOutputProducedBeforeInput,"
            << "h264SubmittedNewInput,"
            << "h264AsyncSubmittedWithoutOutput,"
            << "h264AsyncPendingNoOutput,"
            << "h264AsyncCadenceHoldActive,"
            << "h264AsyncPendingNoOutputStreak,"
            << "h264AsyncCadenceScale,"
            << "h264AsyncCadenceHoldRemainingMs,"
            << "h264AsyncOutputPollBackoffMs,"
            << "h264InputCadenceFps,"
            << "h264NeedInputSubmitWake,"
            << "h264NeedInputSubmitLeadMs,"
            << "encodedCameraFrameId,"
            << "encodedCameraSourceTimestamp100ns,"
            << "encodedCameraCaptureCompletedTimeUs,"
            << "encodedCameraFrameAgeMs,"
            << "encodedCameraReadSampleMs,"
            << "encodedCameraReadSampleEndToCaptureMs,"
            << "encodedCameraCaptureToPublishMs,"
            << "encodedCameraPublishToAcquireMs,"
            << "encodedCameraAcquireToEncoderInputMs,"
            << "jpegEncodeMs,"
            << "packetizeMs,"
            << "sendFrameIntervalMs,"
            << "cameraFrameReady,"
            << "cameraFrameId,"
            << "cameraSourceTimestamp100ns,"
            << "cameraReadSampleStartTimeUs,"
            << "cameraReadSampleEndTimeUs,"
            << "cameraCaptureCompletedTimeUs,"
            << "cameraFramePublishedTimeUs,"
            << "cameraSenderAcquireTimeUs,"
            << "cameraReadSampleMs,"
            << "cameraReadSampleEndToCaptureMs,"
            << "cameraCaptureToPublishMs,"
            << "cameraPublishToAcquireMs,"
            << "cameraAcquireToSendMs,"
            << "cameraFrameAgeMs,"
            << "cameraFrameCacheUsed,"
            << "receiveJpegDecodeMs,"
            << "receiveDecodeWorkerFps,"
            << "receiveDecodeWorkerFrames,"
            << "receiveDecodePopSuccesses,"
            << "receiveDecodePopEmptyPolls,"
            << "receiveDecodeLoopLastPopGapMs,"
            << "receiveDecodeLoopMaxPopGapMs,"
            << "receiveStartupDecodeLoopMaxPopGapMs,"
            << "receiveSteadyDecodeLoopMaxPopGapMs,"
            << "receiveSteadyDecodeLoopMaxPopGapAtMs,"
            << "receiveSteadyDecodeLoopMaxPopGapFrameId,"
            << "receiveSteadyDecodeLoopMaxPopGapStreamId,"
            << "receiveSteadyDecodeLoopMaxPopGapCodec,"
            << "receiveSteadyDecodeLoopMaxPopGapInputFrameAgeMs,"
            << "receiveSteadyDecodeLoopMaxPopGapDecodedQueueSize,"
            << "receiveSteadyDecodeLoopMaxPopGapCompletedQueueSize,"
            << "receiveSteadyDecodeLoopMaxPopGapCompletedQueuePopAgeMs,"
            << "receiveSteadyDecodeLoopMaxPopGapCompletedQueuePushIntervalMs,"
            << "receiveSteadyDecodeLoopMaxPopGapArrivalRatio,"
            << "receiveSteadyDecodeLoopMaxPopGapReceiverJitterMs,"
            << "receiveSteadyDecodeLoopMaxPopGapReceiverLatencyMs,"
            << "receiveSteadyDecodeLoopMaxPopGapClass,"
            << "receiveSteadyDecodeLoopMaxPopGapAgeMs,"
            << "receiveH264StartupActive,"
            << "receiveH264StartupDecoderSynced,"
            << "receiveH264StartupFirstDecoded,"
            << "receiveH264StartupFirstDisplayed,"
            << "receiveH264StartupReady,"
            << "receiveH264StartupElapsedMs,"
            << "receiveH264StartupDecoderSyncMs,"
            << "receiveH264StartupFirstDecodedMs,"
            << "receiveH264StartupFirstDisplayedMs,"
            << "receiveH264StartupReadyMs,"
            << "receiveH264StartupQueueFlushFrames,"
            << "receiveDecodeOverwrittenFrames,"
            << "receiveDecodeQueueDroppedFrames,"
            << "receiveDecodeRenderOverwriteFrames,"
            << "receiveDecodeFailures,"
            << "receiveFreshnessDroppedFrames,"
            << "receiveFreshnessDroppedFramesDelta,"
            << "receiveFreshnessDropClass,"
            << "receiveFreshnessDropEvidence,"
            << "receiveLastFreshnessDropAgeMs,"
            << "receiveMaxFreshnessDropAgeMs,"
            << "receiveLastFreshnessDropFrameId,"
            << "receiveLastFreshnessDropStreamId,"
            << "receiveLastFreshnessDropCodec,"
            << "receiveH264AuInvalidFrames,"
            << "receiveH264AuCrcMismatches,"
            << "receiveH264AuPayloadSizeMismatches,"
            << "receiveH264AuNalCountMismatches,"
            << "receiveH264AuNoAnnexBNals,"
            << "receiveH264AuIdrFlagMismatches,"
            << "receiveH264AuSpsPpsFlagMismatches,"
            << "receiveH264AuSyncWithoutIdr,"
            << "receiveH264AuIdrWithoutSpsPps,"
            << "receiveH264AuForbiddenZeroBit,"
            << "receiveH264AuLastInvalidReason,"
            << "receiveDecodeInputFrameAgeMs,"
            << "receiveDecodeInputCameraFrameAgeMs,"
            << "receiveDecodeInputEncoderOutputAgeMs,"
            << "receiveLatestDecodedFrameAgeMs,"
            << "receiveLatestDecodedCameraFrameAgeMs,"
            << "receiveLatestDecodedEncoderOutputAgeMs,"
            << "receiveFreshnessDropThresholdMs,"
            << "receiveDecodeLastDropReason,"
            << "receiveUploadBufferWaitMs,"
            << "receiveDisplayFrameId,"
            << "receiveDisplayCameraFrameAgeMs,"
            << "receiveDisplayEncoderOutputAgeMs,"
            << "receiveDisplayDecodedFrameAgeMs,"
            << "textureUploadMs,"
            << "presentGpuWaitMs,"
            << "renderFramePacingWaitMs,"
            << "waitableSwapChainWaitMs,"
            << "presentMs,"
            << "presentSyncInterval,"
            << "lowLatencyPresentMode,"
            << "waitableSwapChainPacingEnabled,"
            << "waitableSwapChainAvailable,"
            << "swapChainBufferCount,"
            << "adaptiveInputPacketLossRate,"
            << "adaptiveInputReceiveFps,"
            << "adaptiveInputDecodeFps,"
            << "adaptiveInputJitterMs,"
            << "adaptiveInputDisplayFps,"
            << "adaptiveQoeScore,"
            << "adaptiveDegradationCause,"
            << "adaptiveRecoveryDeadlineRawDelta,"
            << "adaptiveRecoveryDeadlineEffectiveDelta,"
            << "adaptiveRetransmitStaleRawDelta,"
            << "adaptiveRetransmitStaleEffectiveDelta,"
            << "adaptiveRecoveryDeadlineNoiseDelta,"
            << "adaptiveRecoveryDeadlineSyncRiskDelta,"
            << "adaptiveRecoveryDeadlineHardSyncLossDelta,"
            << "adaptiveRecoveryDeadlineSyncEvidenceActive,"
            << "adaptiveRecoveryDeadlineSyncEvidenceAgeMs,"
            << "adaptiveRecoveryDeadlineSyncEvidenceSource,"
            << "adaptiveArrivalGapJitterSpikeActive,"
            << "adaptiveFecRecoveryWorking,"
            << "adaptiveFecGuardActive,"
            << "adaptiveFecRecoveryEfficiency,"
            << "adaptiveFecParityPacketDelta,"
            << "adaptiveFecRecoveredFrameDelta,"
            << "adaptiveFecRecoveredChunkDelta,"
            << "simEnabled,"
            << "simLossRate,"
            << "simDuplicateRate,"
            << "simReorderRate,"
            << "simMinDelayMs,"
            << "simMaxDelayMs,"
            << "simBurstLossLength,"
            << "simSubmittedPackets,"
            << "simSentPackets,"
            << "simDroppedPackets,"
            << "simDuplicatedPackets,"
            << "simReorderedPackets"
            << '\n';

        headerWritten_ = true;
    }

    std::string NetworkCsvLogger::ResolveScenarioName(
        const NetworkStatsSnapshot& stats) const {
        if (!scenarioName_.empty() && scenarioName_ != "Auto") {
            return scenarioName_;
        }

        const NetworkCondition& condition = stats.networkCondition;

        if (!condition.enabled) {
            return "Baseline";
        }
        if (MatchesCondition(condition, true, 0.10, 0, 0, 0)) {
            return "10% loss";
        }
        if (MatchesCondition(condition, true, 0.0, 0, 50, 0)) {
            return "50ms jitter";
        }
        if (MatchesCondition(condition, true, 0.0, 100, 100, 0)) {
            return "100ms delay";
        }
        if (MatchesCondition(condition, true, 0.03, 0, 0, 8)) {
            return "Burst loss";
        }

        std::ostringstream oss;
        oss << "Custom"
            << "_loss" << Percent(condition.lossRate)
            << "_delay" << condition.minDelayMs
            << "-" << condition.maxDelayMs
            << "_dup" << Percent(condition.duplicateRate)
            << "_reorder" << Percent(condition.reorderRate)
            << "_burst" << condition.burstLossLength;

        return oss.str();
    }

} // namespace net
