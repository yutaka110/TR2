#include "NetworkExperimentReporter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <ctime>
#include <unordered_map>

#include <Windows.h>

namespace net {
namespace {

    enum class PenaltyComparison {
        LessThan,
        LessThanOrEqual
    };

    struct QualityPenaltyRule {
        PenaltyComparison comparison = PenaltyComparison::LessThan;
        double threshold = 0.0;
        double penalty = 0.0;
    };

    struct ScoringConfig {
        double p95LatencyMsWeight = 1.0;
        double frameDropRateWeight = 500.0;
        double droppedFramesWeight = 8.0;
        double deadlineDroppedFramesWeight = 12.0;
        double outputQueueDroppedFramesWeight = 5.0;
        double deadlineNackExpiredDroppedFramesWeight = 3.0;
        double fecParityPacketWeight = 0.03;
        double fecInefficiencyPenaltyWeight = 20.0;
        double fecRecoveredFrameBonusWeight = 0.50;
        double avgDisplayFpsBonusWeight = 0.75;
        double avgQoeScoreBonusWeight = 8.0;

        std::vector<QualityPenaltyRule> fpsPenaltyRules;
        std::vector<QualityPenaltyRule> jpegQualityPenaltyRules;
        std::vector<QualityPenaltyRule> bitrateKbpsPenaltyRules;

        bool loadedFromFile = false;
        std::string sourcePath = "defaults";
    };

    std::vector<QualityPenaltyRule> DefaultFpsPenaltyRules() {
        return {
            { PenaltyComparison::LessThanOrEqual, 8.0, 500.0 },
            { PenaltyComparison::LessThan, 12.0, 250.0 },
            { PenaltyComparison::LessThan, 15.0, 120.0 }
        };
    }

    std::vector<QualityPenaltyRule> DefaultJpegQualityPenaltyRules() {
        return {
            { PenaltyComparison::LessThanOrEqual, 40.0, 450.0 },
            { PenaltyComparison::LessThan, 55.0, 220.0 },
            { PenaltyComparison::LessThan, 65.0, 90.0 }
        };
    }

    std::vector<QualityPenaltyRule> DefaultBitratePenaltyRules() {
        return {
            { PenaltyComparison::LessThanOrEqual, 1000.0, 350.0 },
            { PenaltyComparison::LessThan, 1800.0, 180.0 },
            { PenaltyComparison::LessThan, 2500.0, 80.0 }
        };
    }

    ScoringConfig MakeDefaultScoringConfig() {
        ScoringConfig config{};
        config.fpsPenaltyRules = DefaultFpsPenaltyRules();
        config.jpegQualityPenaltyRules = DefaultJpegQualityPenaltyRules();
        config.bitrateKbpsPenaltyRules = DefaultBitratePenaltyRules();
        return config;
    }

    ScoringConfig g_scoringConfig = MakeDefaultScoringConfig();

    std::string FormatDouble(double value);

    std::string MakeTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);

        std::tm localTime{};
        localtime_s(&localTime, &time);

        std::ostringstream oss;
        oss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
        return oss.str();
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

    std::string EscapeJson(std::string value) {
        std::string escaped;
        escaped.reserve(value.size() + 8);

        for (const char ch : value) {
            switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
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

    std::string EnvironmentValue(const char* name) {
        const DWORD requiredSize = GetEnvironmentVariableA(name, nullptr, 0);
        if (requiredSize == 0) {
            return {};
        }

        std::string value(requiredSize, '\0');
        const DWORD written = GetEnvironmentVariableA(
            name,
            value.data(),
            requiredSize
        );
        if (written == 0) {
            return {};
        }

        value.resize(written);
        return value;
    }

    void WriteJsonStringField(
        std::ostream& out,
        const char* name,
        const std::string& value,
        bool trailingComma
    ) {
        out << "    \"" << name << "\": \"" << EscapeJson(value) << "\"";
        if (trailingComma) {
            out << ',';
        }
        out << '\n';
    }

    const char* JsonBool(bool value) {
        return value ? "true" : "false";
    }

    bool ExtractJsonNumber(
        const std::string& text,
        const std::string& key,
        double& value
    ) {
        const std::regex pattern(
            "\"" + key + "\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)"
        );
        std::smatch match;
        if (!std::regex_search(text, match, pattern) || match.size() < 2) {
            return false;
        }

        char* end = nullptr;
        const double parsed = std::strtod(match[1].str().c_str(), &end);
        if (end == match[1].str().c_str()) {
            return false;
        }

        value = parsed;
        return true;
    }

    std::string ExtractJsonArrayBlock(
        const std::string& text,
        const std::string& key
    ) {
        const std::string keyPattern = "\"" + key + "\"";
        const size_t keyPos = text.find(keyPattern);
        if (keyPos == std::string::npos) {
            return {};
        }

        const size_t arrayStart = text.find('[', keyPos);
        if (arrayStart == std::string::npos) {
            return {};
        }

        size_t depth = 0;
        bool inString = false;
        bool escaped = false;
        for (size_t i = arrayStart; i < text.size(); ++i) {
            const char ch = text[i];

            if (inString) {
                if (escaped) {
                    escaped = false;
                }
                else if (ch == '\\') {
                    escaped = true;
                }
                else if (ch == '"') {
                    inString = false;
                }
                continue;
            }

            if (ch == '"') {
                inString = true;
                continue;
            }
            if (ch == '[') {
                depth++;
            }
            else if (ch == ']') {
                if (depth == 0) {
                    return {};
                }
                depth--;
                if (depth == 0) {
                    return text.substr(arrayStart, i - arrayStart + 1);
                }
            }
        }

        return {};
    }

    std::vector<QualityPenaltyRule> ParseQualityPenaltyRules(
        const std::string& text,
        const std::string& key
    ) {
        std::vector<QualityPenaltyRule> rules;
        const std::string arrayBlock = ExtractJsonArrayBlock(text, key);
        if (arrayBlock.empty()) {
            return rules;
        }

        const std::regex objectPattern("\\{([^\\}]*)\\}");
        for (auto it = std::sregex_iterator(
                arrayBlock.begin(),
                arrayBlock.end(),
                objectPattern);
            it != std::sregex_iterator();
            ++it) {
            const std::string objectText = (*it)[1].str();
            double threshold = 0.0;
            double penalty = 0.0;
            QualityPenaltyRule rule{};

            if (ExtractJsonNumber(objectText, "lte", threshold)) {
                rule.comparison = PenaltyComparison::LessThanOrEqual;
            }
            else if (ExtractJsonNumber(objectText, "lt", threshold)) {
                rule.comparison = PenaltyComparison::LessThan;
            }
            else {
                continue;
            }

            if (!ExtractJsonNumber(objectText, "penalty", penalty)) {
                continue;
            }

            rule.threshold = threshold;
            rule.penalty = penalty;
            rules.push_back(rule);
        }

        return rules;
    }

    ScoringConfig LoadScoringConfigFromFile(
        const std::string& configPath
    ) {
        ScoringConfig config = MakeDefaultScoringConfig();

        std::ifstream file(configPath);
        if (!file) {
            return config;
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string text = buffer.str();
        if (text.empty()) {
            return config;
        }

        ExtractJsonNumber(
            text,
            "p95LatencyMs",
            config.p95LatencyMsWeight);
        ExtractJsonNumber(
            text,
            "frameDropRate",
            config.frameDropRateWeight);
        ExtractJsonNumber(
            text,
            "droppedFrames",
            config.droppedFramesWeight);
        ExtractJsonNumber(
            text,
            "deadlineDroppedFrames",
            config.deadlineDroppedFramesWeight);
        ExtractJsonNumber(
            text,
            "outputQueueDroppedFrames",
            config.outputQueueDroppedFramesWeight);
        ExtractJsonNumber(
            text,
            "deadlineNackExpiredDroppedFrames",
            config.deadlineNackExpiredDroppedFramesWeight);
        ExtractJsonNumber(
            text,
            "fecParityPackets",
            config.fecParityPacketWeight);
        ExtractJsonNumber(
            text,
            "fecInefficiency",
            config.fecInefficiencyPenaltyWeight);
        ExtractJsonNumber(
            text,
            "fecRecoveredFramesBonus",
            config.fecRecoveredFrameBonusWeight);
        ExtractJsonNumber(
            text,
            "avgDisplayFpsBonus",
            config.avgDisplayFpsBonusWeight);
        ExtractJsonNumber(
            text,
            "avgQoeScoreBonus",
            config.avgQoeScoreBonusWeight);

        const std::vector<QualityPenaltyRule> fpsRules =
            ParseQualityPenaltyRules(text, "fps");
        const std::vector<QualityPenaltyRule> jpegRules =
            ParseQualityPenaltyRules(text, "jpegQuality");
        const std::vector<QualityPenaltyRule> bitrateRules =
            ParseQualityPenaltyRules(text, "bitrateKbps");

        if (!fpsRules.empty()) {
            config.fpsPenaltyRules = fpsRules;
        }
        if (!jpegRules.empty()) {
            config.jpegQualityPenaltyRules = jpegRules;
        }
        if (!bitrateRules.empty()) {
            config.bitrateKbpsPenaltyRules = bitrateRules;
        }

        config.loadedFromFile = true;
        config.sourcePath = configPath;
        return config;
    }

    const char* PenaltyComparisonName(PenaltyComparison comparison) {
        return comparison == PenaltyComparison::LessThanOrEqual
            ? "lte"
            : "lt";
    }

    double ApplyQualityPenaltyRules(
        double value,
        const std::vector<QualityPenaltyRule>& rules
    ) {
        if (value <= 0.0) {
            return 0.0;
        }

        for (const QualityPenaltyRule& rule : rules) {
            const bool matched =
                rule.comparison == PenaltyComparison::LessThanOrEqual
                ? value <= rule.threshold
                : value < rule.threshold;
            if (matched) {
                return rule.penalty;
            }
        }

        return 0.0;
    }

    void WritePenaltyRuleArray(
        std::ostream& file,
        const std::vector<QualityPenaltyRule>& rules,
        const char* indent
    ) {
        file << "[\n";
        for (size_t i = 0; i < rules.size(); ++i) {
            const QualityPenaltyRule& rule = rules[i];
            file << indent << "  { \""
                << PenaltyComparisonName(rule.comparison)
                << "\": "
                << FormatDouble(rule.threshold)
                << ", \"penalty\": "
                << FormatDouble(rule.penalty)
                << " }";
            if (i + 1 < rules.size()) {
                file << ',';
            }
            file << '\n';
        }
        file << indent << "]";
    }

    std::string FormatDouble(double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << value;
        return oss.str();
    }

    uint64_t SubtractCounter(uint64_t value, uint64_t baseline) {
        if (value < baseline) {
            return 0;
        }
        return value - baseline;
    }

    std::string FormatPercent(double ratio) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (ratio * 100.0) << "%";
        return oss.str();
    }

    std::string FormatSignedPercent(double ratio) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        if (ratio > 0.0) {
            oss << '+';
        }
        oss << (ratio * 100.0) << "%";
        return oss.str();
    }

    std::string FormatImprovementRatio(
        double baseline,
        double current,
        bool higherIsBetter
    ) {
        if (std::abs(baseline) < 0.0001) {
            return std::abs(current) < 0.0001 ? "0.0%" : "n/a";
        }

        const double ratio = higherIsBetter
            ? (current - baseline) / baseline
            : (baseline - current) / baseline;
        return FormatSignedPercent(ratio);
    }

    std::string EscapeMarkdownTable(std::string value) {
        for (char& ch : value) {
            if (ch == '|') {
                ch = '/';
            }
            else if (ch == '\r' || ch == '\n') {
                ch = ' ';
            }
        }
        return value;
    }

    std::string EscapeMermaidText(std::string value) {
        for (char& ch : value) {
            if (ch == '"' || ch == '\r' || ch == '\n') {
                ch = ' ';
            }
        }
        return value;
    }

    double ParseDoubleOrDefault(const std::string& value, double fallback = 0.0) {
        if (value.empty()) {
            return fallback;
        }

        char* end = nullptr;
        const double parsed = std::strtod(value.c_str(), &end);
        return end == value.c_str() ? fallback : parsed;
    }

    uint64_t ParseUint64OrDefault(
        const std::string& value,
        uint64_t fallback = 0
    ) {
        if (value.empty()) {
            return fallback;
        }

        char* end = nullptr;
        const unsigned long long parsed =
            std::strtoull(value.c_str(), &end, 10);
        return end == value.c_str()
            ? fallback
            : static_cast<uint64_t>(parsed);
    }

    int ParseIntOrDefault(const std::string& value, int fallback = 0) {
        if (value.empty()) {
            return fallback;
        }

        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        return end == value.c_str() ? fallback : static_cast<int>(parsed);
    }

    std::string FormatDelta(double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3);
        if (value > 0.0) {
            oss << '+';
        }
        oss << value;
        return oss.str();
    }

    std::string FormatIntDelta(int64_t value) {
        std::ostringstream oss;
        if (value > 0) {
            oss << '+';
        }
        oss << value;
        return oss.str();
    }

    std::string FormatChangePercent(uint64_t before, uint64_t after) {
        if (before == 0) {
            return after == 0 ? "0.0%" : "n/a";
        }

        const double ratio =
            (static_cast<double>(after) - static_cast<double>(before)) /
            static_cast<double>(before);
        return FormatPercent(ratio);
    }

    size_t AdaptiveCauseIndex(const std::string& cause) {
        if (cause == "Loss") {
            return 1;
        }
        if (cause == "Jitter") {
            return 2;
        }
        if (cause == "RTT") {
            return 3;
        }
        if (cause == "Bandwidth") {
            return 4;
        }
        if (cause == "DecodeLoad") {
            return 5;
        }
        if (cause == "DisplayLoad") {
            return 6;
        }
        if (cause == "FrameFreshness") {
            return 7;
        }
        if (cause == "RecoveryDeadline") {
            return 8;
        }
        if (cause == "PacingQueue") {
            return 9;
        }
        return 0;
    }

    const char* AdaptiveCauseName(size_t index) {
        switch (index) {
        case 1:
            return "Loss";
        case 2:
            return "Jitter";
        case 3:
            return "RTT";
        case 4:
            return "Bandwidth";
        case 5:
            return "DecodeLoad";
        case 6:
            return "DisplayLoad";
        case 7:
            return "FrameFreshness";
        case 8:
            return "RecoveryDeadline";
        case 9:
            return "PacingQueue";
        case 0:
        default:
            return "None";
        }
    }

    std::string ExtractNetworkScenarioName(const std::string& scenarioName) {
        const std::string marker = " / ";
        const size_t markerPos = scenarioName.find(marker);
        if (markerPos == std::string::npos) {
            return scenarioName;
        }
        return scenarioName.substr(0, markerPos);
    }

} // namespace

    bool NetworkExperimentReporter::Start(const std::string& directory) {
        Stop();

        g_scoringConfig =
            LoadScoringConfigFromFile("config/network_experiment_scoring.json");

        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return false;
        }

        const std::string timestamp = MakeTimestamp();
        const std::filesystem::path basePath(directory);
        const std::filesystem::path csvPath =
            basePath / ("network_summary_" + timestamp + ".csv");
        const std::filesystem::path textPath =
            basePath / ("network_summary_" + timestamp + ".txt");
        const std::filesystem::path markdownPath =
            basePath / ("network_report_" + timestamp + ".md");
        const std::filesystem::path beforeAfterPath =
            basePath / ("network_before_after_" + timestamp + ".md");
        const std::filesystem::path repeatReportPath =
            basePath / ("network_repeat_report_" + timestamp + ".md");
        const std::filesystem::path manifestPath =
            basePath / ("network_experiment_manifest_" + timestamp + ".json");

        csvFile_.open(csvPath, std::ios::out | std::ios::trunc);
        if (!csvFile_) {
            csvFilePath_.clear();
            textFilePath_.clear();
            markdownFilePath_.clear();
            beforeAfterFilePath_.clear();
            repeatReportFilePath_.clear();
            manifestFilePath_.clear();
            previousSummaryCsvPath_.clear();
            generatedTimestamp_.clear();
            summaries_.clear();
            return false;
        }

        textFile_.open(textPath, std::ios::out | std::ios::trunc);
        if (!textFile_) {
            csvFile_.close();
            csvFilePath_.clear();
            textFilePath_.clear();
            markdownFilePath_.clear();
            beforeAfterFilePath_.clear();
            repeatReportFilePath_.clear();
            manifestFilePath_.clear();
            previousSummaryCsvPath_.clear();
            generatedTimestamp_.clear();
            summaries_.clear();
            return false;
        }

        csvFilePath_ = csvPath.string();
        textFilePath_ = textPath.string();
        markdownFilePath_ = markdownPath.string();
        beforeAfterFilePath_ = beforeAfterPath.string();
        repeatReportFilePath_ = repeatReportPath.string();
        manifestFilePath_ = manifestPath.string();
        previousSummaryCsvPath_ =
            FindPreviousSummaryCsv(directory, csvFilePath_);
        generatedTimestamp_ = timestamp;
        summaries_.clear();
        headerWritten_ = false;
        ResetCurrent();
        WriteHeader();

        textFile_ << "Network Experiment Summary\n";
        textFile_ << "Generated: " << timestamp << "\n\n";
        textFile_.flush();
        WriteMarkdownReport();
        WriteBeforeAfterReport();
        WriteRepeatReport();

        return true;
    }

    void NetworkExperimentReporter::Stop() {
        FinalizeCurrent();

        if (csvFile_.is_open()) {
            csvFile_.flush();
            csvFile_.close();
        }

        if (textFile_.is_open()) {
            textFile_.flush();
            textFile_.close();
        }

        headerWritten_ = false;
        hasCurrent_ = false;
    }

    void NetworkExperimentReporter::RecordSample(
        const std::string& scenarioName,
        const NetworkStatsSnapshot& stats,
        double appTimeSec,
        double scenarioElapsedSec,
        double warmupSec
    ) {
        if (!csvFile_.is_open() || scenarioName.empty()) {
            return;
        }

        if (!hasCurrent_ || current_.name != scenarioName) {
            FinalizeCurrent();
            ResetCurrent();
            current_.name = scenarioName;
            current_.startTimeSec = appTimeSec;
            current_.measurementStartTimeSec = appTimeSec;
            current_.warmupSec = (std::max)(0.0, warmupSec);
            current_.minDisplayFps = (std::numeric_limits<double>::max)();
            current_.minTargetFps = (std::numeric_limits<int>::max)();
            current_.minTargetJpegQuality = (std::numeric_limits<int>::max)();
            current_.minTargetBitrateKbps = (std::numeric_limits<int>::max)();
            hasCurrent_ = true;
        }

        current_.endTimeSec = appTimeSec;
        const bool isWarmup =
            scenarioElapsedSec < current_.warmupSec;
        if (isWarmup) {
            current_.warmupSampleCount++;
            current_.lastStats = stats;
            return;
        }

        if (current_.sampleCount == 0) {
            current_.measurementStartTimeSec = appTimeSec;
            current_.measurementBaselineStats = stats;
            current_.hasMeasurementBaselineStats = true;
        }

        current_.sampleCount++;

        current_.latencySumMs += stats.currentLatencyMs;
        current_.maxLatencyMs =
            (std::max)(current_.maxLatencyMs, stats.currentLatencyMs);
        current_.latencySamplesMs.push_back(stats.currentLatencyMs);

        current_.displayFpsSum += stats.displayFps;
        if (stats.displayFps > 0.0) {
            current_.minDisplayFps =
                (std::min)(current_.minDisplayFps, stats.displayFps);
        }
        current_.receiveFpsSum += stats.receiveFps;
        current_.decodeFpsSum += stats.decodeFps;

        current_.packetLossRateSum += stats.packetLossRate;
        current_.maxPacketLossRate =
            (std::max)(current_.maxPacketLossRate, stats.packetLossRate);

        current_.jitterSumMs += stats.currentJitterMs;
        current_.maxJitterMs =
            (std::max)(current_.maxJitterMs, stats.currentJitterMs);

        if (stats.adaptiveTargetFps > 0) {
            current_.minTargetFps =
                (std::min)(current_.minTargetFps, stats.adaptiveTargetFps);
        }
        if (stats.adaptiveTargetJpegQuality > 0) {
            current_.minTargetJpegQuality =
                (std::min)(current_.minTargetJpegQuality, stats.adaptiveTargetJpegQuality);
        }
        if (stats.adaptiveTargetBitrateKbps > 0) {
            current_.minTargetBitrateKbps =
                (std::min)(current_.minTargetBitrateKbps, stats.adaptiveTargetBitrateKbps);
        }

        if (!stats.networkRuntimeModeName.empty()) {
            if (current_.networkRuntimeMode.empty()) {
                current_.networkRuntimeMode = stats.networkRuntimeModeName;
            }
            else if (current_.networkRuntimeMode != stats.networkRuntimeModeName) {
                current_.networkRuntimeMode = "Mixed";
            }
        }

        if (!stats.adaptiveControlMode.empty()) {
            if (current_.adaptiveControlMode.empty()) {
                current_.adaptiveControlMode = stats.adaptiveControlMode;
            }
            else if (current_.adaptiveControlMode != stats.adaptiveControlMode) {
                current_.adaptiveControlMode = "Mixed";
            }
        }

        if (!stats.adaptiveCongestionControlMode.empty()) {
            if (current_.adaptiveCongestionControlMode.empty()) {
                current_.adaptiveCongestionControlMode =
                    stats.adaptiveCongestionControlMode;
            }
            else if (current_.adaptiveCongestionControlMode !=
                stats.adaptiveCongestionControlMode) {
                current_.adaptiveCongestionControlMode = "Mixed";
            }
        }

        current_.adaptiveCauseSamples[
            AdaptiveCauseIndex(stats.adaptiveDegradationCause)]++;
        if (stats.adaptiveFecRecoveryWorking) {
            current_.adaptiveFecRecoveryWorkingSamples++;
        }
        if (stats.adaptiveFecGuardActive) {
            current_.adaptiveFecGuardActiveSamples++;
        }
        current_.adaptiveFecRecoveryEfficiencySum +=
            stats.adaptiveFecRecoveryEfficiency;
        current_.adaptiveFecParityPacketDeltas +=
            stats.adaptiveFecParityPacketDelta;
        current_.adaptiveFecRecoveredFrameDeltas +=
            stats.adaptiveFecRecoveredFrameDelta;
        current_.adaptiveFecRecoveredChunkDeltas +=
            stats.adaptiveFecRecoveredChunkDelta;

        TimeSeriesSample timeSeriesSample{};
        timeSeriesSample.relativeTimeSec =
            (std::max)(0.0, appTimeSec - current_.startTimeSec);
        timeSeriesSample.displayFps = stats.displayFps;
        timeSeriesSample.currentLatencyMs = stats.currentLatencyMs;
        timeSeriesSample.rollingP95LatencyMs =
            Percentile(current_.latencySamplesMs, 0.95);
        timeSeriesSample.adaptiveQoeScore = stats.adaptiveLastQoeScore;
        timeSeriesSample.adaptiveDegradationCause =
            stats.adaptiveDegradationCause.empty()
            ? "None"
            : stats.adaptiveDegradationCause;
        timeSeriesSample.adaptiveCauseScore =
            static_cast<double>(
                AdaptiveCauseIndex(timeSeriesSample.adaptiveDegradationCause)) *
            20.0;
        timeSeriesSample.targetJpegQuality =
            stats.adaptiveTargetJpegQuality;
        timeSeriesSample.targetFps = stats.adaptiveTargetFps;
        timeSeriesSample.targetBitrateKbps =
            stats.adaptiveTargetBitrateKbps;
        timeSeriesSample.bandwidthCeilingKbps =
            stats.adaptiveBandwidthCeilingKbps;
        timeSeriesSample.estimatedBandwidthBps =
            stats.estimatedBandwidthBps;
        timeSeriesSample.deliveryRateBps =
            stats.deliveryRateBps;
        timeSeriesSample.bandwidthQueueDelayMs =
            stats.bandwidthQueueDelayMs;
        timeSeriesSample.bandwidthLossTrend =
            stats.bandwidthLossTrend;
        timeSeriesSample.bandwidthJitterTrendMs =
            stats.bandwidthJitterTrendMs;
        const NetworkStatsSnapshot& baseline =
            current_.measurementBaselineStats;
        timeSeriesSample.deadlineNackSentFrames =
            SubtractCounter(
                stats.deadlineNackSentFrames,
                baseline.deadlineNackSentFrames);
        timeSeriesSample.deadlineNackRecoveredFrames =
            SubtractCounter(
                stats.deadlineNackRecoveredFrames,
                baseline.deadlineNackRecoveredFrames);
        timeSeriesSample.deadlineNackExpiredDroppedFrames =
            SubtractCounter(
                stats.deadlineNackExpiredDroppedFrames,
                baseline.deadlineNackExpiredDroppedFrames);
        timeSeriesSample.ackRetransmittedChunks =
            SubtractCounter(
                stats.ackRetransmittedChunks,
                baseline.ackRetransmittedChunks);
        timeSeriesSample.fecParityPackets =
            SubtractCounter(
                stats.fecParityPackets,
                baseline.fecParityPackets);
        timeSeriesSample.fecRecoveredFrames =
            SubtractCounter(
                stats.fecRecoveredFrames,
                baseline.fecRecoveredFrames);
        timeSeriesSample.fecRecoveredChunks =
            SubtractCounter(
                stats.fecRecoveredChunks,
                baseline.fecRecoveredChunks);
        timeSeriesSample.adaptiveFecRecoveryWorking =
            stats.adaptiveFecRecoveryWorking;
        timeSeriesSample.adaptiveFecGuardActive =
            stats.adaptiveFecGuardActive;
        timeSeriesSample.adaptiveFecRecoveryEfficiency =
            stats.adaptiveFecRecoveryEfficiency;
        timeSeriesSample.adaptiveFecParityPacketDelta =
            stats.adaptiveFecParityPacketDelta;
        timeSeriesSample.adaptiveFecRecoveredFrameDelta =
            stats.adaptiveFecRecoveredFrameDelta;
        timeSeriesSample.adaptiveFecRecoveredChunkDelta =
            stats.adaptiveFecRecoveredChunkDelta;
        timeSeriesSample.packetLossRate = stats.packetLossRate;
        timeSeriesSample.currentJitterMs = stats.currentJitterMs;
        current_.timeSeriesSamples.push_back(timeSeriesSample);

        current_.lastStats = stats;
    }

    bool NetworkExperimentReporter::IsRunning() const {
        return csvFile_.is_open();
    }

    const std::string& NetworkExperimentReporter::CsvFilePath() const {
        return csvFilePath_;
    }

    const std::string& NetworkExperimentReporter::TextFilePath() const {
        return textFilePath_;
    }

    const std::string& NetworkExperimentReporter::MarkdownFilePath() const {
        return markdownFilePath_;
    }

    const std::string& NetworkExperimentReporter::BeforeAfterFilePath() const {
        return beforeAfterFilePath_;
    }

    const std::string& NetworkExperimentReporter::RepeatReportFilePath() const {
        return repeatReportFilePath_;
    }

    const std::string& NetworkExperimentReporter::ManifestFilePath() const {
        return manifestFilePath_;
    }

    void NetworkExperimentReporter::WriteManifest(
        const std::vector<NetworkExperimentScenario>& scenarios,
        const std::string& rawCsvPath,
        NetworkRuntimeMode runtimeMode,
        bool autoExperiment,
        bool stopAfterOnePass,
        double warmupSec
    ) const {
        if (manifestFilePath_.empty()) {
            return;
        }

        std::ofstream file(manifestFilePath_, std::ios::out | std::ios::trunc);
        if (!file) {
            return;
        }

        const char* envNames[] = {
            "TR2_NETWORK_MODE",
            "TR2_NETWORK_EXPERIMENT_AUTO",
            "RNVP_DISABLE_CAMERA",
            "RNVP_FEC_ENABLED",
            "RNVP_FEC_GROUP_CHUNKS",
            "RNVP_PRESENT_SYNC_INTERVAL",
            "RNVP_LOW_LATENCY_PRESENT",
            "RNVP_WAITABLE_SWAPCHAIN"
        };

        file << "{\n";
        file << "  \"schemaVersion\": 1,\n";
        file << "  \"generated\": \"" << EscapeJson(generatedTimestamp_)
            << "\",\n";
        file << "  \"artifacts\": {\n";
        WriteJsonStringField(file, "rawCsv", rawCsvPath, true);
        WriteJsonStringField(file, "summaryCsv", csvFilePath_, true);
        WriteJsonStringField(file, "summaryText", textFilePath_, true);
        WriteJsonStringField(file, "reportMarkdown", markdownFilePath_, true);
        WriteJsonStringField(file, "beforeAfterReport", beforeAfterFilePath_, true);
        WriteJsonStringField(file, "repeatabilityReport", repeatReportFilePath_, false);
        file << "  },\n";

        file << "  \"runtime\": {\n";
        file << "    \"networkRuntimeMode\": \""
            << EscapeJson(ToString(runtimeMode)) << "\",\n";
        file << "    \"autoExperiment\": " << JsonBool(autoExperiment)
            << ",\n";
        file << "    \"stopAfterOnePass\": " << JsonBool(stopAfterOnePass)
            << ",\n";
        file << "    \"scenarioCount\": " << scenarios.size() << ",\n";
        file << "    \"warmupSec\": " << FormatDouble(warmupSec) << "\n";
        file << "  },\n";

        file << "  \"environment\": {\n";
        for (size_t i = 0; i < std::size(envNames); ++i) {
            const std::string value = EnvironmentValue(envNames[i]);
            file << "    \"" << envNames[i] << "\": ";
            if (value.empty()) {
                file << "null";
            }
            else {
                file << "\"" << EscapeJson(value) << "\"";
            }
            if (i + 1 < std::size(envNames)) {
                file << ',';
            }
            file << '\n';
        }
        file << "  },\n";

        file << "  \"scoring\": {\n";
        file << "    \"configPath\": \""
            << EscapeJson(g_scoringConfig.sourcePath) << "\",\n";
        file << "    \"loadedFromFile\": "
            << JsonBool(g_scoringConfig.loadedFromFile) << ",\n";
        file << "    \"lowerScoreWins\": true,\n";
        file << "    \"controllerComparisonUsesQoeBonus\": true,\n";
        file << "    \"repeatabilityReportUsesQoeBonus\": false,\n";
        file << "    \"weights\": {\n";
        file << "      \"p95LatencyMs\": "
            << FormatDouble(g_scoringConfig.p95LatencyMsWeight) << ",\n";
        file << "      \"frameDropRate\": "
            << FormatDouble(g_scoringConfig.frameDropRateWeight) << ",\n";
        file << "      \"droppedFrames\": "
            << FormatDouble(g_scoringConfig.droppedFramesWeight) << ",\n";
        file << "      \"deadlineDroppedFrames\": "
            << FormatDouble(g_scoringConfig.deadlineDroppedFramesWeight)
            << ",\n";
        file << "      \"outputQueueDroppedFrames\": "
            << FormatDouble(g_scoringConfig.outputQueueDroppedFramesWeight)
            << ",\n";
        file << "      \"deadlineNackExpiredDroppedFrames\": "
            << FormatDouble(
                g_scoringConfig.deadlineNackExpiredDroppedFramesWeight)
            << ",\n";
        file << "      \"fecParityPackets\": "
            << FormatDouble(g_scoringConfig.fecParityPacketWeight)
            << ",\n";
        file << "      \"fecInefficiency\": "
            << FormatDouble(g_scoringConfig.fecInefficiencyPenaltyWeight)
            << ",\n";
        file << "      \"fecRecoveredFramesBonus\": "
            << FormatDouble(g_scoringConfig.fecRecoveredFrameBonusWeight)
            << ",\n";
        file << "      \"avgDisplayFpsBonus\": "
            << FormatDouble(g_scoringConfig.avgDisplayFpsBonusWeight)
            << ",\n";
        file << "      \"avgQoeScoreBonus\": "
            << FormatDouble(g_scoringConfig.avgQoeScoreBonusWeight)
            << "\n";
        file << "    },\n";
        file << "    \"qualityFloorPenalties\": {\n";
        file << "      \"fps\": ";
        WritePenaltyRuleArray(
            file,
            g_scoringConfig.fpsPenaltyRules,
            "      ");
        file << ",\n";
        file << "      \"jpegQuality\": ";
        WritePenaltyRuleArray(
            file,
            g_scoringConfig.jpegQualityPenaltyRules,
            "      ");
        file << ",\n";
        file << "      \"bitrateKbps\": ";
        WritePenaltyRuleArray(
            file,
            g_scoringConfig.bitrateKbpsPenaltyRules,
            "      ");
        file << "\n";
        file << "    }\n";
        file << "  },\n";

        file << "  \"scenarios\": [\n";
        for (size_t i = 0; i < scenarios.size(); ++i) {
            const NetworkExperimentScenario& scenario = scenarios[i];
            const NetworkCondition& condition = scenario.condition;
            file << "    {\n";
            file << "      \"index\": " << i << ",\n";
            file << "      \"name\": \"" << EscapeJson(scenario.name)
                << "\",\n";
            file << "      \"networkScenarioName\": \""
                << EscapeJson(scenario.networkScenarioName) << "\",\n";
            file << "      \"durationSec\": " << FormatDouble(
                scenario.durationSec) << ",\n";
            file << "      \"warmupSec\": " << FormatDouble(warmupSec)
                << ",\n";
            file << "      \"measurementSec\": "
                << FormatDouble(
                    (std::max)(0.0, scenario.durationSec - warmupSec))
                << ",\n";
            file << "      \"adaptiveControlMode\": \""
                << EscapeJson(ToString(scenario.adaptiveControlMode))
                << "\",\n";
            file << "      \"congestionControlMode\": \""
                << EscapeJson(ToString(scenario.congestionControlMode))
                << "\",\n";
            file << "      \"fec\": {\n";
            file << "        \"enabled\": " << JsonBool(scenario.fecEnabled)
                << ",\n";
            file << "        \"adaptive\": "
                << JsonBool(scenario.adaptiveFecEnabled) << ",\n";
            file << "        \"groupChunkCount\": "
                << scenario.fecGroupChunkCount << "\n";
            file << "      },\n";
            file << "      \"networkCondition\": {\n";
            file << "        \"enabled\": " << JsonBool(condition.enabled)
                << ",\n";
            file << "        \"lossRate\": " << FormatDouble(condition.lossRate)
                << ",\n";
            file << "        \"duplicateRate\": "
                << FormatDouble(condition.duplicateRate) << ",\n";
            file << "        \"reorderRate\": "
                << FormatDouble(condition.reorderRate) << ",\n";
            file << "        \"minDelayMs\": " << condition.minDelayMs << ",\n";
            file << "        \"maxDelayMs\": " << condition.maxDelayMs << ",\n";
            file << "        \"burstLossLength\": "
                << condition.burstLossLength << ",\n";
            file << "        \"maxPendingPackets\": "
                << condition.maxPendingPackets << "\n";
            file << "      }\n";
            file << "    }";
            if (i + 1 < scenarios.size()) {
                file << ',';
            }
            file << '\n';
        }
        file << "  ]\n";
        file << "}\n";
    }

    void NetworkExperimentReporter::ResetCurrent() {
        current_ = ScenarioAccumulator{};
        hasCurrent_ = false;
    }

    void NetworkExperimentReporter::FinalizeCurrent() {
        if (!hasCurrent_ || current_.sampleCount == 0) {
            return;
        }

        const ScenarioSummary summary = BuildSummary(current_);
        WriteSummary(summary);
        summaries_.push_back(summary);
        WriteMarkdownReport();
        WriteBeforeAfterReport();
        WriteRepeatReport();
        ResetCurrent();
    }

    void NetworkExperimentReporter::WriteHeader() {
        if (!csvFile_.is_open() || headerWritten_) {
            return;
        }

        csvFile_
            << "scenarioName,"
            << "sampleCount,"
            << "startTimeSec,"
            << "measurementStartTimeSec,"
            << "endTimeSec,"
            << "durationSec,"
            << "warmupSec,"
            << "measurementSec,"
            << "warmupSampleCount,"
            << "measuredSampleCount,"
            << "avgLatencyMs,"
            << "p95LatencyMs,"
            << "maxLatencyMs,"
            << "avgDisplayFps,"
            << "minDisplayFps,"
            << "avgReceiveFps,"
            << "avgDecodeFps,"
            << "avgPacketLossRate,"
            << "maxPacketLossRate,"
            << "avgJitterMs,"
            << "maxJitterMs,"
            << "completedFrames,"
            << "displayedFrames,"
            << "droppedFrames,"
            << "deadlineDroppedFrames,"
            << "outputQueueDroppedFrames,"
            << "outputQueueDropEvents,"
            << "outputQueueDropBurstEvents,"
            << "maxOutputQueueDropOldestAgeMs,"
            << "lastOutputQueueDropReason,"
            << "ackCount,"
            << "ackRetransmittedFrames,"
            << "deadlineNackSentFrames,"
            << "deadlineNackRecoveredFrames,"
            << "deadlineNackMissingChunks,"
            << "deadlineNackExpiredDroppedFrames,"
            << "deadlineNackExpiredAfterNackFrames,"
            << "deadlineNackExpiredMissingChunks,"
            << "fecEnabled,"
            << "adaptiveFecEnabled,"
            << "fecGroupChunkCount,"
            << "fecParityPackets,"
            << "fecRecoveredFrames,"
            << "fecRecoveredChunks,"
            << "adaptiveFecRecoveryWorkingSamples,"
            << "adaptiveFecGuardActiveSamples,"
            << "adaptiveFecRecoveryWorkingRatio,"
            << "adaptiveFecGuardActiveRatio,"
            << "avgAdaptiveFecRecoveryEfficiency,"
            << "adaptiveFecParityPacketDeltas,"
            << "adaptiveFecRecoveredFrameDeltas,"
            << "adaptiveFecRecoveredChunkDeltas,"
            << "simDroppedPackets,"
            << "minTargetFps,"
            << "minTargetJpegQuality,"
            << "minTargetBitrateKbps,"
            << "networkRuntimeMode,"
            << "adaptiveControlMode,"
            << "adaptiveCongestionControlMode,"
            << "dominantAdaptiveDegradationCause,"
            << "verdict,"
            << "notes"
            << '\n';

        headerWritten_ = true;
    }

    void NetworkExperimentReporter::WriteSummary(
        const ScenarioSummary& summary
    ) {
        if (csvFile_.is_open()) {
            csvFile_ << std::fixed << std::setprecision(3)
                << EscapeCsv(summary.name) << ','
                << summary.sampleCount << ','
                << summary.startTimeSec << ','
                << summary.measurementStartTimeSec << ','
                << summary.endTimeSec << ','
                << summary.durationSec << ','
                << summary.warmupSec << ','
                << summary.measurementSec << ','
                << summary.warmupSampleCount << ','
                << summary.measuredSampleCount << ','
                << summary.avgLatencyMs << ','
                << summary.p95LatencyMs << ','
                << summary.maxLatencyMs << ','
                << summary.avgDisplayFps << ','
                << summary.minDisplayFps << ','
                << summary.avgReceiveFps << ','
                << summary.avgDecodeFps << ','
                << summary.avgPacketLossRate << ','
                << summary.maxPacketLossRate << ','
                << summary.avgJitterMs << ','
                << summary.maxJitterMs << ','
                << summary.completedFrames << ','
                << summary.displayedFrames << ','
                << summary.droppedFrames << ','
                << summary.deadlineDroppedFrames << ','
                << summary.outputQueueDroppedFrames << ','
                << summary.outputQueueDropEvents << ','
                << summary.outputQueueDropBurstEvents << ','
                << summary.maxOutputQueueDropOldestAgeMs << ','
                << EscapeCsv(summary.lastOutputQueueDropReason) << ','
                << summary.ackCount << ','
                << summary.ackRetransmittedFrames << ','
                << summary.deadlineNackSentFrames << ','
                << summary.deadlineNackRecoveredFrames << ','
                << summary.deadlineNackMissingChunks << ','
                << summary.deadlineNackExpiredDroppedFrames << ','
                << summary.deadlineNackExpiredAfterNackFrames << ','
                << summary.deadlineNackExpiredMissingChunks << ','
                << (summary.fecEnabled ? 1 : 0) << ','
                << (summary.adaptiveFecEnabled ? 1 : 0) << ','
                << summary.fecGroupChunkCount << ','
                << summary.fecParityPackets << ','
                << summary.fecRecoveredFrames << ','
                << summary.fecRecoveredChunks << ','
                << summary.adaptiveFecRecoveryWorkingSamples << ','
                << summary.adaptiveFecGuardActiveSamples << ','
                << summary.adaptiveFecRecoveryWorkingRatio << ','
                << summary.adaptiveFecGuardActiveRatio << ','
                << summary.avgAdaptiveFecRecoveryEfficiency << ','
                << summary.adaptiveFecParityPacketDeltas << ','
                << summary.adaptiveFecRecoveredFrameDeltas << ','
                << summary.adaptiveFecRecoveredChunkDeltas << ','
                << summary.simDroppedPackets << ','
                << summary.minTargetFps << ','
                << summary.minTargetJpegQuality << ','
                << summary.minTargetBitrateKbps << ','
                << EscapeCsv(summary.networkRuntimeMode) << ','
                << EscapeCsv(summary.adaptiveControlMode) << ','
                << EscapeCsv(summary.adaptiveCongestionControlMode) << ','
                << EscapeCsv(summary.dominantAdaptiveDegradationCause) << ','
                << EscapeCsv(summary.verdict) << ','
                << EscapeCsv(summary.notes)
                << '\n';
            csvFile_.flush();
        }

        if (textFile_.is_open()) {
            textFile_ << "Scenario: " << summary.name << "\n";
            textFile_ << "  samples: " << summary.sampleCount
                << " durationSec: " << FormatDouble(summary.durationSec)
                << "\n";
            textFile_ << "  warmup/measurement sec: "
                << FormatDouble(summary.warmupSec) << " / "
                << FormatDouble(summary.measurementSec)
                << " measuredSamples: "
                << summary.measuredSampleCount
                << " warmupSamplesExcluded: "
                << summary.warmupSampleCount << "\n";
            textFile_ << "  latency avg/p95/max ms: "
                << FormatDouble(summary.avgLatencyMs) << " / "
                << FormatDouble(summary.p95LatencyMs) << " / "
                << FormatDouble(summary.maxLatencyMs) << "\n";
            textFile_ << "  fps display avg/min: "
                << FormatDouble(summary.avgDisplayFps) << " / "
                << FormatDouble(summary.minDisplayFps) << "\n";
            textFile_ << "  drops deadline/output/total: "
                << summary.deadlineDroppedFrames << " / "
                << summary.outputQueueDroppedFrames << " / "
                << summary.droppedFrames << "\n";
            textFile_ << "  output drop events/burst/maxAge/reason: "
                << summary.outputQueueDropEvents << " / "
                << summary.outputQueueDropBurstEvents << " / "
                << FormatDouble(summary.maxOutputQueueDropOldestAgeMs)
                << " ms / "
                << (summary.lastOutputQueueDropReason.empty()
                    ? "none"
                    : summary.lastOutputQueueDropReason)
                << "\n";
            textFile_ << "  adaptive min fps/quality/bitrate: "
                << summary.minTargetFps << " / "
                << summary.minTargetJpegQuality << " / "
                << summary.minTargetBitrateKbps << "\n";
            textFile_ << "  network runtime mode: "
                << (summary.networkRuntimeMode.empty()
                    ? "unknown"
                    : summary.networkRuntimeMode)
                << "\n";
            textFile_ << "  adaptive control mode: "
                << (summary.adaptiveControlMode.empty()
                    ? "unknown"
                    : summary.adaptiveControlMode)
                << "\n";
            textFile_ << "  congestion control mode: "
                << (summary.adaptiveCongestionControlMode.empty()
                    ? "unknown"
                    : summary.adaptiveCongestionControlMode)
                << "\n";
            textFile_ << "  adaptive dominant cause: "
                << (summary.dominantAdaptiveDegradationCause.empty()
                    ? "None"
                    : summary.dominantAdaptiveDegradationCause)
                << "\n";
            textFile_ << "  deadline nack sent/recovered/missingChunks: "
                << summary.deadlineNackSentFrames << " / "
                << summary.deadlineNackRecoveredFrames << " / "
                << summary.deadlineNackMissingChunks << "\n";
            textFile_ << "  deadline nack expired drops/afterNack/missingChunks: "
                << summary.deadlineNackExpiredDroppedFrames << " / "
                << summary.deadlineNackExpiredAfterNackFrames << " / "
                << summary.deadlineNackExpiredMissingChunks << "\n";
            textFile_ << "  fec enabled/adaptive/group parity/recoveredFrames/recoveredChunks: "
                << (summary.fecEnabled ? "on" : "off") << " / "
                << (summary.adaptiveFecEnabled ? "on" : "off") << " / "
                << summary.fecGroupChunkCount << " / "
                << summary.fecParityPackets << " / "
                << summary.fecRecoveredFrames << " / "
                << summary.fecRecoveredChunks << "\n";
            textFile_ << "  adaptive fec guard working/active/avgEff/delta parity/recoveredFrames/recoveredChunks: "
                << summary.adaptiveFecRecoveryWorkingSamples << " / "
                << summary.adaptiveFecGuardActiveSamples << " / "
                << FormatPercent(summary.avgAdaptiveFecRecoveryEfficiency)
                << " / "
                << summary.adaptiveFecParityPacketDeltas << " / "
                << summary.adaptiveFecRecoveredFrameDeltas << " / "
                << summary.adaptiveFecRecoveredChunkDeltas << "\n";
            textFile_ << "  verdict: " << summary.verdict;
            if (!summary.notes.empty()) {
                textFile_ << " (" << summary.notes << ")";
            }
            textFile_ << "\n\n";
            textFile_.flush();
        }
    }

    void NetworkExperimentReporter::WriteMarkdownReport() const {
        if (markdownFilePath_.empty()) {
            return;
        }

        std::ofstream file(markdownFilePath_, std::ios::out | std::ios::trunc);
        if (!file) {
            return;
        }

        uint32_t passCount = 0;
        uint64_t totalDeadlineDrops = 0;
        uint64_t totalOutputQueueDrops = 0;
        uint64_t totalDeadlineNacks = 0;
        uint64_t totalDeadlineNackRecoveries = 0;
        uint64_t totalDeadlineNackMissingChunks = 0;
        uint64_t totalDeadlineNackExpiredDrops = 0;
        uint64_t totalFecParityPackets = 0;
        uint64_t totalFecRecoveredFrames = 0;
        uint64_t totalFecRecoveredChunks = 0;
        uint32_t totalAdaptiveFecGuardActiveSamples = 0;
        uint32_t totalAdaptiveFecRecoveryWorkingSamples = 0;
        double worstP95LatencyMs = 0.0;

        for (const ScenarioSummary& summary : summaries_) {
            if (summary.verdict == "PASS") {
                passCount++;
            }
            totalDeadlineDrops += summary.deadlineDroppedFrames;
            totalOutputQueueDrops += summary.outputQueueDroppedFrames;
            totalDeadlineNacks += summary.deadlineNackSentFrames;
            totalDeadlineNackRecoveries += summary.deadlineNackRecoveredFrames;
            totalDeadlineNackMissingChunks += summary.deadlineNackMissingChunks;
            totalDeadlineNackExpiredDrops +=
                summary.deadlineNackExpiredDroppedFrames;
            totalFecParityPackets += summary.fecParityPackets;
            totalFecRecoveredFrames += summary.fecRecoveredFrames;
            totalFecRecoveredChunks += summary.fecRecoveredChunks;
            totalAdaptiveFecGuardActiveSamples +=
                summary.adaptiveFecGuardActiveSamples;
            totalAdaptiveFecRecoveryWorkingSamples +=
                summary.adaptiveFecRecoveryWorkingSamples;
            worstP95LatencyMs =
                (std::max)(worstP95LatencyMs, summary.p95LatencyMs);
        }

        file << "# Network Experiment Report\n\n";
        file << "Generated: " << generatedTimestamp_ << "\n\n";
        file << "Runtime mode note: Loopback runs sender and receiver in one process for development experiments; Sender transmits camera video only; Receiver rebuilds and displays RNVP frames; Monitor keeps telemetry/reporting visible without sending or displaying video.\n\n";

        file << "## Executive Summary\n\n";
        if (summaries_.empty()) {
            file << "- No completed scenarios yet. The report will be refreshed as scenarios finish.\n\n";
        }
        else {
            const uint32_t warnCount =
                static_cast<uint32_t>(summaries_.size()) - passCount;
            file << "- Completed scenarios: " << summaries_.size()
                << " (" << passCount << " PASS, "
                << warnCount << " WARN).\n";
            file << "- Worst p95 latency: "
                << FormatDouble(worstP95LatencyMs)
                << " ms against the 150 ms display deadline.\n";
            file << "- Deadline drops: " << totalDeadlineDrops
                << ", output queue drops: " << totalOutputQueueDrops << ".\n";
            file << "- NACK recovery expired drops: "
                << totalDeadlineNackExpiredDrops
                << " frames were discarded instead of recovering stale video.\n";

            if (totalDeadlineNacks > 0) {
                const double recoveryRatio =
                    static_cast<double>(totalDeadlineNackRecoveries) /
                    static_cast<double>(totalDeadlineNacks);
                file << "- Selective retransmit recovered "
                    << totalDeadlineNackRecoveries
                    << " incomplete frames after "
                    << totalDeadlineNacks
                    << " deadline NACK events ("
                    << FormatPercent(recoveryRatio)
                    << " frame-level recovery evidence, "
                    << totalDeadlineNackMissingChunks
                    << " missing chunk requests).\n";
            }
            else {
                file << "- Selective retransmit was not needed in completed scenarios.\n";
            }
            file << "- FEC sent " << totalFecParityPackets
                << " parity packets and recovered "
                << totalFecRecoveredFrames << " frames / "
                << totalFecRecoveredChunks
                << " chunks across completed scenarios.\n";
            file << "- Adaptive FEC guard was active for "
                << totalAdaptiveFecGuardActiveSamples
                << " measured samples, with recovery-working evidence in "
                << totalAdaptiveFecRecoveryWorkingSamples
                << " samples.\n";
            file << "- Evaluation window: the first "
                << FormatDouble(summaries_.front().warmupSec)
                << " sec of each scenario is warmup and excluded from "
                << "summary averages, scoring, and cumulative counters.\n";
            file << "\n";
        }

        file << "## Scenario Results\n\n";
        file
            << "| Scenario | Verdict | Runtime | Adaptive Mode | Congestion Mode | FEC | Cause | Warmup s | Measured Samples | Avg FPS | Min FPS | Avg Latency ms | P95 Latency ms | Deadline Drops | Output Drops | NACK Sent | NACK Recovered | FEC Recovered | FEC Guard | NACK Expired | Notes |\n"
            << "| --- | --- | --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |\n";

        for (const ScenarioSummary& summary : summaries_) {
            file << "| "
                << EscapeMarkdownTable(summary.name) << " | "
                << summary.verdict << " | "
                << EscapeMarkdownTable(
                    summary.networkRuntimeMode.empty()
                    ? "unknown"
                    : summary.networkRuntimeMode) << " | "
                << EscapeMarkdownTable(
                    summary.adaptiveControlMode.empty()
                    ? "unknown"
                    : summary.adaptiveControlMode) << " | "
                << EscapeMarkdownTable(
                    summary.adaptiveCongestionControlMode.empty()
                    ? "unknown"
                    : summary.adaptiveCongestionControlMode) << " | "
                << EscapeMarkdownTable(
                    summary.adaptiveFecEnabled
                    ? "adaptive"
                    : (summary.fecEnabled
                        ? "g" + std::to_string(summary.fecGroupChunkCount)
                        : "off")) << " | "
                << EscapeMarkdownTable(
                    summary.dominantAdaptiveDegradationCause.empty()
                    ? "None"
                    : summary.dominantAdaptiveDegradationCause) << " | "
                << FormatDouble(summary.warmupSec) << " | "
                << summary.measuredSampleCount << " | "
                << FormatDouble(summary.avgDisplayFps) << " | "
                << FormatDouble(summary.minDisplayFps) << " | "
                << FormatDouble(summary.avgLatencyMs) << " | "
                << FormatDouble(summary.p95LatencyMs) << " | "
                << summary.deadlineDroppedFrames << " | "
                << summary.outputQueueDroppedFrames << " | "
                << summary.deadlineNackSentFrames << " | "
                << summary.deadlineNackRecoveredFrames << " | "
                << summary.fecRecoveredFrames << " | "
                << FormatPercent(summary.adaptiveFecGuardActiveRatio)
                << " | "
                << summary.deadlineNackExpiredDroppedFrames << " | "
                << EscapeMarkdownTable(
                    summary.notes.empty() ? "none" : summary.notes)
                << " |\n";
        }
        file << "\n";

        file << "## Controller A/B Comparison\n\n";
        if (summaries_.empty()) {
            file << "No controller comparison is available yet.\n\n";
        }
        else {
            std::vector<std::string> networkScenarioOrder;
            std::unordered_map<std::string, std::vector<const ScenarioSummary*>>
                summariesByNetworkScenario;

            for (const ScenarioSummary& summary : summaries_) {
                const std::string networkScenario =
                    ExtractNetworkScenarioName(summary.name);
                if (summariesByNetworkScenario.find(networkScenario) ==
                    summariesByNetworkScenario.end()) {
                    networkScenarioOrder.push_back(networkScenario);
                }
                summariesByNetworkScenario[networkScenario].push_back(&summary);
            }

            const auto modeName =
                [](const ScenarioSummary& summary) -> std::string {
                const std::string adaptiveMode =
                    summary.adaptiveControlMode.empty()
                    ? std::string("unknown")
                    : summary.adaptiveControlMode;
                if (adaptiveMode == "QoE/Deadline Adaptive" &&
                    !summary.adaptiveCongestionControlMode.empty()) {
                    return adaptiveMode + " / " +
                        summary.adaptiveCongestionControlMode;
                }
                return adaptiveMode;
            };

            const auto isFixedQuality =
                [&](const ScenarioSummary& summary) -> bool {
                return modeName(summary) == "Fixed Quality" ||
                    summary.name.find(" / Fixed Quality") !=
                    std::string::npos;
            };

            const auto scoreController =
                [&](const ScenarioSummary& summary) -> double {
                return BuildScoreBreakdown(summary, true, true).finalScore;
            };

            file
                << "| Network Scenario | Controller | Frame Drop | Drop vs Fixed | Avg Latency ms | Latency vs Fixed | Display FPS | FPS vs Fixed | Avg QoE | FEC Eff | FEC Guard | FEC Guard Eff | FEC Rec | FEC Parity | Quality Penalty | Final Score | NACK Recovered | NACK Expired | Target FPS | Target JPEG | Target Bitrate kbps | Verdict |\n"
                << "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |\n";

            for (const std::string& networkScenario : networkScenarioOrder) {
                const auto it =
                    summariesByNetworkScenario.find(networkScenario);
                if (it == summariesByNetworkScenario.end()) {
                    continue;
                }

                const ScenarioSummary* fixedBaseline = nullptr;
                for (const ScenarioSummary* summary : it->second) {
                    if (isFixedQuality(*summary)) {
                        fixedBaseline = summary;
                        break;
                    }
                }

                for (const ScenarioSummary* summary : it->second) {
                    const double dropRate = FrameDropRate(*summary);
                    const ScoreBreakdown score =
                        BuildScoreBreakdown(*summary, true, true);
                    const double avgQoeScore = AverageQoeScore(*summary);
                    const std::string dropVsFixed = fixedBaseline == nullptr
                        ? "n/a"
                        : FormatImprovementRatio(
                            FrameDropRate(*fixedBaseline),
                            dropRate,
                            false);
                    const std::string latencyVsFixed = fixedBaseline == nullptr
                        ? "n/a"
                        : FormatImprovementRatio(
                            fixedBaseline->avgLatencyMs,
                            summary->avgLatencyMs,
                            false);
                    const std::string fpsVsFixed = fixedBaseline == nullptr
                        ? "n/a"
                        : FormatImprovementRatio(
                            fixedBaseline->avgDisplayFps,
                            summary->avgDisplayFps,
                            true);

                    file << "| "
                        << EscapeMarkdownTable(networkScenario) << " | "
                        << EscapeMarkdownTable(modeName(*summary)) << " | "
                        << FormatPercent(dropRate) << " | "
                        << dropVsFixed << " | "
                        << FormatDouble(summary->avgLatencyMs) << " | "
                        << latencyVsFixed << " | "
                        << FormatDouble(summary->avgDisplayFps) << " | "
                        << fpsVsFixed << " | "
                        << FormatDouble(avgQoeScore) << " | "
                        << FormatPercent(FecRecoveryEfficiency(*summary))
                        << " | "
                        << FormatPercent(summary->adaptiveFecGuardActiveRatio)
                        << " | "
                        << FormatPercent(
                            summary->avgAdaptiveFecRecoveryEfficiency)
                        << " | "
                        << summary->fecRecoveredFrames << " | "
                        << summary->fecParityPackets << " | "
                        << FormatDouble(score.qualityPenalty) << " | "
                        << FormatDouble(score.finalScore) << " | "
                        << summary->deadlineNackRecoveredFrames << " | "
                        << summary->deadlineNackExpiredDroppedFrames << " | "
                        << summary->minTargetFps << " | "
                        << summary->minTargetJpegQuality << " | "
                        << summary->minTargetBitrateKbps << " | "
                        << summary->verdict << " |\n";
                }
            }

            file << "\n";
            file << "### Controller Score Breakdown\n\n";
            file << "Lower final score wins. `FPS Bonus`, `QoE Bonus`, and `FEC Recovery Bonus` are positive values that are subtracted from the final score; the other columns add cost. `Frame Drop Penalty` combines drop rate, dropped frames, and deadline drops. FEC overhead penalizes parity traffic, while FEC inefficiency penalizes parity streams that recover too few frames.\n\n";
            file
                << "| Network Scenario | Controller | Latency Score | Frame Drop Penalty | Output Queue Penalty | NACK Expire Penalty | FEC Overhead | FEC Inefficiency | FEC Recovery Bonus | FPS Bonus | QoE Bonus | Quality Penalty | Final Score |\n"
                << "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";

            for (const std::string& networkScenario : networkScenarioOrder) {
                const auto it =
                    summariesByNetworkScenario.find(networkScenario);
                if (it == summariesByNetworkScenario.end()) {
                    continue;
                }

                for (const ScenarioSummary* summary : it->second) {
                    const ScoreBreakdown score =
                        BuildScoreBreakdown(*summary, true, true);
                    file << "| "
                        << EscapeMarkdownTable(networkScenario) << " | "
                        << EscapeMarkdownTable(modeName(*summary)) << " | "
                        << FormatDouble(score.latencyScore) << " | "
                        << FormatDouble(score.frameDropPenalty) << " | "
                        << FormatDouble(score.outputQueuePenalty) << " | "
                        << FormatDouble(score.nackExpirePenalty) << " | "
                        << FormatDouble(score.fecOverheadPenalty) << " | "
                        << FormatDouble(score.fecInefficiencyPenalty) << " | "
                        << FormatDouble(score.fecRecoveryBonus) << " | "
                        << FormatDouble(score.fpsBonus) << " | "
                        << FormatDouble(score.qoeBonus) << " | "
                        << FormatDouble(score.qualityPenalty) << " | "
                        << FormatDouble(score.finalScore) << " |\n";
                }
            }

            file << "\n";

            file << "### Best Controller By Scenario\n\n";
            file
                << "| Network Scenario | Best Controller | Why It Wins | Fixed Baseline | Evidence |\n"
                << "| --- | --- | --- | --- | --- |\n";

            for (const std::string& networkScenario : networkScenarioOrder) {
                const auto it =
                    summariesByNetworkScenario.find(networkScenario);
                if (it == summariesByNetworkScenario.end() ||
                    it->second.empty()) {
                    continue;
                }

                const ScenarioSummary* fixedBaseline = nullptr;
                const ScenarioSummary* winner = it->second.front();
                double winnerScore = scoreController(*winner);

                for (const ScenarioSummary* candidate : it->second) {
                    if (isFixedQuality(*candidate)) {
                        fixedBaseline = candidate;
                    }

                    const double candidateScore =
                        scoreController(*candidate);
                    if (candidateScore < winnerScore) {
                        winner = candidate;
                        winnerScore = candidateScore;
                    }
                }

                std::string why = "best latency/drop/FPS balance";
                if (winner->p95LatencyMs < 150.0 &&
                    FrameDropRate(*winner) <= 0.01) {
                    why = "kept p95 under 150ms with almost no frame drops";
                }
                else if (fixedBaseline != nullptr &&
                    FrameDropRate(*winner) < FrameDropRate(*fixedBaseline) &&
                    winner->avgDisplayFps >= fixedBaseline->avgDisplayFps) {
                    why = "reduced drops without sacrificing display FPS";
                }
                else if (winner->deadlineNackRecoveredFrames > 0 &&
                    winner->deadlineNackExpiredDroppedFrames <=
                    winner->deadlineNackRecoveredFrames) {
                    why = "recovered missing frames while bounding stale recovery";
                }

                std::string fixedBaselineText = "not available";
                std::string evidence = "score "
                    + FormatDouble(winnerScore) +
                    ", quality penalty " +
                    FormatDouble(QualityFloorPenalty(*winner));
                if (fixedBaseline != nullptr) {
                    fixedBaselineText =
                        FormatPercent(FrameDropRate(*fixedBaseline)) +
                        " drop, " +
                        FormatDouble(fixedBaseline->avgLatencyMs) +
                        " ms latency, " +
                        FormatDouble(fixedBaseline->avgDisplayFps) +
                        " fps";
                    evidence =
                        "drop " +
                        FormatImprovementRatio(
                            FrameDropRate(*fixedBaseline),
                            FrameDropRate(*winner),
                            false) +
                        ", latency " +
                        FormatImprovementRatio(
                            fixedBaseline->avgLatencyMs,
                            winner->avgLatencyMs,
                            false) +
                        ", fps " +
                        FormatImprovementRatio(
                            fixedBaseline->avgDisplayFps,
                            winner->avgDisplayFps,
                            true) +
                        " vs Fixed, quality penalty " +
                        FormatDouble(QualityFloorPenalty(*winner));
                }

                file << "| "
                    << EscapeMarkdownTable(networkScenario) << " | "
                    << EscapeMarkdownTable(modeName(*winner)) << " | "
                    << EscapeMarkdownTable(why) << " | "
                    << EscapeMarkdownTable(fixedBaselineText) << " | "
                    << EscapeMarkdownTable(evidence) << " |\n";
            }

            file << "\n";
            file << "### Congestion Control A/B Comparison\n\n";
            file << "This table compares `Loss Based`, `Delay Based`, and `Hybrid` under the same network scenario using only `QoE/Deadline Adaptive` runs.\n\n";
            file
                << "| Network Scenario | Congestion Mode | Frame Drop | Avg Latency ms | P95 Latency ms | Display FPS | Avg QoE | Score |\n"
                << "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |\n";

            for (const std::string& networkScenario : networkScenarioOrder) {
                const auto it =
                    summariesByNetworkScenario.find(networkScenario);
                if (it == summariesByNetworkScenario.end()) {
                    continue;
                }

                for (const ScenarioSummary* summary : it->second) {
                    if (summary->adaptiveControlMode !=
                        "QoE/Deadline Adaptive") {
                        continue;
                    }

                    file << "| "
                        << EscapeMarkdownTable(networkScenario) << " | "
                        << EscapeMarkdownTable(
                            summary->adaptiveCongestionControlMode.empty()
                            ? "unknown"
                            : summary->adaptiveCongestionControlMode) << " | "
                        << FormatPercent(FrameDropRate(*summary)) << " | "
                        << FormatDouble(summary->avgLatencyMs) << " | "
                        << FormatDouble(summary->p95LatencyMs) << " | "
                        << FormatDouble(summary->avgDisplayFps) << " | "
                        << FormatDouble(AverageQoeScore(*summary)) << " | "
                        << FormatDouble(scoreController(*summary)) << " |\n";
                }
            }

            file << "\n";
            file << "### Best Congestion Mode By Scenario\n\n";
            file
                << "| Network Scenario | Best Congestion Mode | Why It Wins | Evidence |\n"
                << "| --- | --- | --- | --- |\n";

            for (const std::string& networkScenario : networkScenarioOrder) {
                const auto it =
                    summariesByNetworkScenario.find(networkScenario);
                if (it == summariesByNetworkScenario.end()) {
                    continue;
                }

                const ScenarioSummary* winner = nullptr;
                double winnerScore = 0.0;
                for (const ScenarioSummary* candidate : it->second) {
                    if (candidate->adaptiveControlMode !=
                        "QoE/Deadline Adaptive") {
                        continue;
                    }

                    const double candidateScore =
                        scoreController(*candidate);
                    if (winner == nullptr || candidateScore < winnerScore) {
                        winner = candidate;
                        winnerScore = candidateScore;
                    }
                }

                if (winner == nullptr) {
                    continue;
                }

                std::string why = "best latency/drop/FPS/QoE balance";
                if (winner->adaptiveCongestionControlMode == "Loss Based") {
                    why = "loss-driven AIMD gave the best score";
                }
                else if (winner->adaptiveCongestionControlMode == "Delay Based") {
                    why = "delay-driven AIMD avoided queue growth best";
                }
                else if (winner->adaptiveCongestionControlMode == "Hybrid") {
                    why = "hybrid transport and QoE signals gave the best score";
                }

                const std::string evidence =
                    "drop " + FormatPercent(FrameDropRate(*winner)) +
                    ", p95 " + FormatDouble(winner->p95LatencyMs) +
                    " ms, fps " + FormatDouble(winner->avgDisplayFps) +
                    ", score " + FormatDouble(winnerScore);

                file << "| "
                    << EscapeMarkdownTable(networkScenario) << " | "
                    << EscapeMarkdownTable(
                        winner->adaptiveCongestionControlMode.empty()
                        ? "unknown"
                        : winner->adaptiveCongestionControlMode) << " | "
                    << EscapeMarkdownTable(why) << " | "
                    << EscapeMarkdownTable(evidence) << " |\n";
            }

            file << "\n";
            file << "This section is the controller A/B evidence path: Fixed Quality is the uncontrolled baseline, Loss Reactive is packet-loss-only adaptation, and QoE/Deadline Adaptive is evaluated against the same network condition using drop rate, latency, display FPS, QoE, and recovery events.\n\n";
        }

        file << "## Time-Series Controller Response\n\n";
        if (summaries_.empty()) {
            file << "No time-series samples are available yet.\n\n";
        }
        else {
            constexpr size_t kMaxChartSamples = 30;

            const auto shouldPreferScenario =
                [](const ScenarioSummary& summary) -> bool {
                return
                    summary.name == "Baseline / Fixed Quality" ||
                    summary.name == "10% loss / Fixed Quality" ||
                    summary.name == "10% loss / QoE/Deadline Adaptive / Hybrid" ||
                    summary.name == "50ms jitter / QoE/Deadline Adaptive / Delay Based" ||
                    summary.name == "Burst loss / QoE/Deadline Adaptive / Loss Based" ||
                    summary.name == "Burst loss / QoE/Deadline Adaptive / Hybrid";
            };

            std::vector<const ScenarioSummary*> chartSummaries;
            for (const ScenarioSummary& summary : summaries_) {
                if (!summary.timeSeriesSamples.empty() &&
                    shouldPreferScenario(summary)) {
                    chartSummaries.push_back(&summary);
                }
            }
            if (chartSummaries.empty()) {
                for (const ScenarioSummary& summary : summaries_) {
                    if (!summary.timeSeriesSamples.empty()) {
                        chartSummaries.push_back(&summary);
                    }
                    if (chartSummaries.size() >= 4) {
                        break;
                    }
                }
            }

            file << "Selected scenarios are capped at "
                << kMaxChartSamples
                << " points each so the Markdown stays readable while preserving controller reaction trends.\n\n";

            const auto buildNumberArray =
                [](const std::vector<double>& values) -> std::string {
                std::ostringstream oss;
                oss << "[";
                for (size_t i = 0; i < values.size(); ++i) {
                    if (i > 0) {
                        oss << ", ";
                    }
                    oss << FormatDouble(values[i]);
                }
                oss << "]";
                return oss.str();
            };

            const auto collectSamples =
                [&](const ScenarioSummary& summary) {
                std::vector<const TimeSeriesSample*> samples;
                if (summary.timeSeriesSamples.empty()) {
                    return samples;
                }

                const size_t stride =
                    (std::max)(
                        size_t{ 1 },
                        (summary.timeSeriesSamples.size() +
                            kMaxChartSamples - 1) / kMaxChartSamples);

                for (size_t i = 0; i < summary.timeSeriesSamples.size(); i += stride) {
                    samples.push_back(&summary.timeSeriesSamples[i]);
                }

                if (samples.empty() ||
                    samples.back() != &summary.timeSeriesSamples.back()) {
                    samples.push_back(&summary.timeSeriesSamples.back());
                }

                return samples;
            };

            const auto writeChart =
                [&](const std::string& title,
                    const std::string& yLabel,
                    double yMax,
                    const std::vector<double>& xValues,
                    const std::vector<std::pair<std::string, std::vector<double>>>& lines) {
                if (xValues.empty() || lines.empty()) {
                    return;
                }

                file << "```mermaid\n";
                file << "xychart-beta\n";
                file << "  title \"" << EscapeMermaidText(title) << "\"\n";
                file << "  x-axis " << buildNumberArray(xValues) << "\n";
                file << "  y-axis \"" << EscapeMermaidText(yLabel)
                    << "\" 0 --> " << FormatDouble((std::max)(1.0, yMax)) << "\n";
                for (const auto& line : lines) {
                    file << "  line \"" << EscapeMermaidText(line.first)
                        << "\" " << buildNumberArray(line.second) << "\n";
                }
                file << "```\n\n";
            };

            for (const ScenarioSummary* summary : chartSummaries) {
                const std::vector<const TimeSeriesSample*> samples =
                    collectSamples(*summary);
                if (samples.empty()) {
                    continue;
                }

                std::vector<double> x;
                std::vector<double> displayFps;
                std::vector<double> currentLatency;
                std::vector<double> rollingP95Latency;
                std::vector<double> deadlineLine;
                std::vector<double> qoeScoreScaled;
                std::vector<double> causeScore;
                std::vector<double> targetQuality;
                std::vector<double> targetFps;
                std::vector<double> targetBitrateScaled;
                std::vector<double> bandwidthCeilingScaled;
                std::vector<double> estimatedBandwidthMbps;
                std::vector<double> deliveryRateMbps;
                std::vector<double> bandwidthQueueDelayMs;
                std::vector<double> bandwidthLossPercent;
                std::vector<double> bandwidthJitterMs;
                std::vector<double> nackSent;
                std::vector<double> nackRecovered;
                std::vector<double> nackExpired;
                std::vector<double> retransmittedChunks;
                std::vector<double> fecGuardActive;
                std::vector<double> fecRecoveryWorking;
                std::vector<double> fecRecoveryEfficiencyPercent;
                std::vector<double> packetLossPercent;
                std::vector<double> jitterMs;

                double recoveryMax = 1.0;
                double networkInputMax = 10.0;
                double bandwidthMax = 1.0;

                for (const TimeSeriesSample* sample : samples) {
                    x.push_back(sample->relativeTimeSec);
                    displayFps.push_back(sample->displayFps);
                    currentLatency.push_back(sample->currentLatencyMs);
                    rollingP95Latency.push_back(sample->rollingP95LatencyMs);
                    deadlineLine.push_back(150.0);
                    qoeScoreScaled.push_back(sample->adaptiveQoeScore * 20.0);
                    causeScore.push_back(sample->adaptiveCauseScore);
                    targetQuality.push_back(
                        static_cast<double>(sample->targetJpegQuality));
                    targetFps.push_back(static_cast<double>(sample->targetFps));
                    targetBitrateScaled.push_back(
                        static_cast<double>(sample->targetBitrateKbps) / 120.0);
                    bandwidthCeilingScaled.push_back(
                        static_cast<double>(sample->bandwidthCeilingKbps) / 120.0);
                    estimatedBandwidthMbps.push_back(
                        static_cast<double>(sample->estimatedBandwidthBps) /
                        1000.0 / 1000.0);
                    deliveryRateMbps.push_back(
                        static_cast<double>(sample->deliveryRateBps) /
                        1000.0 / 1000.0);
                    bandwidthQueueDelayMs.push_back(
                        sample->bandwidthQueueDelayMs);
                    bandwidthLossPercent.push_back(
                        sample->bandwidthLossTrend * 100.0);
                    bandwidthJitterMs.push_back(
                        sample->bandwidthJitterTrendMs);
                    nackSent.push_back(
                        static_cast<double>(sample->deadlineNackSentFrames));
                    nackRecovered.push_back(
                        static_cast<double>(sample->deadlineNackRecoveredFrames));
                    nackExpired.push_back(
                        static_cast<double>(sample->deadlineNackExpiredDroppedFrames));
                    retransmittedChunks.push_back(
                        static_cast<double>(sample->ackRetransmittedChunks));
                    fecGuardActive.push_back(
                        sample->adaptiveFecGuardActive ? 100.0 : 0.0);
                    fecRecoveryWorking.push_back(
                        sample->adaptiveFecRecoveryWorking ? 100.0 : 0.0);
                    fecRecoveryEfficiencyPercent.push_back(
                        sample->adaptiveFecRecoveryEfficiency * 100.0);
                    packetLossPercent.push_back(sample->packetLossRate * 100.0);
                    jitterMs.push_back(sample->currentJitterMs);

                    recoveryMax =
                        (std::max)(recoveryMax, nackSent.back());
                    recoveryMax =
                        (std::max)(recoveryMax, nackRecovered.back());
                    recoveryMax =
                        (std::max)(recoveryMax, nackExpired.back());
                    recoveryMax =
                        (std::max)(recoveryMax, retransmittedChunks.back());
                    networkInputMax =
                        (std::max)(networkInputMax, packetLossPercent.back());
                    networkInputMax =
                        (std::max)(networkInputMax, jitterMs.back());
                    bandwidthMax =
                        (std::max)(bandwidthMax, estimatedBandwidthMbps.back());
                    bandwidthMax =
                        (std::max)(bandwidthMax, deliveryRateMbps.back());
                    bandwidthMax =
                        (std::max)(bandwidthMax, bandwidthQueueDelayMs.back());
                    bandwidthMax =
                        (std::max)(bandwidthMax, bandwidthLossPercent.back());
                    bandwidthMax =
                        (std::max)(bandwidthMax, bandwidthJitterMs.back());
                }

                file << "### " << summary->name << "\n\n";
                file << "Adaptive cause score legend: None=0, Loss=20, Jitter=40, RTT=60, Bandwidth=80, DecodeLoad=100, DisplayLoad=120, FrameFreshness=140, RecoveryDeadline=160, PacingQueue=180. Target bitrate and bandwidth ceiling are drawn as `kbps / 120` to share the same axis as quality and FPS.\n\n";

                writeChart(
                    summary->name + " - FPS and Latency",
                    "FPS / ms",
                    160.0,
                    x,
                    {
                        { "Display FPS", displayFps },
                        { "Current latency ms", currentLatency },
                        { "Rolling p95 latency ms", rollingP95Latency },
                        { "150ms deadline", deadlineLine },
                    });

                writeChart(
                    summary->name + " - QoE and Adaptive Cause",
                    "Score",
                    120.0,
                    x,
                    {
                        { "QoE score x20", qoeScoreScaled },
                        { "Cause score", causeScore },
                    });

                writeChart(
                    summary->name + " - Target Quality, FPS, Bitrate",
                    "Control target",
                    100.0,
                    x,
                    {
                        { "JPEG quality", targetQuality },
                        { "Target FPS", targetFps },
                        { "Bitrate kbps / 120", targetBitrateScaled },
                        { "BW ceiling kbps / 120", bandwidthCeilingScaled },
                    });

                writeChart(
                    summary->name + " - Bandwidth Estimator",
                    "Mbps / ms / %",
                    bandwidthMax + 2.0,
                    x,
                    {
                        { "Estimated bandwidth Mbps", estimatedBandwidthMbps },
                        { "Delivery rate Mbps", deliveryRateMbps },
                        { "Queue delay ms", bandwidthQueueDelayMs },
                        { "Loss trend %", bandwidthLossPercent },
                        { "Jitter trend ms", bandwidthJitterMs },
                    });

                writeChart(
                    summary->name + " - Recovery Events",
                    "Count",
                    recoveryMax + 1.0,
                    x,
                    {
                        { "Deadline NACK sent", nackSent },
                        { "NACK recovered", nackRecovered },
                        { "NACK expired", nackExpired },
                        { "Retransmitted chunks", retransmittedChunks },
                    });

                writeChart(
                    summary->name + " - Adaptive FEC Guard",
                    "Percent / on-off",
                    100.0,
                    x,
                    {
                        { "FEC guard active", fecGuardActive },
                        { "FEC recovery working", fecRecoveryWorking },
                        { "FEC recovery efficiency %", fecRecoveryEfficiencyPercent },
                    });

                writeChart(
                    summary->name + " - Packet Loss and Jitter",
                    "Loss % / ms",
                    networkInputMax + 5.0,
                    x,
                    {
                        { "Packet loss %", packetLossPercent },
                        { "Jitter ms", jitterMs },
                    });
            }
        }

        file << "## Baseline Comparison\n\n";
        if (summaries_.empty()) {
            file << "No baseline is available yet.\n\n";
        }
        else {
            const ScenarioSummary& baseline = summaries_.front();
            file
                << "| Scenario | Mode | FPS Delta | P95 Latency Delta ms | Drop Delta | Adaptive Min FPS | Adaptive Min JPEG Quality |\n"
                << "| --- | --- | ---: | ---: | ---: | ---: | ---: |\n";

            for (const ScenarioSummary& summary : summaries_) {
                const double fpsDelta =
                    summary.avgDisplayFps - baseline.avgDisplayFps;
                const double p95Delta =
                    summary.p95LatencyMs - baseline.p95LatencyMs;
                const int64_t dropDelta =
                    static_cast<int64_t>(summary.droppedFrames) -
                    static_cast<int64_t>(baseline.droppedFrames);

                file << "| "
                    << EscapeMarkdownTable(summary.name) << " | "
                    << EscapeMarkdownTable(
                        summary.adaptiveControlMode.empty()
                        ? "unknown"
                        : summary.adaptiveControlMode) << " | "
                    << FormatDouble(fpsDelta) << " | "
                    << FormatDouble(p95Delta) << " | "
                    << dropDelta << " | "
                    << summary.minTargetFps << " | "
                    << summary.minTargetJpegQuality << " |\n";
            }
            file << "\n";
        }

        file << "## Automatic Diagnosis\n\n";
        if (summaries_.empty()) {
            file << "- Waiting for experiment data.\n";
        }
        else {
            for (const ScenarioSummary& summary : summaries_) {
                file << "- " << summary.name << ": ";
                if (summary.verdict == "PASS") {
                    file << "deadline and QoE targets stayed healthy";
                }
                else {
                    file << summary.notes;
                }

                if (summary.deadlineNackSentFrames > 0) {
                    file << "; deadline NACK recovered "
                        << summary.deadlineNackRecoveredFrames
                        << " frames from "
                        << summary.deadlineNackSentFrames
                        << " NACK events";
                }

                if (summary.deadlineNackExpiredDroppedFrames > 0) {
                    file << "; "
                        << summary.deadlineNackExpiredDroppedFrames
                        << " stale incomplete frames expired before display deadline";
                }

                if (summary.outputQueueDroppedFrames > 0 &&
                    !summary.lastOutputQueueDropReason.empty()) {
                    file << "; last output-drop reason was "
                        << summary.lastOutputQueueDropReason;
                }

                if (!summary.dominantAdaptiveDegradationCause.empty() &&
                    summary.dominantAdaptiveDegradationCause != "None") {
                    file << "; dominant adaptive cause was "
                        << summary.dominantAdaptiveDegradationCause;
                }

                file << ".\n";
            }
        }

        file << "\n## Interview Summary\n\n";
        if (summaries_.empty()) {
            file << "- Waiting for completed scenarios.\n\n";
        }
        else {
            std::vector<std::string> networkScenarioOrder;
            std::unordered_map<std::string, std::vector<const ScenarioSummary*>>
                summariesByNetworkScenario;

            for (const ScenarioSummary& summary : summaries_) {
                const std::string networkScenario =
                    ExtractNetworkScenarioName(summary.name);
                if (summariesByNetworkScenario.find(networkScenario) ==
                    summariesByNetworkScenario.end()) {
                    networkScenarioOrder.push_back(networkScenario);
                }
                summariesByNetworkScenario[networkScenario].push_back(&summary);
            }

            const auto modeName =
                [](const ScenarioSummary& summary) -> std::string {
                const std::string adaptiveMode =
                    summary.adaptiveControlMode.empty()
                    ? std::string("unknown")
                    : summary.adaptiveControlMode;
                if (adaptiveMode == "QoE/Deadline Adaptive" &&
                    !summary.adaptiveCongestionControlMode.empty()) {
                    return adaptiveMode + " / " +
                        summary.adaptiveCongestionControlMode;
                }
                return adaptiveMode;
            };

            const auto isFixedQuality =
                [&](const ScenarioSummary& summary) -> bool {
                return modeName(summary) == "Fixed Quality" ||
                    summary.name.find(" / Fixed Quality") !=
                    std::string::npos;
            };

            const auto scoreController =
                [&](const ScenarioSummary& summary) -> double {
                return BuildScoreBreakdown(summary, false, true).finalScore;
            };

            uint32_t comparableScenarioCount = 0;
            uint32_t adaptiveWinCount = 0;
            double bestDropImprovement = -std::numeric_limits<double>::infinity();
            double bestLatencyImprovement = -std::numeric_limits<double>::infinity();
            double bestFpsImprovement = -std::numeric_limits<double>::infinity();
            std::string bestDropScenario;
            std::string bestLatencyScenario;
            std::string bestFpsScenario;

            for (const std::string& networkScenario : networkScenarioOrder) {
                const auto it =
                    summariesByNetworkScenario.find(networkScenario);
                if (it == summariesByNetworkScenario.end() ||
                    it->second.empty()) {
                    continue;
                }

                const ScenarioSummary* fixedBaseline = nullptr;
                const ScenarioSummary* winner = it->second.front();
                double winnerScore = scoreController(*winner);

                for (const ScenarioSummary* candidate : it->second) {
                    if (isFixedQuality(*candidate)) {
                        fixedBaseline = candidate;
                    }

                    const double candidateScore =
                        scoreController(*candidate);
                    if (candidateScore < winnerScore) {
                        winner = candidate;
                        winnerScore = candidateScore;
                    }
                }

                if (fixedBaseline == nullptr) {
                    continue;
                }

                comparableScenarioCount++;
                if (!isFixedQuality(*winner)) {
                    adaptiveWinCount++;
                }

                const double fixedDropRate = FrameDropRate(*fixedBaseline);
                const double winnerDropRate = FrameDropRate(*winner);
                const double dropImprovement =
                    std::abs(fixedDropRate) < 0.0001
                    ? 0.0
                    : (fixedDropRate - winnerDropRate) / fixedDropRate;
                const double latencyImprovement =
                    std::abs(fixedBaseline->avgLatencyMs) < 0.0001
                    ? 0.0
                    : (fixedBaseline->avgLatencyMs - winner->avgLatencyMs) /
                    fixedBaseline->avgLatencyMs;
                const double fpsImprovement =
                    std::abs(fixedBaseline->avgDisplayFps) < 0.0001
                    ? 0.0
                    : (winner->avgDisplayFps -
                        fixedBaseline->avgDisplayFps) /
                    fixedBaseline->avgDisplayFps;

                if (dropImprovement > bestDropImprovement) {
                    bestDropImprovement = dropImprovement;
                    bestDropScenario =
                        networkScenario + " / " + modeName(*winner);
                }
                if (latencyImprovement > bestLatencyImprovement) {
                    bestLatencyImprovement = latencyImprovement;
                    bestLatencyScenario =
                        networkScenario + " / " + modeName(*winner);
                }
                if (fpsImprovement > bestFpsImprovement) {
                    bestFpsImprovement = fpsImprovement;
                    bestFpsScenario =
                        networkScenario + " / " + modeName(*winner);
                }
            }

            if (comparableScenarioCount == 0) {
                file << "- Fixed Quality baseline was not found for the completed scenarios. Run the Adaptive A/B experiment so each network condition has Fixed Quality, Loss Reactive, and QoE/Deadline Adaptive rows.\n\n";
            }
            else {
                file << "- Compared " << comparableScenarioCount
                    << " network scenarios against Fixed Quality baselines.\n";
                file << "- Non-fixed adaptive controllers won "
                    << adaptiveWinCount << " / "
                    << comparableScenarioCount
                    << " comparable scenarios by the latency/drop/FPS score.\n";
                file << "- Best frame-drop reduction: "
                    << FormatSignedPercent(bestDropImprovement)
                    << " at " << bestDropScenario << ".\n";
                file << "- Best average-latency improvement: "
                    << FormatSignedPercent(bestLatencyImprovement)
                    << " at " << bestLatencyScenario << ".\n";
                file << "- Best display-FPS improvement: "
                    << FormatSignedPercent(bestFpsImprovement)
                    << " at " << bestFpsScenario << ".\n";
                file << "- Interview framing: under the same network condition, Fixed Quality is the baseline and the adaptive modes are judged by user-visible outcomes: deadline latency, displayed FPS, frame drop rate, QoE, and selective retransmit recovery.\n\n";
                file << "- Quality guardrail: modes that fall below configured FPS, JPEG quality, or bitrate floors receive extra score penalties so low-quality escape routes do not win by latency alone. The exact weights and thresholds are saved in the experiment manifest.\n\n";
            }
        }

        file << "\n## Portfolio Summary\n\n";
        file << "This engine now covers implementation, measurement, evaluation, and improvement in one loop: CSV telemetry records raw runtime behavior, scenario summaries condense each network condition, and this report converts the run into reviewable evidence. Deadline-based NACK and selective retransmission preserve UDP latency while recovering missing chunks when a frame misses its receive deadline.\n";
        file.flush();
    }

    void NetworkExperimentReporter::WriteBeforeAfterReport() const {
        if (beforeAfterFilePath_.empty()) {
            return;
        }

        std::ofstream file(
            beforeAfterFilePath_,
            std::ios::out | std::ios::trunc
        );
        if (!file) {
            return;
        }

        const std::vector<ScenarioSummary> beforeSummaries =
            previousSummaryCsvPath_.empty()
            ? std::vector<ScenarioSummary>{}
            : LoadSummaryCsv(previousSummaryCsvPath_);

        std::unordered_map<std::string, ScenarioSummary> beforeByName;
        for (const ScenarioSummary& summary : beforeSummaries) {
            beforeByName[summary.name] = summary;
        }

        struct ScenarioPair {
            ScenarioSummary before;
            ScenarioSummary after;
        };

        std::vector<ScenarioPair> matched;
        for (const ScenarioSummary& after : summaries_) {
            const auto it = beforeByName.find(after.name);
            if (it != beforeByName.end()) {
                matched.push_back({ it->second, after });
            }
        }

        double beforeFpsSum = 0.0;
        double afterFpsSum = 0.0;
        double beforeP95Sum = 0.0;
        double afterP95Sum = 0.0;
        uint64_t beforeDrops = 0;
        uint64_t afterDrops = 0;
        uint64_t beforeDeadlineDrops = 0;
        uint64_t afterDeadlineDrops = 0;
        uint64_t beforeOutputDrops = 0;
        uint64_t afterOutputDrops = 0;
        uint64_t beforeNackRecovered = 0;
        uint64_t afterNackRecovered = 0;
        uint64_t beforeNackExpired = 0;
        uint64_t afterNackExpired = 0;

        for (const ScenarioPair& pair : matched) {
            beforeFpsSum += pair.before.avgDisplayFps;
            afterFpsSum += pair.after.avgDisplayFps;
            beforeP95Sum += pair.before.p95LatencyMs;
            afterP95Sum += pair.after.p95LatencyMs;
            beforeDrops += pair.before.droppedFrames;
            afterDrops += pair.after.droppedFrames;
            beforeDeadlineDrops += pair.before.deadlineDroppedFrames;
            afterDeadlineDrops += pair.after.deadlineDroppedFrames;
            beforeOutputDrops += pair.before.outputQueueDroppedFrames;
            afterOutputDrops += pair.after.outputQueueDroppedFrames;
            beforeNackRecovered += pair.before.deadlineNackRecoveredFrames;
            afterNackRecovered += pair.after.deadlineNackRecoveredFrames;
            beforeNackExpired +=
                pair.before.deadlineNackExpiredDroppedFrames;
            afterNackExpired +=
                pair.after.deadlineNackExpiredDroppedFrames;
        }

        const double matchedCount =
            (std::max)(1.0, static_cast<double>(matched.size()));
        const double avgFpsDelta =
            matched.empty()
            ? 0.0
            : (afterFpsSum - beforeFpsSum) / matchedCount;
        const double avgP95Delta =
            matched.empty()
            ? 0.0
            : (afterP95Sum - beforeP95Sum) / matchedCount;
        const int64_t dropDelta =
            static_cast<int64_t>(afterDrops) -
            static_cast<int64_t>(beforeDrops);
        const int64_t deadlineDropDelta =
            static_cast<int64_t>(afterDeadlineDrops) -
            static_cast<int64_t>(beforeDeadlineDrops);
        const int64_t outputDropDelta =
            static_cast<int64_t>(afterOutputDrops) -
            static_cast<int64_t>(beforeOutputDrops);
        const int64_t nackRecoveryDelta =
            static_cast<int64_t>(afterNackRecovered) -
            static_cast<int64_t>(beforeNackRecovered);
        const int64_t nackExpiredDelta =
            static_cast<int64_t>(afterNackExpired) -
            static_cast<int64_t>(beforeNackExpired);

        const bool hasLatencyImprovement = avgP95Delta <= -1.0;
        const bool hasDropImprovement = dropDelta < 0;
        const bool hasFpsImprovement = avgFpsDelta >= 1.0;
        const bool hasMeaningfulRegression =
            avgP95Delta >= 5.0 ||
            dropDelta > 3 ||
            avgFpsDelta <= -3.0;

        const char* overallResult = "INCONCLUSIVE";
        if (!matched.empty()) {
            if ((hasLatencyImprovement || hasDropImprovement || hasFpsImprovement) &&
                !hasMeaningfulRegression) {
                overallResult = "IMPROVED";
            }
            else if (hasMeaningfulRegression &&
                !hasLatencyImprovement &&
                !hasDropImprovement) {
                overallResult = "REGRESSED";
            }
            else {
                overallResult = "MIXED";
            }
        }

        file << "# Network Before/After Portfolio Report\n\n";
        file << "Generated: " << generatedTimestamp_ << "\n\n";
        file << "- Before CSV: "
            << (previousSummaryCsvPath_.empty()
                ? "not found"
                : previousSummaryCsvPath_)
            << "\n";
        file << "- After CSV: " << csvFilePath_ << "\n\n";

        file << "## Executive Summary\n\n";
        if (previousSummaryCsvPath_.empty() || beforeSummaries.empty()) {
            file << "- No previous summary CSV was available. Run the experiment at least twice to produce a true before/after comparison.\n\n";
        }
        else if (summaries_.empty()) {
            file << "- Previous summary was loaded, but no scenarios have completed in the current run yet. This report refreshes as each scenario finishes.\n\n";
        }
        else if (matched.empty()) {
            file << "- No matching scenario names were found between the previous and current summaries.\n\n";
        }
        else {
            file << "- Overall result: **" << overallResult << "**.\n";
            file << "- Matched scenarios: " << matched.size() << ".\n";
            file << "- Average display FPS delta: "
                << FormatDelta(avgFpsDelta)
                << " fps.\n";
            file << "- Average p95 latency delta: "
                << FormatDelta(avgP95Delta)
                << " ms.\n";
            file << "- Total drop delta: "
                << FormatIntDelta(dropDelta)
                << " frames ("
                << FormatChangePercent(beforeDrops, afterDrops)
                << " change).\n";
            file << "- Deadline drop delta: "
                << FormatIntDelta(deadlineDropDelta)
                << ", output queue drop delta: "
                << FormatIntDelta(outputDropDelta)
                << ".\n";
            file << "- Deadline NACK recovered-frame delta: "
                << FormatIntDelta(nackRecoveryDelta)
                << " frames.\n";
            file << "- Deadline NACK expired-drop delta: "
                << FormatIntDelta(nackExpiredDelta)
                << " frames.\n\n";
        }

        file << "## Portfolio Snapshot\n\n";
        file
            << "| Item | Summary |\n"
            << "| --- | --- |\n"
            << "| What changed | Compared persisted `network_summary_*.csv` runs and generated a scenario-matched evaluation report. |\n"
            << "| Why it matters | The engine now demonstrates the full implementation -> measurement -> evaluation -> improvement loop expected in realtime networking work. |\n"
            << "| Evidence source | CSV telemetry condensed into scenario summaries, then compared by scenario name and adaptive mode. |\n"
            << "| Primary result | " << overallResult << " across "
            << matched.size() << " matched scenarios. |\n"
            << "| Main trade-off | Low-latency recovery may intentionally expire stale frames instead of displaying late video. |\n\n";

        file << "## Key Metrics\n\n";
        file
            << "| Metric | Before | After | Delta | Interpretation |\n"
            << "| --- | ---: | ---: | ---: | --- |\n";

        file << "| Avg display FPS | "
            << FormatDouble(matched.empty() ? 0.0 : beforeFpsSum / matchedCount)
            << " | "
            << FormatDouble(matched.empty() ? 0.0 : afterFpsSum / matchedCount)
            << " | "
            << FormatDelta(avgFpsDelta)
            << " | "
            << (avgFpsDelta >= 1.0
                ? "smoother display"
                : avgFpsDelta <= -1.0
                ? "lower display rate"
                : "roughly unchanged")
            << " |\n";

        file << "| Avg p95 latency ms | "
            << FormatDouble(matched.empty() ? 0.0 : beforeP95Sum / matchedCount)
            << " | "
            << FormatDouble(matched.empty() ? 0.0 : afterP95Sum / matchedCount)
            << " | "
            << FormatDelta(avgP95Delta)
            << " | "
            << (avgP95Delta <= -1.0
                ? "lower tail latency"
                : avgP95Delta >= 1.0
                ? "higher tail latency"
                : "roughly unchanged")
            << " |\n";

        file << "| Total frame drops | "
            << beforeDrops << " | "
            << afterDrops << " | "
            << FormatIntDelta(dropDelta)
            << " | "
            << (dropDelta < 0
                ? "fewer lost frames"
                : dropDelta > 0
                ? "more dropped frames"
                : "unchanged")
            << " |\n";

        file << "| Deadline drops | "
            << beforeDeadlineDrops << " | "
            << afterDeadlineDrops << " | "
            << FormatIntDelta(deadlineDropDelta)
            << " | display-deadline pressure |\n";

        file << "| Output queue drops | "
            << beforeOutputDrops << " | "
            << afterOutputDrops << " | "
            << FormatIntDelta(outputDropDelta)
            << " | renderer/jitter backlog pressure |\n";

        file << "| NACK recovered frames | "
            << beforeNackRecovered << " | "
            << afterNackRecovered << " | "
            << FormatIntDelta(nackRecoveryDelta)
            << " | selective retransmit recovery evidence |\n";

        file << "| NACK expired drops | "
            << beforeNackExpired << " | "
            << afterNackExpired << " | "
            << FormatIntDelta(nackExpiredDelta)
            << " | stale recovery discarded before missing deadline |\n\n";

        file << "## Scenario Comparison\n\n";
        file
            << "| Scenario | Runtime Before | Runtime After | Mode Before | Mode After | Cause Before | Cause After | FPS Before | FPS After | FPS Delta | P95 Before ms | P95 After ms | P95 Delta ms | Drops Before | Drops After | Drop Delta | Drop Change | NACK Recovered Before | NACK Recovered After | NACK Expired Before | NACK Expired After |\n"
            << "| --- | --- | --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";

        for (const ScenarioPair& pair : matched) {
            const double fpsDelta =
                pair.after.avgDisplayFps - pair.before.avgDisplayFps;
            const double p95Delta =
                pair.after.p95LatencyMs - pair.before.p95LatencyMs;
            const int64_t dropDelta =
                static_cast<int64_t>(pair.after.droppedFrames) -
                static_cast<int64_t>(pair.before.droppedFrames);

            file << "| "
                << EscapeMarkdownTable(pair.after.name) << " | "
                << EscapeMarkdownTable(
                    pair.before.networkRuntimeMode.empty()
                    ? "unknown"
                    : pair.before.networkRuntimeMode) << " | "
                << EscapeMarkdownTable(
                    pair.after.networkRuntimeMode.empty()
                    ? "unknown"
                    : pair.after.networkRuntimeMode) << " | "
                << EscapeMarkdownTable(
                    pair.before.adaptiveControlMode.empty()
                    ? "unknown"
                    : pair.before.adaptiveControlMode) << " | "
                << EscapeMarkdownTable(
                    pair.after.adaptiveControlMode.empty()
                    ? "unknown"
                    : pair.after.adaptiveControlMode) << " | "
                << EscapeMarkdownTable(
                    pair.before.dominantAdaptiveDegradationCause.empty()
                    ? "None"
                    : pair.before.dominantAdaptiveDegradationCause) << " | "
                << EscapeMarkdownTable(
                    pair.after.dominantAdaptiveDegradationCause.empty()
                    ? "None"
                    : pair.after.dominantAdaptiveDegradationCause) << " | "
                << FormatDouble(pair.before.avgDisplayFps) << " | "
                << FormatDouble(pair.after.avgDisplayFps) << " | "
                << FormatDelta(fpsDelta) << " | "
                << FormatDouble(pair.before.p95LatencyMs) << " | "
                << FormatDouble(pair.after.p95LatencyMs) << " | "
                << FormatDelta(p95Delta) << " | "
                << pair.before.droppedFrames << " | "
                << pair.after.droppedFrames << " | "
                << FormatIntDelta(dropDelta) << " | "
                << FormatChangePercent(
                    pair.before.droppedFrames,
                    pair.after.droppedFrames)
                << " | "
                << pair.before.deadlineNackRecoveredFrames << " | "
                << pair.after.deadlineNackRecoveredFrames << " | "
                << pair.before.deadlineNackExpiredDroppedFrames << " | "
                << pair.after.deadlineNackExpiredDroppedFrames << " |\n";
        }
        file << "\n";

        file << "## After-Run Mode Winners\n\n";
        if (summaries_.empty()) {
            file << "- Waiting for current-run scenarios.\n\n";
        }
        else {
            std::vector<std::string> networkScenarioOrder;
            std::unordered_map<std::string, std::vector<const ScenarioSummary*>>
                summariesByNetworkScenario;

            for (const ScenarioSummary& summary : summaries_) {
                const std::string networkScenario =
                    ExtractNetworkScenarioName(summary.name);
                if (summariesByNetworkScenario.find(networkScenario) ==
                    summariesByNetworkScenario.end()) {
                    networkScenarioOrder.push_back(networkScenario);
                }
                summariesByNetworkScenario[networkScenario].push_back(&summary);
            }

            const auto scoreScenario =
                [](const ScenarioSummary& summary) -> double {
                return BuildScoreBreakdown(summary, false, false).finalScore;
            };

            file
                << "| Network Scenario | Best Mode | Why It Wins | FPS | P95 Latency ms | Drops | NACK Recovered | Trade-off |\n"
                << "| --- | --- | --- | ---: | ---: | ---: | ---: | --- |\n";

            for (const std::string& networkScenario : networkScenarioOrder) {
                const auto it =
                    summariesByNetworkScenario.find(networkScenario);
                if (it == summariesByNetworkScenario.end() ||
                    it->second.empty()) {
                    continue;
                }

                const ScenarioSummary* winner = it->second.front();
                double winnerScore = scoreScenario(*winner);
                for (const ScenarioSummary* candidate : it->second) {
                    const double candidateScore = scoreScenario(*candidate);
                    if (candidateScore < winnerScore) {
                        winner = candidate;
                        winnerScore = candidateScore;
                    }
                }

                std::string why = "best latency/drop balance";
                if (winner->droppedFrames == 0 &&
                    winner->p95LatencyMs < 150.0) {
                    why = "met the 150ms deadline with no frame drops";
                }
                else if (winner->deadlineNackRecoveredFrames > 0 &&
                    winner->droppedFrames <= winner->deadlineNackRecoveredFrames) {
                    why = "used selective retransmit while keeping drops bounded";
                }
                else if (winner->avgDisplayFps >= 24.0 &&
                    winner->p95LatencyMs < 150.0) {
                    why = "kept interactive FPS under the latency deadline";
                }

                std::string tradeoff = "none observed";
                if (winner->minTargetJpegQuality <= 40 ||
                    winner->minTargetFps <= 8 ||
                    (winner->minTargetBitrateKbps > 0 &&
                        winner->minTargetBitrateKbps <= 1000)) {
                    tradeoff = "quality or FPS reached the lower adaptive bound";
                }
                else if (winner->deadlineNackExpiredDroppedFrames > 0) {
                    tradeoff = "expired stale recovery to protect latency";
                }
                else if (winner->avgDisplayFps < 20.0) {
                    tradeoff = "lower display FPS";
                }

                file << "| "
                    << EscapeMarkdownTable(networkScenario) << " | "
                    << EscapeMarkdownTable(
                        winner->adaptiveControlMode.empty()
                        ? "unknown"
                        : winner->adaptiveControlMode) << " | "
                    << EscapeMarkdownTable(why) << " | "
                    << FormatDouble(winner->avgDisplayFps) << " | "
                    << FormatDouble(winner->p95LatencyMs) << " | "
                    << winner->droppedFrames << " | "
                    << winner->deadlineNackRecoveredFrames << " | "
                    << EscapeMarkdownTable(tradeoff) << " |\n";
            }

            file << "\n";
        }

        file << "## Automatic Evaluation\n\n";
        if (matched.empty()) {
            file << "- Waiting for comparable scenarios.\n";
        }
        else {
            for (const ScenarioPair& pair : matched) {
                const double fpsDelta =
                    pair.after.avgDisplayFps - pair.before.avgDisplayFps;
                const double p95Delta =
                    pair.after.p95LatencyMs - pair.before.p95LatencyMs;
                const int64_t dropDelta =
                    static_cast<int64_t>(pair.after.droppedFrames) -
                    static_cast<int64_t>(pair.before.droppedFrames);
                const int64_t nackRecoveryDelta =
                    static_cast<int64_t>(pair.after.deadlineNackRecoveredFrames) -
                    static_cast<int64_t>(pair.before.deadlineNackRecoveredFrames);
                const int64_t nackExpiredDelta =
                    static_cast<int64_t>(pair.after.deadlineNackExpiredDroppedFrames) -
                    static_cast<int64_t>(pair.before.deadlineNackExpiredDroppedFrames);

                file << "- " << pair.after.name << ": ";

                bool wroteFinding = false;
                if (fpsDelta >= 1.0) {
                    file << "display FPS improved by "
                        << FormatDouble(fpsDelta) << " fps";
                    wroteFinding = true;
                }
                else if (fpsDelta <= -1.0) {
                    file << "display FPS regressed by "
                        << FormatDouble(-fpsDelta) << " fps";
                    wroteFinding = true;
                }

                if (p95Delta <= -1.0) {
                    file << (wroteFinding ? "; " : "")
                        << "p95 latency improved by "
                        << FormatDouble(-p95Delta) << " ms";
                    wroteFinding = true;
                }
                else if (p95Delta >= 1.0) {
                    file << (wroteFinding ? "; " : "")
                        << "p95 latency regressed by "
                        << FormatDouble(p95Delta) << " ms";
                    wroteFinding = true;
                }

                if (dropDelta < 0) {
                    file << (wroteFinding ? "; " : "")
                        << "frame drops reduced by "
                        << -dropDelta;
                    wroteFinding = true;
                }
                else if (dropDelta > 0) {
                    file << (wroteFinding ? "; " : "")
                        << "frame drops increased by "
                        << dropDelta;
                    wroteFinding = true;
                }

                if (nackRecoveryDelta > 0) {
                    file << (wroteFinding ? "; " : "")
                        << "selective retransmit recovered "
                        << nackRecoveryDelta
                        << " more frames";
                    wroteFinding = true;
                }

                if (nackExpiredDelta > 0) {
                    file << (wroteFinding ? "; " : "")
                        << "stale NACK recovery expired "
                        << nackExpiredDelta
                        << " more frames before display deadline";
                    wroteFinding = true;
                }

                if (!wroteFinding) {
                    file << "no material metric change";
                }

                file << ".\n";
            }
        }

        file << "\n## Interview-Ready Summary\n\n";
        file << "### What Changed\n\n";
        file << "The engine compares two persisted experiment runs directly from `network_summary_*.csv`, matches scenarios by name, and reports FPS, p95 latency, frame drops, deadline drops, output queue drops, NACK recovery, and stale-recovery expiry.\n\n";

        file << "### Why It Matters\n\n";
        file << "Realtime video networking is not only about sending packets. This report shows whether reliability and adaptive-control changes actually improved the user-visible pipeline under repeatable network conditions.\n\n";

        file << "### Evidence\n\n";
        if (matched.empty()) {
            file << "No matching scenarios are available yet. Run the same automatic experiment sequence twice to populate this section with true before/after evidence.\n\n";
        }
        else {
            file << "- Overall result: " << overallResult << ".\n";
            file << "- Average display FPS delta: "
                << FormatDelta(avgFpsDelta) << " fps.\n";
            file << "- Average p95 latency delta: "
                << FormatDelta(avgP95Delta) << " ms.\n";
            file << "- Total frame drop delta: "
                << FormatIntDelta(dropDelta) << " frames.\n";
            file << "- NACK recovered-frame delta: "
                << FormatIntDelta(nackRecoveryDelta) << " frames.\n\n";
        }

        file << "### Trade-Off\n\n";
        file << "The low-latency policy may drop or expire incomplete frames that cannot arrive before the display deadline. That is intentional for interactive video: a late frame is less useful than a fresh frame.\n\n";

        file << "### Next Improvement\n\n";
        file << "The next portfolio-level step is to add time-series charts for FPS, p95 latency, QoE score, target quality, and recovery events so the report shows both summary evidence and how the controller reacted over time.\n";
        file.flush();
    }

    void NetworkExperimentReporter::WriteRepeatReport() const {
        if (repeatReportFilePath_.empty() || csvFilePath_.empty()) {
            return;
        }

        std::ofstream file(repeatReportFilePath_, std::ios::out | std::ios::trunc);
        if (!file) {
            return;
        }

        file << "# Network Repeatability Report\n\n";
        file << "Generated: " << generatedTimestamp_ << "\n\n";

        const std::filesystem::path csvPath(csvFilePath_);
        const std::filesystem::path directory = csvPath.parent_path();
        const std::vector<std::string> candidateSummaryPaths =
            FindLatestSummaryCsvs(directory.string(), 8);

        std::vector<std::string> summaryPaths;
        const std::vector<ScenarioSummary> currentRunSummaries =
            LoadSummaryCsv(csvFilePath_);
        std::error_code currentPathEc;
        const std::filesystem::path currentSummaryPath =
            std::filesystem::absolute(csvFilePath_, currentPathEc)
            .lexically_normal();
        if (!currentRunSummaries.empty()) {
            summaryPaths.push_back(csvFilePath_);
        }

        for (const std::string& candidatePath : candidateSummaryPaths) {
            std::error_code candidatePathEc;
            const std::filesystem::path normalizedCandidate =
                std::filesystem::absolute(candidatePath, candidatePathEc)
                .lexically_normal();
            if (!currentPathEc && !candidatePathEc &&
                normalizedCandidate == currentSummaryPath) {
                continue;
            }
            summaryPaths.push_back(candidatePath);
            if (summaryPaths.size() >= 3) {
                break;
            }
        }

        file << "## Source Runs\n\n";
        if (summaryPaths.empty()) {
            file << "- Waiting for completed `network_summary_*.csv` files.\n\n";
            return;
        }

        file
            << "| Order | Summary CSV |\n"
            << "| ---: | --- |\n";
        for (size_t i = 0; i < summaryPaths.size(); ++i) {
            file << "| " << (i + 1) << " | `"
                << EscapeMarkdownTable(
                    std::filesystem::path(summaryPaths[i]).filename().string())
                << "` |\n";
        }
        file << "\n";

        struct RepeatRun {
            std::string path;
            std::string label;
            std::vector<ScenarioSummary> summaries;
            std::unordered_map<std::string, const ScenarioSummary*> byName;
        };

        std::vector<RepeatRun> runs;
        runs.reserve(summaryPaths.size());
        for (const std::string& path : summaryPaths) {
            RepeatRun run{};
            run.path = path;
            run.label = std::filesystem::path(path).filename().string();
            run.summaries = LoadSummaryCsv(path);
            if (run.summaries.empty()) {
                continue;
            }
            for (const ScenarioSummary& summary : run.summaries) {
                run.byName[summary.name] = &summary;
            }
            runs.push_back(std::move(run));
        }

        const double requiredWarmupSec = currentRunSummaries.empty()
            ? 0.0
            : currentRunSummaries.front().warmupSec;
        auto isCompatibleRun =
            [requiredWarmupSec](const RepeatRun& run) -> bool {
            if (run.summaries.empty()) {
                return false;
            }

            for (const ScenarioSummary& summary : run.summaries) {
                if (std::fabs(summary.warmupSec - requiredWarmupSec) > 0.001) {
                    return false;
                }
                if (requiredWarmupSec > 0.0 &&
                    summary.measuredSampleCount == 0) {
                    return false;
                }
            }

            return true;
        };

        std::vector<RepeatRun> compatibleRuns;
        compatibleRuns.reserve(runs.size());
        size_t incompatibleRunCount = 0;
        for (RepeatRun& run : runs) {
            if (isCompatibleRun(run)) {
                compatibleRuns.push_back(std::move(run));
            }
            else {
                incompatibleRunCount++;
            }
        }
        runs = std::move(compatibleRuns);

        if (incompatibleRunCount > 0) {
            file << "## Evaluation Compatibility\n\n";
            file << "- Excluded " << incompatibleRunCount
                << " older/incompatible run(s) from repeatability because "
                << "their warmup window or measured-sample schema differs "
                << "from the current run.\n\n";
        }

        if (runs.size() < 3) {
            file << "## Status\n\n";
            file << "- Need 3 compatible completed summary CSV files; found "
                << runs.size()
                << " compatible completed run(s). Run more automatic "
                << "experiments with the same manifest/scoring config to "
                << "enable repeatability statistics.\n\n";
            return;
        }

        file << "## Evaluation Window\n\n";
        file << "- Warmup excluded per scenario: "
            << FormatDouble(runs.front().summaries.front().warmupSec)
            << " sec.\n";
        file << "- Repeatability metrics use the measured window recorded in "
            << "`measurementSec` / `measuredSampleCount`, so startup "
            << "transients do not dominate the winner.\n\n";

        std::vector<std::string> scenarioOrder;
        for (const ScenarioSummary& summary : runs.front().summaries) {
            bool presentInAllRuns = true;
            for (const RepeatRun& run : runs) {
                if (run.byName.find(summary.name) == run.byName.end()) {
                    presentInAllRuns = false;
                    break;
                }
            }
            if (presentInAllRuns) {
                scenarioOrder.push_back(summary.name);
            }
        }

        const auto formatStats =
            [](const RepeatMetricStats& stats) -> std::string {
            if (stats.count == 0) {
                return "n/a";
            }

            return FormatDouble(stats.mean) + " +/- " +
                FormatDouble(stats.stddev) + " (" +
                FormatDouble(stats.min) + "-" +
                FormatDouble(stats.max) + ")";
        };

        const auto repeatScore =
            [](const ScenarioSummary& summary) -> double {
            return BuildScoreBreakdown(summary, false, false).finalScore;
        };

        std::vector<std::string> networkScenarioOrder;
        for (const ScenarioSummary& summary : runs.front().summaries) {
            const std::string networkScenario =
                ExtractNetworkScenarioName(summary.name);
            if (std::find(
                networkScenarioOrder.begin(),
                networkScenarioOrder.end(),
                networkScenario) == networkScenarioOrder.end()) {
                networkScenarioOrder.push_back(networkScenario);
            }
        }

        uint32_t stableWinnerCount = 0;
        uint32_t comparableWinnerCount = 0;
        for (const std::string& networkScenario : networkScenarioOrder) {
            std::unordered_map<std::string, uint32_t> winnerCounts;
            for (const RepeatRun& run : runs) {
                const ScenarioSummary* winner = nullptr;
                double winnerScore = 0.0;
                for (const ScenarioSummary& summary : run.summaries) {
                    if (ExtractNetworkScenarioName(summary.name) !=
                        networkScenario) {
                        continue;
                    }
                    const double candidateScore = repeatScore(summary);
                    if (winner == nullptr || candidateScore < winnerScore) {
                        winner = &summary;
                        winnerScore = candidateScore;
                    }
                }
                if (winner != nullptr) {
                    winnerCounts[ModeLabel(*winner)]++;
                }
            }

            if (!winnerCounts.empty()) {
                comparableWinnerCount++;
                uint32_t bestCount = 0;
                for (const auto& entry : winnerCounts) {
                    bestCount = (std::max)(bestCount, entry.second);
                }
                if (bestCount == runs.size()) {
                    stableWinnerCount++;
                }
            }
        }

        file << "## Executive Summary\n\n";
        file << "- Runs analyzed: " << runs.size()
            << " latest completed summaries.\n";
        file << "- Common scenario rows: " << scenarioOrder.size() << ".\n";
        file << "- Winner stability: " << stableWinnerCount << " / "
            << comparableWinnerCount
            << " network scenarios had the same winner in all analyzed runs.\n";
        file << "- Score formula: latency, drops, output queue drops, stale "
            << "NACK expiry, FPS bonus, and quality guardrail penalty. "
            << "QoE time-series bonus is omitted here because persisted summary "
            << "CSV files do not contain per-sample QoE history.\n\n";

        file << "## Scenario Repeatability\n\n";
        if (scenarioOrder.empty()) {
            file << "- No scenario names were common across all analyzed runs.\n\n";
        }
        else {
            file
                << "| Scenario | Runs | Display FPS | P95 Latency ms | Dropped Frames | Final Score |\n"
                << "| --- | ---: | ---: | ---: | ---: | ---: |\n";

            for (const std::string& scenarioName : scenarioOrder) {
                std::vector<double> fpsValues;
                std::vector<double> p95Values;
                std::vector<double> dropValues;
                std::vector<double> scoreValues;
                fpsValues.reserve(runs.size());
                p95Values.reserve(runs.size());
                dropValues.reserve(runs.size());
                scoreValues.reserve(runs.size());

                for (const RepeatRun& run : runs) {
                    const auto it = run.byName.find(scenarioName);
                    if (it == run.byName.end()) {
                        continue;
                    }
                    const ScenarioSummary& summary = *it->second;
                    fpsValues.push_back(summary.avgDisplayFps);
                    p95Values.push_back(summary.p95LatencyMs);
                    dropValues.push_back(
                        static_cast<double>(summary.droppedFrames));
                    scoreValues.push_back(repeatScore(summary));
                }

                file << "| "
                    << EscapeMarkdownTable(scenarioName) << " | "
                    << fpsValues.size() << " | "
                    << formatStats(ComputeRepeatMetricStats(fpsValues)) << " | "
                    << formatStats(ComputeRepeatMetricStats(p95Values)) << " | "
                    << formatStats(ComputeRepeatMetricStats(dropValues)) << " | "
                    << formatStats(ComputeRepeatMetricStats(scoreValues)) << " |\n";
            }
            file << "\n";
        }

        file << "## Winner Consistency\n\n";
        file
            << "| Network Scenario | Agreement | Majority Winner | Run Winners | Interpretation |\n"
            << "| --- | ---: | --- | --- | --- |\n";

        for (const std::string& networkScenario : networkScenarioOrder) {
            std::vector<std::string> runWinners;
            std::unordered_map<std::string, uint32_t> winnerCounts;

            for (const RepeatRun& run : runs) {
                const ScenarioSummary* winner = nullptr;
                double winnerScore = 0.0;
                for (const ScenarioSummary& summary : run.summaries) {
                    if (ExtractNetworkScenarioName(summary.name) !=
                        networkScenario) {
                        continue;
                    }
                    const double candidateScore = repeatScore(summary);
                    if (winner == nullptr || candidateScore < winnerScore) {
                        winner = &summary;
                        winnerScore = candidateScore;
                    }
                }

                if (winner == nullptr) {
                    runWinners.push_back("n/a");
                    continue;
                }

                const std::string winnerMode = ModeLabel(*winner);
                runWinners.push_back(winnerMode);
                winnerCounts[winnerMode]++;
            }

            std::string majorityWinner = "n/a";
            uint32_t majorityCount = 0;
            for (const auto& entry : winnerCounts) {
                if (entry.second > majorityCount) {
                    majorityWinner = entry.first;
                    majorityCount = entry.second;
                }
            }

            std::ostringstream runWinnerText;
            for (size_t i = 0; i < runWinners.size(); ++i) {
                if (i > 0) {
                    runWinnerText << " / ";
                }
                runWinnerText << runWinners[i];
            }

            const std::string interpretation =
                majorityCount == runs.size()
                ? "stable winner"
                : "winner varied across runs; inspect stddev before claiming dominance";

            file << "| "
                << EscapeMarkdownTable(networkScenario) << " | "
                << majorityCount << " / " << runs.size() << " | "
                << EscapeMarkdownTable(majorityWinner) << " | "
                << EscapeMarkdownTable(runWinnerText.str()) << " | "
                << EscapeMarkdownTable(interpretation) << " |\n";
        }

        file << "\n";
        file << "## Interview-Ready Reading\n\n";
        file << "- Use the mean as the expected behavior and stddev/min/max as "
            << "the stability evidence.\n";
        file << "- A mode that wins 3 / 3 runs is a stronger portfolio claim than "
            << "a mode that wins only once by a narrow score.\n";
        file << "- If winner consistency is low, the next engineering task should "
            << "be reducing variance or increasing scenario duration before "
            << "tuning adaptive control weights.\n";
    }

    std::vector<std::string> NetworkExperimentReporter::FindLatestSummaryCsvs(
        const std::string& directory,
        size_t count
    ) {
        std::vector<std::pair<std::filesystem::file_time_type, std::string>>
            candidates;
        std::error_code ec;

        for (const auto& entry : std::filesystem::directory_iterator(
            directory,
            ec)) {
            if (ec) {
                break;
            }

            if (!entry.is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }

            const std::filesystem::path path = entry.path();
            const std::string fileName = path.filename().string();
            if (fileName.rfind("network_summary_", 0) != 0 ||
                path.extension() != ".csv") {
                continue;
            }

            if (entry.file_size(ec) == 0 || ec) {
                ec.clear();
                continue;
            }

            const std::filesystem::file_time_type writeTime =
                entry.last_write_time(ec);
            if (ec) {
                ec.clear();
                continue;
            }

            candidates.emplace_back(writeTime, path.string());
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.first > rhs.first;
            }
        );

        std::vector<std::string> paths;
        const size_t limit = (std::min)(count, candidates.size());
        paths.reserve(limit);
        for (size_t i = 0; i < limit; ++i) {
            paths.push_back(candidates[i].second);
        }

        return paths;
    }

    std::string NetworkExperimentReporter::FindPreviousSummaryCsv(
        const std::string& directory,
        const std::string& currentCsvPath
    ) {
        std::error_code ec;
        const std::filesystem::path currentPath =
            std::filesystem::absolute(currentCsvPath, ec).lexically_normal();

        std::vector<
            std::pair<std::filesystem::file_time_type, std::string>
        > candidates;

        for (const std::filesystem::directory_entry& entry :
            std::filesystem::directory_iterator(directory, ec)) {
            if (ec) {
                break;
            }

            if (!entry.is_regular_file(ec)) {
                continue;
            }

            const std::filesystem::path path = entry.path();
            const std::string fileName = path.filename().string();
            if (fileName.rfind("network_summary_", 0) != 0 ||
                path.extension() != ".csv") {
                continue;
            }

            if (entry.file_size(ec) == 0) {
                continue;
            }

            const std::filesystem::path absolutePath =
                std::filesystem::absolute(path, ec).lexically_normal();
            if (!ec && absolutePath == currentPath) {
                continue;
            }

            const std::filesystem::file_time_type writeTime =
                entry.last_write_time(ec);
            if (ec) {
                continue;
            }

            candidates.emplace_back(writeTime, path.string());
        }

        if (candidates.empty()) {
            return {};
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.first > rhs.first;
            }
        );

        return candidates.front().second;
    }

    std::vector<NetworkExperimentReporter::ScenarioSummary>
        NetworkExperimentReporter::LoadSummaryCsv(const std::string& path) {
        std::ifstream file(path);
        if (!file) {
            return {};
        }

        std::string headerLine;
        if (!std::getline(file, headerLine)) {
            return {};
        }

        const std::vector<std::string> headers = ParseCsvLine(headerLine);
        std::unordered_map<std::string, size_t> columns;
        for (size_t i = 0; i < headers.size(); ++i) {
            columns[headers[i]] = i;
        }

        const auto getCell = [&columns](const std::vector<std::string>& row,
            const std::string& name) -> std::string {
            const auto it = columns.find(name);
            if (it == columns.end() || it->second >= row.size()) {
                return {};
            }
            return row[it->second];
        };

        std::vector<ScenarioSummary> summaries;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) {
                continue;
            }

            const std::vector<std::string> row = ParseCsvLine(line);
            ScenarioSummary summary{};
            summary.name = getCell(row, "scenarioName");
            if (summary.name.empty()) {
                continue;
            }

            summary.sampleCount = static_cast<uint32_t>(
                ParseUint64OrDefault(getCell(row, "sampleCount")));
            summary.startTimeSec =
                ParseDoubleOrDefault(getCell(row, "startTimeSec"));
            summary.measurementStartTimeSec =
                ParseDoubleOrDefault(
                    getCell(row, "measurementStartTimeSec"));
            summary.endTimeSec =
                ParseDoubleOrDefault(getCell(row, "endTimeSec"));
            summary.durationSec =
                ParseDoubleOrDefault(getCell(row, "durationSec"));
            summary.warmupSec =
                ParseDoubleOrDefault(getCell(row, "warmupSec"));
            summary.measurementSec =
                ParseDoubleOrDefault(getCell(row, "measurementSec"));
            summary.warmupSampleCount = static_cast<uint32_t>(
                ParseUint64OrDefault(getCell(row, "warmupSampleCount")));
            summary.measuredSampleCount = static_cast<uint32_t>(
                ParseUint64OrDefault(getCell(row, "measuredSampleCount")));
            if (summary.measuredSampleCount == 0) {
                summary.measuredSampleCount = summary.sampleCount;
            }
            summary.avgLatencyMs =
                ParseDoubleOrDefault(getCell(row, "avgLatencyMs"));
            summary.p95LatencyMs =
                ParseDoubleOrDefault(getCell(row, "p95LatencyMs"));
            summary.maxLatencyMs =
                ParseDoubleOrDefault(getCell(row, "maxLatencyMs"));
            summary.avgDisplayFps =
                ParseDoubleOrDefault(getCell(row, "avgDisplayFps"));
            summary.minDisplayFps =
                ParseDoubleOrDefault(getCell(row, "minDisplayFps"));
            summary.avgReceiveFps =
                ParseDoubleOrDefault(getCell(row, "avgReceiveFps"));
            summary.avgDecodeFps =
                ParseDoubleOrDefault(getCell(row, "avgDecodeFps"));
            summary.avgPacketLossRate =
                ParseDoubleOrDefault(getCell(row, "avgPacketLossRate"));
            summary.maxPacketLossRate =
                ParseDoubleOrDefault(getCell(row, "maxPacketLossRate"));
            summary.avgJitterMs =
                ParseDoubleOrDefault(getCell(row, "avgJitterMs"));
            summary.maxJitterMs =
                ParseDoubleOrDefault(getCell(row, "maxJitterMs"));
            summary.completedFrames =
                ParseUint64OrDefault(getCell(row, "completedFrames"));
            summary.displayedFrames =
                ParseUint64OrDefault(getCell(row, "displayedFrames"));
            summary.droppedFrames =
                ParseUint64OrDefault(getCell(row, "droppedFrames"));
            summary.deadlineDroppedFrames =
                ParseUint64OrDefault(getCell(row, "deadlineDroppedFrames"));
            summary.outputQueueDroppedFrames =
                ParseUint64OrDefault(getCell(row, "outputQueueDroppedFrames"));
            summary.outputQueueDropEvents =
                ParseUint64OrDefault(getCell(row, "outputQueueDropEvents"));
            summary.outputQueueDropBurstEvents =
                ParseUint64OrDefault(
                    getCell(row, "outputQueueDropBurstEvents"));
            summary.maxOutputQueueDropOldestAgeMs =
                ParseDoubleOrDefault(
                    getCell(row, "maxOutputQueueDropOldestAgeMs"));
            summary.lastOutputQueueDropReason =
                getCell(row, "lastOutputQueueDropReason");
            summary.ackCount =
                ParseUint64OrDefault(getCell(row, "ackCount"));
            summary.ackRetransmittedFrames =
                ParseUint64OrDefault(getCell(row, "ackRetransmittedFrames"));
            summary.deadlineNackSentFrames =
                ParseUint64OrDefault(getCell(row, "deadlineNackSentFrames"));
            summary.deadlineNackRecoveredFrames =
                ParseUint64OrDefault(
                    getCell(row, "deadlineNackRecoveredFrames"));
            summary.deadlineNackMissingChunks =
                ParseUint64OrDefault(
                    getCell(row, "deadlineNackMissingChunks"));
            summary.deadlineNackExpiredDroppedFrames =
                ParseUint64OrDefault(
                    getCell(row, "deadlineNackExpiredDroppedFrames"));
            summary.deadlineNackExpiredAfterNackFrames =
                ParseUint64OrDefault(
                    getCell(row, "deadlineNackExpiredAfterNackFrames"));
            summary.deadlineNackExpiredMissingChunks =
                ParseUint64OrDefault(
                    getCell(row, "deadlineNackExpiredMissingChunks"));
            summary.fecEnabled =
                ParseUint64OrDefault(getCell(row, "fecEnabled")) != 0;
            summary.adaptiveFecEnabled =
                ParseUint64OrDefault(getCell(row, "adaptiveFecEnabled")) != 0;
            summary.fecGroupChunkCount = static_cast<uint32_t>(
                ParseUint64OrDefault(getCell(row, "fecGroupChunkCount")));
            summary.fecParityPackets =
                ParseUint64OrDefault(getCell(row, "fecParityPackets"));
            summary.fecRecoveredFrames =
                ParseUint64OrDefault(getCell(row, "fecRecoveredFrames"));
            summary.fecRecoveredChunks =
                ParseUint64OrDefault(getCell(row, "fecRecoveredChunks"));
            summary.adaptiveFecRecoveryWorkingSamples =
                static_cast<uint32_t>(ParseUint64OrDefault(
                    getCell(row, "adaptiveFecRecoveryWorkingSamples")));
            summary.adaptiveFecGuardActiveSamples =
                static_cast<uint32_t>(ParseUint64OrDefault(
                    getCell(row, "adaptiveFecGuardActiveSamples")));
            summary.adaptiveFecRecoveryWorkingRatio =
                ParseDoubleOrDefault(
                    getCell(row, "adaptiveFecRecoveryWorkingRatio"));
            summary.adaptiveFecGuardActiveRatio =
                ParseDoubleOrDefault(
                    getCell(row, "adaptiveFecGuardActiveRatio"));
            summary.avgAdaptiveFecRecoveryEfficiency =
                ParseDoubleOrDefault(
                    getCell(row, "avgAdaptiveFecRecoveryEfficiency"));
            summary.adaptiveFecParityPacketDeltas =
                ParseUint64OrDefault(
                    getCell(row, "adaptiveFecParityPacketDeltas"));
            summary.adaptiveFecRecoveredFrameDeltas =
                ParseUint64OrDefault(
                    getCell(row, "adaptiveFecRecoveredFrameDeltas"));
            summary.adaptiveFecRecoveredChunkDeltas =
                ParseUint64OrDefault(
                    getCell(row, "adaptiveFecRecoveredChunkDeltas"));
            summary.simDroppedPackets =
                ParseUint64OrDefault(getCell(row, "simDroppedPackets"));
            summary.minTargetFps =
                ParseIntOrDefault(getCell(row, "minTargetFps"));
            summary.minTargetJpegQuality =
                ParseIntOrDefault(getCell(row, "minTargetJpegQuality"));
            summary.minTargetBitrateKbps =
                ParseIntOrDefault(getCell(row, "minTargetBitrateKbps"));
            summary.networkRuntimeMode =
                getCell(row, "networkRuntimeMode");
            summary.adaptiveControlMode =
                getCell(row, "adaptiveControlMode");
            summary.adaptiveCongestionControlMode =
                getCell(row, "adaptiveCongestionControlMode");
            summary.dominantAdaptiveDegradationCause =
                getCell(row, "dominantAdaptiveDegradationCause");
            summary.verdict = getCell(row, "verdict");
            summary.notes = getCell(row, "notes");

            summaries.push_back(summary);
        }

        return summaries;
    }

    std::vector<std::string> NetworkExperimentReporter::ParseCsvLine(
        const std::string& line
    ) {
        std::vector<std::string> cells;
        std::string cell;
        bool inQuotes = false;

        for (size_t i = 0; i < line.size(); ++i) {
            const char ch = line[i];

            if (inQuotes) {
                if (ch == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        cell.push_back('"');
                        i++;
                    }
                    else {
                        inQuotes = false;
                    }
                }
                else {
                    cell.push_back(ch);
                }
                continue;
            }

            if (ch == '"') {
                inQuotes = true;
            }
            else if (ch == ',') {
                cells.push_back(cell);
                cell.clear();
            }
            else {
                cell.push_back(ch);
            }
        }

        cells.push_back(cell);
        return cells;
    }

    NetworkExperimentReporter::ScenarioSummary
        NetworkExperimentReporter::BuildSummary(
            const ScenarioAccumulator& current
        ) {
        ScenarioSummary summary{};
        summary.name = current.name;
        summary.sampleCount = current.sampleCount;
        summary.startTimeSec = current.startTimeSec;
        summary.measurementStartTimeSec = current.measurementStartTimeSec;
        summary.endTimeSec = current.endTimeSec;
        summary.durationSec =
            (std::max)(0.0, current.endTimeSec - current.startTimeSec);
        summary.warmupSec = current.warmupSec;
        summary.measurementSec =
            current.sampleCount == 0
            ? 0.0
            : (std::max)(
                0.0,
                current.endTimeSec - current.measurementStartTimeSec);
        summary.warmupSampleCount = current.warmupSampleCount;
        summary.measuredSampleCount = current.sampleCount;

        const double sampleCount =
            (std::max)(1.0, static_cast<double>(current.sampleCount));

        summary.avgLatencyMs = current.latencySumMs / sampleCount;
        summary.p95LatencyMs =
            Percentile(current.latencySamplesMs, 0.95);
        summary.maxLatencyMs = current.maxLatencyMs;

        summary.avgDisplayFps = current.displayFpsSum / sampleCount;
        summary.minDisplayFps =
            current.minDisplayFps == (std::numeric_limits<double>::max)()
            ? 0.0
            : current.minDisplayFps;
        summary.avgReceiveFps = current.receiveFpsSum / sampleCount;
        summary.avgDecodeFps = current.decodeFpsSum / sampleCount;

        summary.avgPacketLossRate = current.packetLossRateSum / sampleCount;
        summary.maxPacketLossRate = current.maxPacketLossRate;
        summary.avgJitterMs = current.jitterSumMs / sampleCount;
        summary.maxJitterMs = current.maxJitterMs;

        const NetworkStatsSnapshot& baseline =
            current.hasMeasurementBaselineStats
            ? current.measurementBaselineStats
            : current.lastStats;

        summary.completedFrames =
            SubtractCounter(
                current.lastStats.completedFrames,
                baseline.completedFrames);
        summary.displayedFrames =
            SubtractCounter(
                current.lastStats.displayedFrames,
                baseline.displayedFrames);
        summary.droppedFrames =
            SubtractCounter(
                current.lastStats.droppedFrames,
                baseline.droppedFrames);
        summary.deadlineDroppedFrames =
            SubtractCounter(
                current.lastStats.deadlineDroppedFrames,
                baseline.deadlineDroppedFrames);
        summary.outputQueueDroppedFrames =
            SubtractCounter(
                current.lastStats.outputQueueDroppedFrames,
                baseline.outputQueueDroppedFrames);
        summary.outputQueueDropEvents =
            SubtractCounter(
                current.lastStats.outputQueueDropEvents,
                baseline.outputQueueDropEvents);
        summary.outputQueueDropBurstEvents =
            SubtractCounter(
                current.lastStats.outputQueueDropBurstEvents,
                baseline.outputQueueDropBurstEvents);
        summary.maxOutputQueueDropOldestAgeMs =
            current.lastStats.maxOutputQueueDropOldestAgeMs;
        summary.lastOutputQueueDropReason =
            summary.outputQueueDroppedFrames == 0
            ? std::string{}
            : current.lastStats.lastOutputQueueDropReason;
        summary.ackCount =
            SubtractCounter(
                current.lastStats.ackCount,
                baseline.ackCount);
        summary.ackRetransmittedFrames =
            SubtractCounter(
                current.lastStats.ackRetransmittedFrames,
                baseline.ackRetransmittedFrames);
        summary.deadlineNackSentFrames =
            SubtractCounter(
                current.lastStats.deadlineNackSentFrames,
                baseline.deadlineNackSentFrames);
        summary.deadlineNackRecoveredFrames =
            SubtractCounter(
                current.lastStats.deadlineNackRecoveredFrames,
                baseline.deadlineNackRecoveredFrames);
        summary.deadlineNackMissingChunks =
            SubtractCounter(
                current.lastStats.deadlineNackMissingChunks,
                baseline.deadlineNackMissingChunks);
        summary.deadlineNackExpiredDroppedFrames =
            SubtractCounter(
                current.lastStats.deadlineNackExpiredDroppedFrames,
                baseline.deadlineNackExpiredDroppedFrames);
        summary.deadlineNackExpiredAfterNackFrames =
            SubtractCounter(
                current.lastStats.deadlineNackExpiredAfterNackFrames,
                baseline.deadlineNackExpiredAfterNackFrames);
        summary.deadlineNackExpiredMissingChunks =
            SubtractCounter(
                current.lastStats.deadlineNackExpiredMissingChunks,
                baseline.deadlineNackExpiredMissingChunks);
        summary.fecEnabled = current.lastStats.fecEnabled;
        summary.adaptiveFecEnabled = current.lastStats.adaptiveFecEnabled;
        summary.fecGroupChunkCount = current.lastStats.fecGroupChunkCount;
        summary.fecParityPackets =
            SubtractCounter(
                current.lastStats.fecParityPackets,
                baseline.fecParityPackets);
        summary.fecRecoveredFrames =
            SubtractCounter(
                current.lastStats.fecRecoveredFrames,
                baseline.fecRecoveredFrames);
        summary.fecRecoveredChunks =
            SubtractCounter(
                current.lastStats.fecRecoveredChunks,
                baseline.fecRecoveredChunks);
        summary.adaptiveFecRecoveryWorkingSamples =
            current.adaptiveFecRecoveryWorkingSamples;
        summary.adaptiveFecGuardActiveSamples =
            current.adaptiveFecGuardActiveSamples;
        summary.adaptiveFecRecoveryWorkingRatio =
            sampleCount == 0
            ? 0.0
            : static_cast<double>(
                current.adaptiveFecRecoveryWorkingSamples) /
                static_cast<double>(sampleCount);
        summary.adaptiveFecGuardActiveRatio =
            sampleCount == 0
            ? 0.0
            : static_cast<double>(
                current.adaptiveFecGuardActiveSamples) /
                static_cast<double>(sampleCount);
        summary.avgAdaptiveFecRecoveryEfficiency =
            current.adaptiveFecRecoveryEfficiencySum / sampleCount;
        summary.adaptiveFecParityPacketDeltas = summary.fecParityPackets;
        summary.adaptiveFecRecoveredFrameDeltas = summary.fecRecoveredFrames;
        summary.adaptiveFecRecoveredChunkDeltas = summary.fecRecoveredChunks;
        summary.avgAdaptiveFecRecoveryEfficiency =
            summary.adaptiveFecParityPacketDeltas == 0
            ? 0.0
            : static_cast<double>(
                summary.adaptiveFecRecoveredFrameDeltas) /
                static_cast<double>(
                    summary.adaptiveFecParityPacketDeltas);
        summary.simDroppedPackets =
            SubtractCounter(
                current.lastStats.networkSimulation.droppedPackets,
                baseline.networkSimulation.droppedPackets);

        summary.minTargetFps =
            current.minTargetFps == (std::numeric_limits<int>::max)()
            ? 0
            : current.minTargetFps;
        summary.minTargetJpegQuality =
            current.minTargetJpegQuality == (std::numeric_limits<int>::max)()
            ? 0
            : current.minTargetJpegQuality;
        summary.minTargetBitrateKbps =
            current.minTargetBitrateKbps == (std::numeric_limits<int>::max)()
            ? 0
            : current.minTargetBitrateKbps;
        summary.networkRuntimeMode = current.networkRuntimeMode;
        summary.adaptiveControlMode = current.adaptiveControlMode;
        summary.adaptiveCongestionControlMode =
            current.adaptiveCongestionControlMode;

        size_t dominantCauseIndex = 0;
        uint32_t dominantCauseSamples = 0;
        for (size_t i = 0; i < current.adaptiveCauseSamples.size(); ++i) {
            if (current.adaptiveCauseSamples[i] > dominantCauseSamples) {
                dominantCauseSamples = current.adaptiveCauseSamples[i];
                dominantCauseIndex = i;
            }
        }
        summary.dominantAdaptiveDegradationCause =
            AdaptiveCauseName(dominantCauseIndex);
        summary.timeSeriesSamples = current.timeSeriesSamples;

        summary.notes = BuildVerdictNotes(summary);
        summary.verdict = summary.notes.empty() ? "PASS" : "WARN";

        return summary;
    }

    double NetworkExperimentReporter::Percentile(
        std::vector<double> values,
        double percentile
    ) {
        if (values.empty()) {
            return 0.0;
        }

        std::sort(values.begin(), values.end());

        const double clamped =
            (std::max)(0.0, (std::min)(1.0, percentile));
        const double rawIndex =
            clamped * static_cast<double>(values.size() - 1);
        const size_t lowerIndex = static_cast<size_t>(std::floor(rawIndex));
        const size_t upperIndex = static_cast<size_t>(std::ceil(rawIndex));

        if (lowerIndex == upperIndex) {
            return values[lowerIndex];
        }

        const double t = rawIndex - static_cast<double>(lowerIndex);
        return values[lowerIndex] * (1.0 - t) + values[upperIndex] * t;
    }

    double NetworkExperimentReporter::FrameDropRate(
        const ScenarioSummary& summary
    ) {
        const uint64_t totalFrames =
            summary.displayedFrames + summary.droppedFrames;
        if (totalFrames == 0) {
            return 0.0;
        }

        return static_cast<double>(summary.droppedFrames) /
            static_cast<double>(totalFrames);
    }

    double NetworkExperimentReporter::AverageQoeScore(
        const ScenarioSummary& summary
    ) {
        if (summary.timeSeriesSamples.empty()) {
            return 0.0;
        }

        double sum = 0.0;
        for (const TimeSeriesSample& sample : summary.timeSeriesSamples) {
            sum += sample.adaptiveQoeScore;
        }

        return sum /
            static_cast<double>(summary.timeSeriesSamples.size());
    }

    double NetworkExperimentReporter::FecRecoveryEfficiency(
        const ScenarioSummary& summary
    ) {
        if (summary.fecParityPackets == 0) {
            return 0.0;
        }

        return static_cast<double>(summary.fecRecoveredFrames) /
            static_cast<double>(summary.fecParityPackets);
    }

    NetworkExperimentReporter::RepeatMetricStats
        NetworkExperimentReporter::ComputeRepeatMetricStats(
            const std::vector<double>& values
        ) {
        RepeatMetricStats stats{};
        stats.count = static_cast<uint32_t>(values.size());
        if (values.empty()) {
            return stats;
        }

        stats.min = values.front();
        stats.max = values.front();
        double sum = 0.0;
        for (const double value : values) {
            sum += value;
            stats.min = (std::min)(stats.min, value);
            stats.max = (std::max)(stats.max, value);
        }
        stats.mean = sum / static_cast<double>(values.size());

        double varianceSum = 0.0;
        for (const double value : values) {
            const double delta = value - stats.mean;
            varianceSum += delta * delta;
        }
        stats.stddev =
            std::sqrt(varianceSum / static_cast<double>(values.size()));

        return stats;
    }

    std::string NetworkExperimentReporter::ModeLabel(
        const ScenarioSummary& summary
    ) {
        std::string mode = summary.adaptiveControlMode.empty()
            ? "unknown"
            : summary.adaptiveControlMode;
        if (summary.adaptiveControlMode == "QoE/Deadline Adaptive" &&
            !summary.adaptiveCongestionControlMode.empty()) {
            mode += " / ";
            mode += summary.adaptiveCongestionControlMode;
        }
        mode += " / ";
        if (summary.adaptiveFecEnabled) {
            mode += "Adaptive FEC";
        }
        else if (summary.fecEnabled) {
            mode += "FEC g";
            mode += std::to_string(summary.fecGroupChunkCount);
        }
        else {
            mode += "FEC off";
        }
        return mode;
    }

    double NetworkExperimentReporter::QualityFloorPenalty(
        const ScenarioSummary& summary
    ) {
        return
            ApplyQualityPenaltyRules(
                static_cast<double>(summary.minTargetFps),
                g_scoringConfig.fpsPenaltyRules) +
            ApplyQualityPenaltyRules(
                static_cast<double>(summary.minTargetJpegQuality),
                g_scoringConfig.jpegQualityPenaltyRules) +
            ApplyQualityPenaltyRules(
                static_cast<double>(summary.minTargetBitrateKbps),
                g_scoringConfig.bitrateKbpsPenaltyRules);
    }

    NetworkExperimentReporter::ScoreBreakdown
        NetworkExperimentReporter::BuildScoreBreakdown(
            const ScenarioSummary& summary,
            bool includeQoeBonus,
            bool includeFrameDropRatePenalty
        ) {
        ScoreBreakdown score{};

        score.latencyScore =
            summary.p95LatencyMs * g_scoringConfig.p95LatencyMsWeight;
        score.frameDropPenalty =
            static_cast<double>(summary.droppedFrames) *
            g_scoringConfig.droppedFramesWeight +
            static_cast<double>(summary.deadlineDroppedFrames) *
            g_scoringConfig.deadlineDroppedFramesWeight;
        if (includeFrameDropRatePenalty) {
            score.frameDropPenalty +=
                FrameDropRate(summary) *
                g_scoringConfig.frameDropRateWeight;
        }
        score.outputQueuePenalty =
            static_cast<double>(summary.outputQueueDroppedFrames) *
            g_scoringConfig.outputQueueDroppedFramesWeight;
        score.nackExpirePenalty =
            static_cast<double>(
                summary.deadlineNackExpiredDroppedFrames) *
            g_scoringConfig.deadlineNackExpiredDroppedFramesWeight;
        score.fecOverheadPenalty =
            static_cast<double>(summary.fecParityPackets) *
            g_scoringConfig.fecParityPacketWeight;
        if (summary.fecParityPackets > 0) {
            const double efficiency =
                FecRecoveryEfficiency(summary);
            score.fecInefficiencyPenalty =
                (std::max)(0.0, 0.25 - efficiency) *
                g_scoringConfig.fecInefficiencyPenaltyWeight;
        }
        score.fecRecoveryBonus =
            static_cast<double>(
                (std::min)(
                    summary.fecRecoveredFrames,
                    summary.fecParityPackets)) *
            g_scoringConfig.fecRecoveredFrameBonusWeight;
        score.fpsBonus =
            summary.avgDisplayFps *
            g_scoringConfig.avgDisplayFpsBonusWeight;
        score.qoeBonus = includeQoeBonus
            ? AverageQoeScore(summary) *
                g_scoringConfig.avgQoeScoreBonusWeight
            : 0.0;
        score.qualityPenalty = QualityFloorPenalty(summary);

        score.finalScore =
            score.latencyScore +
            score.frameDropPenalty +
            score.outputQueuePenalty +
            score.nackExpirePenalty -
            score.fecRecoveryBonus +
            score.fecOverheadPenalty +
            score.fecInefficiencyPenalty -
            score.fpsBonus -
            score.qoeBonus +
            score.qualityPenalty;

        return score;
    }

    std::string NetworkExperimentReporter::BuildVerdictNotes(
        const ScenarioSummary& summary
    ) {
        std::vector<std::string> notes;

        if (summary.p95LatencyMs >= 150.0) {
            notes.push_back("p95 latency reached 150ms deadline");
        }
        else if (summary.p95LatencyMs >= 135.0) {
            notes.push_back("p95 latency is near 150ms deadline");
        }

        if (summary.deadlineDroppedFrames > 1) {
            notes.push_back("deadline drops observed");
        }

        if (summary.outputQueueDroppedFrames > 0) {
            if (summary.outputQueueDropBurstEvents > 0 &&
                summary.outputQueueDropBurstEvents >= summary.outputQueueDropEvents) {
                notes.push_back("output drops likely from jitter burst release");
            }
            else if (summary.lastOutputQueueDropReason == "renderer-lag") {
                notes.push_back("output drops likely from render/decode lag");
            }
            else {
                notes.push_back("output queue drops observed");
            }
        }

        if (summary.minTargetFps > 0 &&
            summary.avgDisplayFps <
            static_cast<double>(summary.minTargetFps) * 0.75) {
            notes.push_back("display fps below adaptive target");
        }

        if (summary.minTargetFps <= 8 ||
            summary.minTargetJpegQuality <= 40 ||
            (summary.minTargetBitrateKbps > 0 &&
                summary.minTargetBitrateKbps <= 1000)) {
            notes.push_back("adaptive reached minimum quality");
        }

        std::ostringstream oss;
        for (size_t i = 0; i < notes.size(); ++i) {
            if (i > 0) {
                oss << "; ";
            }
            oss << notes[i];
        }

        return oss.str();
    }

} // namespace net
