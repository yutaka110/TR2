#include "NetworkExperimentReplay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace net {
namespace {

    struct ReplayRow {
        std::string scenario;
        double timeSec = 0.0;
        uint64_t completedFrames = 0;
        uint64_t droppedFrames = 0;
        uint64_t deadlineDroppedFrames = 0;
        uint64_t outputQueueDroppedFrames = 0;
        uint64_t nackSent = 0;
        uint64_t nackRecovered = 0;
        uint64_t nackExpired = 0;
        uint64_t fecParity = 0;
        uint64_t fecRecovered = 0;
        uint64_t fecRecoveredChunks = 0;
        int fecGroup = 0;
        std::string decisionReason;
        std::string holdReason;
        bool qualityHold = false;
        bool emergencyG2 = false;
        bool recoveryWorking = false;
        bool guardActive = false;
        double recoveryEfficiency = 0.0;
        uint64_t parityDelta = 0;
        uint64_t recoveredDelta = 0;
        uint64_t recoveredChunkDelta = 0;
        int targetJpegQuality = 0;
        double targetFps = 0.0;
        int targetBitrateKbps = 0;
        double displayFps = 0.0;
        double latencyMs = 0.0;
        double qoeScore = 0.0;
    };

    struct ReplayPhaseStats {
        std::string phase;
        size_t samples = 0;
        uint64_t completedFrames = 0;
        uint64_t droppedFrames = 0;
        uint64_t deadlineDroppedFrames = 0;
        uint64_t outputQueueDroppedFrames = 0;
        uint64_t nackSent = 0;
        uint64_t nackRecovered = 0;
        uint64_t nackExpired = 0;
        uint64_t fecParity = 0;
        uint64_t fecRecovered = 0;
        uint64_t fecRecoveredChunks = 0;
        size_t qualityHoldSamples = 0;
        size_t emergencyG2Samples = 0;
        size_t recoveryWorkingSamples = 0;
        size_t guardSamples = 0;
        int minJpegQuality = 1000;
        double minFps = 1000.0;
        int minBitrateKbps = 1000000;
        double displayFpsSum = 0.0;
        size_t displayFpsSamples = 0;
        double qoeScoreSum = 0.0;
        size_t qoeScoreSamples = 0;
        std::vector<double> latencySamplesMs;
        std::map<std::string, size_t> decisionCounts;
        std::map<std::string, size_t> holdCounts;
    };

    struct ReplayScoreBreakdown {
        double latencyScore = 0.0;
        double frameDropPenalty = 0.0;
        double outputQueuePenalty = 0.0;
        double nackExpirePenalty = 0.0;
        double fecOverheadPenalty = 0.0;
        double fecInefficiencyPenalty = 0.0;
        double fecRecoveryBonus = 0.0;
        double fpsBonus = 0.0;
        double qoeBonus = 0.0;
        double qualityPenalty = 0.0;
        double finalScore = 0.0;
    };

    struct ReplayEvaluation {
        ReplayScoreBreakdown score;
        std::string verdict;
        std::string reason;
        bool winner = false;
    };

    struct SummaryRow {
        std::string scenario;
        uint32_t warmupSampleCount = 0;
        uint32_t measuredSampleCount = 0;
        double p95LatencyMs = 0.0;
        double avgDisplayFps = 0.0;
        uint64_t completedFrames = 0;
        uint64_t droppedFrames = 0;
        uint64_t deadlineDroppedFrames = 0;
        uint64_t outputQueueDroppedFrames = 0;
        uint64_t nackExpired = 0;
        uint64_t fecParity = 0;
        uint64_t fecRecovered = 0;
        int minTargetFps = 0;
        int minTargetJpegQuality = 0;
        int minTargetBitrateKbps = 0;
    };

    struct ConsistencyRow {
        std::string scenario;
        double replayScore = 0.0;
        double reporterScore = 0.0;
        double scoreDiff = 0.0;
        bool replayWinner = false;
        bool reporterWinner = false;
        std::string verdict;
        std::string reason;
    };

    std::string ReadEnvString(const char* name) {
        char* buffer = nullptr;
        size_t size = 0;
        if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
            return {};
        }

        std::string value(buffer);
        std::free(buffer);
        return value;
    }

    std::string MakeTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);

        std::tm localTime{};
        localtime_s(&localTime, &time);

        std::ostringstream oss;
        oss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
        return oss.str();
    }

    std::vector<std::string> ParseCsvLine(const std::string& line) {
        std::vector<std::string> fields;
        std::string field;
        bool quoted = false;

        for (size_t i = 0; i < line.size(); ++i) {
            const char ch = line[i];
            if (quoted) {
                if (ch == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') {
                        field.push_back('"');
                        ++i;
                    }
                    else {
                        quoted = false;
                    }
                }
                else {
                    field.push_back(ch);
                }
            }
            else if (ch == '"') {
                quoted = true;
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

    std::string EscapeCsv(std::string value) {
        if (value.find_first_of(",\"\r\n") == std::string::npos) {
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

    std::string ValueAt(
        const std::vector<std::string>& fields,
        const std::unordered_map<std::string, size_t>& columns,
        const char* name) {
        const auto it = columns.find(name);
        if (it == columns.end() || it->second >= fields.size()) {
            return {};
        }
        return fields[it->second];
    }

    double ToDouble(const std::string& value, double fallback = 0.0) {
        if (value.empty()) {
            return fallback;
        }

        char* end = nullptr;
        const double parsed = std::strtod(value.c_str(), &end);
        return end != value.c_str() ? parsed : fallback;
    }

    uint64_t ToUint64(const std::string& value) {
        if (value.empty()) {
            return 0;
        }

        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
        return end != value.c_str() ? static_cast<uint64_t>(parsed) : 0;
    }

    int ToInt(const std::string& value) {
        return static_cast<int>(ToUint64(value));
    }

    bool ToBool(const std::string& value) {
        return value == "1" || value == "true" || value == "TRUE";
    }

    uint64_t Delta(uint64_t current, uint64_t previous) {
        return current >= previous ? current - previous : 0;
    }

    std::string Dominant(const std::map<std::string, size_t>& counts) {
        std::string best = "-";
        size_t bestCount = 0;

        for (const auto& entry : counts) {
            if (entry.first.empty()) {
                continue;
            }
            if (entry.second > bestCount) {
                best = entry.first;
                bestCount = entry.second;
            }
        }

        return best;
    }

    double Percentile(std::vector<double> values, double percentile) {
        if (values.empty()) {
            return 0.0;
        }

        std::sort(values.begin(), values.end());
        const double clamped = (std::max)(0.0, (std::min)(1.0, percentile));
        const double index = clamped * static_cast<double>(values.size() - 1);
        const size_t lower = static_cast<size_t>(std::floor(index));
        const size_t upper = static_cast<size_t>(std::ceil(index));
        if (lower == upper) {
            return values[lower];
        }

        const double weight = index - static_cast<double>(lower);
        return values[lower] * (1.0 - weight) + values[upper] * weight;
    }

    double AverageDisplayFps(const ReplayPhaseStats& stats) {
        return stats.displayFpsSamples > 0
            ? stats.displayFpsSum / static_cast<double>(stats.displayFpsSamples)
            : 0.0;
    }

    double AverageQoeScore(const ReplayPhaseStats& stats) {
        return stats.qoeScoreSamples > 0
            ? stats.qoeScoreSum / static_cast<double>(stats.qoeScoreSamples)
            : 0.0;
    }

    double FrameDropRate(const ReplayPhaseStats& stats) {
        const uint64_t denominator =
            stats.completedFrames + stats.droppedFrames;
        return denominator > 0
            ? static_cast<double>(stats.droppedFrames) /
                static_cast<double>(denominator)
            : 0.0;
    }

    double FecRecoveryEfficiency(const ReplayPhaseStats& stats) {
        return stats.fecParity > 0
            ? static_cast<double>(stats.fecRecovered) /
                static_cast<double>(stats.fecParity)
            : 0.0;
    }

    int QualityPenalty(int minJpegQuality, double minFps, int minBitrateKbps) {
        int penalty = 0;

        if (minFps > 0.0 && minFps <= 8.0) {
            penalty += 500;
        }
        else if (minFps > 0.0 && minFps < 12.0) {
            penalty += 250;
        }
        else if (minFps > 0.0 && minFps < 15.0) {
            penalty += 120;
        }

        if (minJpegQuality > 0 && minJpegQuality <= 40) {
            penalty += 450;
        }
        else if (minJpegQuality > 0 && minJpegQuality < 55) {
            penalty += 220;
        }
        else if (minJpegQuality > 0 && minJpegQuality < 65) {
            penalty += 90;
        }

        if (minBitrateKbps > 0 && minBitrateKbps <= 1000) {
            penalty += 350;
        }
        else if (minBitrateKbps > 0 && minBitrateKbps < 1800) {
            penalty += 180;
        }
        else if (minBitrateKbps > 0 && minBitrateKbps < 2500) {
            penalty += 80;
        }

        return penalty;
    }

    double ReplayRiskScore(
        uint64_t nackExpired,
        uint64_t fecRecovered,
        uint64_t fecParity,
        int qualityPenalty) {
        return static_cast<double>(nackExpired) * 20.0 +
            static_cast<double>(qualityPenalty) +
            static_cast<double>(fecParity) * 0.08 -
            static_cast<double>(fecRecovered) * 4.0;
    }

    ReplayScoreBreakdown BuildScoreBreakdown(
        const ReplayPhaseStats& stats,
        bool includeQoeBonus = true) {
        ReplayScoreBreakdown score{};

        score.latencyScore = Percentile(stats.latencySamplesMs, 0.95);
        score.frameDropPenalty =
            static_cast<double>(stats.droppedFrames) * 8.0 +
            static_cast<double>(stats.deadlineDroppedFrames) * 12.0 +
            FrameDropRate(stats) * 500.0;
        score.outputQueuePenalty =
            static_cast<double>(stats.outputQueueDroppedFrames) * 5.0;
        score.nackExpirePenalty =
            static_cast<double>(stats.nackExpired) * 3.0;
        score.fecOverheadPenalty =
            static_cast<double>(stats.fecParity) * 0.03;
        if (stats.fecParity > 0) {
            score.fecInefficiencyPenalty =
                (std::max)(0.0, 0.25 - FecRecoveryEfficiency(stats)) * 20.0;
        }
        score.fecRecoveryBonus =
            static_cast<double>(
                (std::min)(stats.fecRecovered, stats.fecParity)) * 0.50;
        score.fpsBonus = AverageDisplayFps(stats) * 0.75;
        score.qoeBonus = includeQoeBonus
            ? AverageQoeScore(stats) * 8.0
            : 0.0;
        score.qualityPenalty = static_cast<double>(
            QualityPenalty(
                stats.minJpegQuality,
                stats.minFps,
                stats.minBitrateKbps));

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

    void AddRowToPhase(
        ReplayPhaseStats& stats,
        const ReplayRow& row,
        const ReplayRow* previous) {
        stats.samples++;

        if (previous) {
            stats.completedFrames +=
                Delta(row.completedFrames, previous->completedFrames);
            stats.droppedFrames +=
                Delta(row.droppedFrames, previous->droppedFrames);
            stats.deadlineDroppedFrames +=
                Delta(row.deadlineDroppedFrames,
                    previous->deadlineDroppedFrames);
            stats.outputQueueDroppedFrames +=
                Delta(row.outputQueueDroppedFrames,
                    previous->outputQueueDroppedFrames);
            stats.nackSent += Delta(row.nackSent, previous->nackSent);
            stats.nackRecovered += Delta(row.nackRecovered, previous->nackRecovered);
            stats.nackExpired += Delta(row.nackExpired, previous->nackExpired);
            stats.fecParity += Delta(row.fecParity, previous->fecParity);
            stats.fecRecovered += Delta(row.fecRecovered, previous->fecRecovered);
            stats.fecRecoveredChunks +=
                Delta(row.fecRecoveredChunks, previous->fecRecoveredChunks);
        }

        if (row.qualityHold) {
            stats.qualityHoldSamples++;
        }
        if (row.emergencyG2) {
            stats.emergencyG2Samples++;
        }
        if (row.recoveryWorking) {
            stats.recoveryWorkingSamples++;
        }
        if (row.guardActive) {
            stats.guardSamples++;
        }
        if (!row.decisionReason.empty()) {
            stats.decisionCounts[row.decisionReason]++;
        }
        if (!row.holdReason.empty()) {
            stats.holdCounts[row.holdReason]++;
        }
        if (row.targetJpegQuality > 0) {
            stats.minJpegQuality =
                (std::min)(stats.minJpegQuality, row.targetJpegQuality);
        }
        if (row.targetFps > 0.0) {
            stats.minFps = (std::min)(stats.minFps, row.targetFps);
        }
        if (row.targetBitrateKbps > 0) {
            stats.minBitrateKbps =
                (std::min)(stats.minBitrateKbps, row.targetBitrateKbps);
        }
        if (row.displayFps > 0.0) {
            stats.displayFpsSum += row.displayFps;
            stats.displayFpsSamples++;
        }
        if (row.qoeScore > 0.0) {
            stats.qoeScoreSum += row.qoeScore;
            stats.qoeScoreSamples++;
        }
        if (row.latencyMs > 0.0) {
            stats.latencySamplesMs.push_back(row.latencyMs);
        }
    }

    std::string ClassifyPhase(
        const ReplayRow& row,
        const ReplayRow* previous,
        bool expiredSeen) {
        if (!previous) {
            return "pre_burst";
        }

        const uint64_t sentDelta = Delta(row.nackSent, previous->nackSent);
        const uint64_t expiredDelta =
            Delta(row.nackExpired, previous->nackExpired);
        const uint64_t parityDelta =
            Delta(row.fecParity, previous->fecParity);
        const uint64_t recoveredDelta =
            Delta(row.fecRecovered, previous->fecRecovered);

        if (expiredDelta > 0) {
            return "burst_defense";
        }

        if (expiredSeen) {
            return "burst_recovery";
        }

        if (sentDelta > 0 || parityDelta > 0 || recoveredDelta > 0 ||
            row.qualityHold || row.recoveryWorking) {
            return "burst_onset";
        }

        return "pre_burst";
    }

    bool LoadRows(
        const std::filesystem::path& path,
        std::vector<ReplayRow>& rows) {
        std::ifstream file(path);
        if (!file) {
            return false;
        }

        std::string headerLine;
        if (!std::getline(file, headerLine)) {
            return false;
        }

        const std::vector<std::string> header = ParseCsvLine(headerLine);
        std::unordered_map<std::string, size_t> columns;
        for (size_t i = 0; i < header.size(); ++i) {
            columns[header[i]] = i;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) {
                continue;
            }

            const std::vector<std::string> fields = ParseCsvLine(line);
            ReplayRow row{};
            row.scenario = ValueAt(fields, columns, "networkExperimentScenarioName");
            if (row.scenario.empty()) {
                row.scenario = ValueAt(fields, columns, "scenarioName");
            }
            if (row.scenario.empty()) {
                row.scenario = "Unknown";
            }

            row.timeSec = ToDouble(ValueAt(fields, columns, "timeSec"));
            row.completedFrames =
                ToUint64(ValueAt(fields, columns, "completedFrames"));
            row.droppedFrames =
                ToUint64(ValueAt(fields, columns, "droppedFrames"));
            row.deadlineDroppedFrames =
                ToUint64(ValueAt(fields, columns, "deadlineDroppedFrames"));
            row.outputQueueDroppedFrames =
                ToUint64(ValueAt(fields, columns, "outputQueueDroppedFrames"));
            row.nackSent = ToUint64(
                ValueAt(fields, columns, "deadlineNackSentFrames"));
            row.nackRecovered = ToUint64(
                ValueAt(fields, columns, "deadlineNackRecoveredFrames"));
            row.nackExpired = ToUint64(
                ValueAt(fields, columns, "deadlineNackExpiredDroppedFrames"));
            if (row.nackExpired == 0) {
                row.nackExpired = ToUint64(ValueAt(
                    fields,
                    columns,
                    "deadlineNackExpiredAfterNackFrames"));
            }
            row.fecParity = ToUint64(
                ValueAt(fields, columns, "fecParityPackets"));
            row.fecRecovered = ToUint64(
                ValueAt(fields, columns, "fecRecoveredFrames"));
            row.fecRecoveredChunks = ToUint64(
                ValueAt(fields, columns, "fecRecoveredChunks"));
            row.fecGroup = ToInt(ValueAt(fields, columns, "fecGroupChunkCount"));
            row.decisionReason =
                ValueAt(fields, columns, "adaptiveFecDecisionReason");
            row.holdReason = ValueAt(fields, columns, "adaptiveFecHoldReason");
            row.qualityHold = ToBool(
                ValueAt(fields, columns, "adaptiveFecQualityHoldActive"));
            row.emergencyG2 = ToBool(
                ValueAt(fields, columns, "adaptiveFecEmergencyG2Active"));
            row.recoveryWorking = ToBool(
                ValueAt(fields, columns, "adaptiveFecRecoveryWorking"));
            row.guardActive = ToBool(
                ValueAt(fields, columns, "adaptiveFecGuardActive"));
            row.recoveryEfficiency = ToDouble(
                ValueAt(fields, columns, "adaptiveFecRecoveryEfficiency"));
            row.parityDelta = ToUint64(
                ValueAt(fields, columns, "adaptiveFecParityPacketDelta"));
            row.recoveredDelta = ToUint64(
                ValueAt(fields, columns, "adaptiveFecRecoveredFrameDelta"));
            row.recoveredChunkDelta = ToUint64(
                ValueAt(fields, columns, "adaptiveFecRecoveredChunkDelta"));
            row.targetJpegQuality =
                ToInt(ValueAt(fields, columns, "adaptiveTargetJpegQuality"));
            row.targetFps =
                ToDouble(ValueAt(fields, columns, "adaptiveTargetFps"));
            row.targetBitrateKbps =
                ToInt(ValueAt(fields, columns, "adaptiveTargetBitrateKbps"));
            row.displayFps = ToDouble(ValueAt(fields, columns, "displayFps"));
            row.latencyMs =
                ToDouble(ValueAt(fields, columns, "currentLatencyMs"));
            row.qoeScore =
                ToDouble(ValueAt(fields, columns, "adaptiveLastQoeScore"));

            rows.push_back(std::move(row));
        }

        return !rows.empty();
    }

    bool LoadSummaryRows(
        const std::filesystem::path& path,
        std::vector<SummaryRow>& rows) {
        std::ifstream file(path);
        if (!file) {
            return false;
        }

        std::string headerLine;
        if (!std::getline(file, headerLine)) {
            return false;
        }

        const std::vector<std::string> header = ParseCsvLine(headerLine);
        std::unordered_map<std::string, size_t> columns;
        for (size_t i = 0; i < header.size(); ++i) {
            columns[header[i]] = i;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) {
                continue;
            }

            const std::vector<std::string> fields = ParseCsvLine(line);
            SummaryRow row{};
            row.scenario = ValueAt(fields, columns, "scenarioName");
            row.warmupSampleCount = static_cast<uint32_t>(
                ToUint64(ValueAt(fields, columns, "warmupSampleCount")));
            row.measuredSampleCount = static_cast<uint32_t>(
                ToUint64(ValueAt(fields, columns, "measuredSampleCount")));
            row.p95LatencyMs =
                ToDouble(ValueAt(fields, columns, "p95LatencyMs"));
            row.avgDisplayFps =
                ToDouble(ValueAt(fields, columns, "avgDisplayFps"));
            row.completedFrames =
                ToUint64(ValueAt(fields, columns, "completedFrames"));
            row.droppedFrames =
                ToUint64(ValueAt(fields, columns, "droppedFrames"));
            row.deadlineDroppedFrames =
                ToUint64(ValueAt(fields, columns, "deadlineDroppedFrames"));
            row.outputQueueDroppedFrames =
                ToUint64(ValueAt(fields, columns, "outputQueueDroppedFrames"));
            row.nackExpired = ToUint64(
                ValueAt(fields, columns, "deadlineNackExpiredDroppedFrames"));
            row.fecParity =
                ToUint64(ValueAt(fields, columns, "fecParityPackets"));
            row.fecRecovered =
                ToUint64(ValueAt(fields, columns, "fecRecoveredFrames"));
            row.minTargetFps =
                ToInt(ValueAt(fields, columns, "minTargetFps"));
            row.minTargetJpegQuality =
                ToInt(ValueAt(fields, columns, "minTargetJpegQuality"));
            row.minTargetBitrateKbps =
                ToInt(ValueAt(fields, columns, "minTargetBitrateKbps"));

            if (!row.scenario.empty()) {
                rows.push_back(std::move(row));
            }
        }

        return !rows.empty();
    }

    void NormalizeMinima(ReplayPhaseStats& stats) {
        if (stats.minJpegQuality == 1000) {
            stats.minJpegQuality = 0;
        }
        if (stats.minFps == 1000.0) {
            stats.minFps = 0.0;
        }
        if (stats.minBitrateKbps == 1000000) {
            stats.minBitrateKbps = 0;
        }
    }

    std::string VerdictFor(const ReplayPhaseStats& stats) {
        if (stats.samples == 0) {
            return "-";
        }

        const double efficiency = stats.fecParity > 0
            ? static_cast<double>(stats.fecRecovered) /
                static_cast<double>(stats.fecParity)
            : 0.0;

        if (stats.nackExpired == 0 && stats.fecRecovered > 0 &&
            efficiency >= 0.15) {
            return "FEC covered";
        }
        if (stats.nackExpired > 0 && stats.qualityHoldSamples > 0) {
            return "hold under risk";
        }
        if (stats.nackExpired > 0) {
            return "deadline risk";
        }
        if (stats.fecParity > 0 && stats.fecRecovered == 0) {
            return "FEC overhead";
        }
        return "stable";
    }

    std::string ScenarioGroupKey(const std::string& scenario) {
        const size_t separator = scenario.find(" / ");
        return separator == std::string::npos
            ? scenario
            : scenario.substr(0, separator);
    }

    std::string EvaluationKey(
        const std::string& scenario,
        const std::string& phase) {
        return scenario + "\n" + phase;
    }

    std::map<std::string, ReplayEvaluation> BuildEvaluations(
        const std::map<std::string, std::map<std::string, ReplayPhaseStats>>&
            statsByScenarioPhase) {
        std::map<std::string, ReplayEvaluation> evaluations;
        std::map<std::string, std::vector<std::string>> groupMembers;

        for (const auto& scenarioEntry : statsByScenarioPhase) {
            const std::string group = ScenarioGroupKey(scenarioEntry.first);
            for (const auto& phaseEntry : scenarioEntry.second) {
                const std::string key =
                    EvaluationKey(scenarioEntry.first, phaseEntry.first);
                ReplayPhaseStats stats = phaseEntry.second;
                NormalizeMinima(stats);
                ReplayEvaluation evaluation{};
                evaluation.score = BuildScoreBreakdown(stats);
                evaluations[key] = evaluation;
                groupMembers[group + "\n" + phaseEntry.first].push_back(key);
            }
        }

        for (const auto& groupEntry : groupMembers) {
            double winnerScore = 0.0;
            bool hasWinner = false;
            std::string winnerKey;

            for (const std::string& key : groupEntry.second) {
                const double score = evaluations[key].score.finalScore;
                if (!hasWinner || score < winnerScore) {
                    hasWinner = true;
                    winnerScore = score;
                    winnerKey = key;
                }
            }

            if (!hasWinner) {
                continue;
            }

            const double warnMargin =
                (std::max)(25.0, std::abs(winnerScore) * 0.15);
            for (const std::string& key : groupEntry.second) {
                ReplayEvaluation& evaluation = evaluations[key];
                const double delta =
                    evaluation.score.finalScore - winnerScore;
                if (key == winnerKey) {
                    evaluation.winner = true;
                    evaluation.verdict = "PASS";
                    evaluation.reason = "phase winner";
                }
                else if (delta <= warnMargin) {
                    evaluation.verdict = "WARN";
                    evaluation.reason = "near winner +" +
                        std::to_string(static_cast<int>(std::lround(delta)));
                }
                else {
                    evaluation.verdict = "FAIL";
                    evaluation.reason = "behind winner +" +
                        std::to_string(static_cast<int>(std::lround(delta)));
                }
            }
        }

        return evaluations;
    }

    std::map<std::string, std::map<std::string, ReplayPhaseStats>>
        BuildTotalStatsByScenario(
            const std::map<std::string, std::vector<ReplayRow>>& byScenario) {
        std::map<std::string, std::map<std::string, ReplayPhaseStats>> totals;

        for (const auto& scenarioEntry : byScenario) {
            ReplayPhaseStats total{};
            total.phase = "total";
            const std::vector<ReplayRow>& rows = scenarioEntry.second;
            for (size_t i = 0; i < rows.size(); ++i) {
                AddRowToPhase(
                    total,
                    rows[i],
                    i > 0 ? &rows[i - 1] : nullptr);
            }
            NormalizeMinima(total);
            totals[scenarioEntry.first]["total"] = std::move(total);
        }

        return totals;
    }

    ReplayPhaseStats BuildMeasuredTotalStats(
        const std::vector<ReplayRow>& rows,
        uint32_t warmupSampleCount) {
        ReplayPhaseStats total{};
        total.phase = "total";

        if (rows.empty()) {
            return total;
        }

        const size_t startIndex =
            (std::min)(static_cast<size_t>(warmupSampleCount), rows.size());
        for (size_t i = startIndex; i < rows.size(); ++i) {
            const ReplayRow* previous =
                i > 0 ? &rows[i - 1] : nullptr;
            AddRowToPhase(total, rows[i], previous);
        }
        NormalizeMinima(total);
        return total;
    }

    void ApplySummaryQualityFloor(
        ReplayPhaseStats& stats,
        const SummaryRow& summary) {
        if (stats.minFps <= 0.0 && summary.minTargetFps > 0) {
            stats.minFps = static_cast<double>(summary.minTargetFps);
        }
        if (stats.minJpegQuality <= 0 && summary.minTargetJpegQuality > 0) {
            stats.minJpegQuality = summary.minTargetJpegQuality;
        }
        if (stats.minBitrateKbps <= 0 && summary.minTargetBitrateKbps > 0) {
            stats.minBitrateKbps = summary.minTargetBitrateKbps;
        }
    }

    ReplayScoreBreakdown BuildSummaryScoreBreakdown(const SummaryRow& row) {
        ReplayPhaseStats stats{};
        stats.completedFrames = row.completedFrames;
        stats.droppedFrames = row.droppedFrames;
        stats.deadlineDroppedFrames = row.deadlineDroppedFrames;
        stats.outputQueueDroppedFrames = row.outputQueueDroppedFrames;
        stats.nackExpired = row.nackExpired;
        stats.fecParity = row.fecParity;
        stats.fecRecovered = row.fecRecovered;
        stats.minFps = static_cast<double>(row.minTargetFps);
        stats.minJpegQuality = row.minTargetJpegQuality;
        stats.minBitrateKbps = row.minTargetBitrateKbps;
        stats.displayFpsSum = row.avgDisplayFps;
        stats.displayFpsSamples = row.avgDisplayFps > 0.0 ? 1 : 0;
        if (row.p95LatencyMs > 0.0) {
            stats.latencySamplesMs.push_back(row.p95LatencyMs);
        }

        return BuildScoreBreakdown(stats, false);
    }

    std::filesystem::path FindSummaryPathForRaw(
        const std::filesystem::path& sourcePath) {
        const std::string overridePath =
            ReadEnvString("TR2_NETWORK_EXPERIMENT_REPLAY_SUMMARY");
        if (!overridePath.empty()) {
            return std::filesystem::absolute(
                std::filesystem::path(overridePath));
        }

        const std::string filename = sourcePath.filename().string();
        const std::string prefix = "network_";
        if (filename.rfind(prefix, 0) != 0 ||
            filename.rfind("network_summary_", 0) == 0) {
            return {};
        }

        const std::filesystem::path summaryPath =
            sourcePath.parent_path() /
            ("network_summary_" + filename.substr(prefix.size()));
        return std::filesystem::exists(summaryPath)
            ? summaryPath
            : std::filesystem::path{};
    }

    std::vector<ConsistencyRow> BuildConsistencyRows(
        const std::map<std::string, std::vector<ReplayRow>>& byScenario,
        const std::vector<SummaryRow>& summaryRows) {
        std::map<std::string, SummaryRow> summariesByScenario;
        std::map<std::string, double> replayScores;
        std::map<std::string, double> reporterScores;
        std::map<std::string, std::string> replayWinnerByGroup;
        std::map<std::string, std::string> reporterWinnerByGroup;

        for (const SummaryRow& summary : summaryRows) {
            summariesByScenario[summary.scenario] = summary;
            reporterScores[summary.scenario] =
                BuildSummaryScoreBreakdown(summary).finalScore;
        }

        for (const auto& scenarioEntry : byScenario) {
            const auto summaryIt = summariesByScenario.find(scenarioEntry.first);
            if (summaryIt == summariesByScenario.end()) {
                continue;
            }

            const ReplayPhaseStats measuredStats = BuildMeasuredTotalStats(
                scenarioEntry.second,
                summaryIt->second.warmupSampleCount);
            ReplayPhaseStats comparableStats = measuredStats;
            ApplySummaryQualityFloor(comparableStats, summaryIt->second);
            replayScores[scenarioEntry.first] =
                BuildScoreBreakdown(comparableStats, false).finalScore;
        }

        auto assignWinners =
            [](const std::map<std::string, double>& scores,
                std::map<std::string, std::string>& winners) {
                std::map<std::string, double> bestScores;
                for (const auto& entry : scores) {
                    const std::string group = ScenarioGroupKey(entry.first);
                    const auto bestIt = bestScores.find(group);
                    if (bestIt == bestScores.end() ||
                        entry.second < bestIt->second) {
                        bestScores[group] = entry.second;
                        winners[group] = entry.first;
                    }
                }
            };

        assignWinners(replayScores, replayWinnerByGroup);
        assignWinners(reporterScores, reporterWinnerByGroup);

        std::vector<ConsistencyRow> rows;
        for (const auto& summaryEntry : summariesByScenario) {
            const std::string& scenario = summaryEntry.first;
            const auto replayIt = replayScores.find(scenario);
            const auto reporterIt = reporterScores.find(scenario);
            if (replayIt == replayScores.end() ||
                reporterIt == reporterScores.end()) {
                continue;
            }

            ConsistencyRow row{};
            row.scenario = scenario;
            row.replayScore = replayIt->second;
            row.reporterScore = reporterIt->second;
            row.scoreDiff = row.replayScore - row.reporterScore;

            const std::string group = ScenarioGroupKey(scenario);
            row.replayWinner = replayWinnerByGroup[group] == scenario;
            row.reporterWinner = reporterWinnerByGroup[group] == scenario;

            const bool winnerMatches =
                replayWinnerByGroup[group] == reporterWinnerByGroup[group];
            const double tolerance =
                (std::max)(25.0, std::abs(row.reporterScore) * 0.20);
            if (winnerMatches && std::abs(row.scoreDiff) <= tolerance) {
                row.verdict = "PASS";
                row.reason = "winner and score aligned";
            }
            else if (winnerMatches) {
                row.verdict = "WARN";
                row.reason = "winner aligned, score drift";
            }
            else {
                row.verdict = "FAIL";
                row.reason = "winner mismatch";
            }

            rows.push_back(std::move(row));
        }

        std::sort(
            rows.begin(),
            rows.end(),
            [](const ConsistencyRow& lhs, const ConsistencyRow& rhs) {
                return lhs.scenario < rhs.scenario;
            });
        return rows;
    }

    bool WriteMarkdown(
        const std::filesystem::path& path,
        const std::filesystem::path& sourcePath,
        const std::map<std::string, std::vector<ReplayRow>>& byScenario,
        const std::map<std::string, std::map<std::string, ReplayPhaseStats>>&
            byPhase,
        const std::filesystem::path& summaryPath,
        const std::vector<ConsistencyRow>& consistencyRows) {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out) {
            return false;
        }

        out << "# Network Experiment Replay Report\n\n";
        out << "- Source CSV: `" << sourcePath.string() << "`\n";
        out << "- Scenario count: " << byScenario.size() << "\n\n";
        if (!summaryPath.empty()) {
            out << "- Reporter summary CSV: `" << summaryPath.string() << "`\n";
            out << "- Consistency scoring: Reporter default weights with QoE bonus omitted because `network_summary_*.csv` does not persist QoE time-series samples.\n\n";
        }

        const auto totalStatsByScenario = BuildTotalStatsByScenario(byScenario);
        const auto totalEvaluations = BuildEvaluations(totalStatsByScenario);
        const auto phaseEvaluations = BuildEvaluations(byPhase);

        out << "## Scenario Summary\n\n";
        out << "| Scenario | Verdict | Winner | Samples | NACK Expired | FEC Recovered | FEC Parity | FEC Eff | Guard Ratio | Recovery Working | Quality Penalty | Final Score | Reason | Dominant Decision |\n";
        out << "|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|\n";

        for (const auto& scenarioEntry : totalStatsByScenario) {
            ReplayPhaseStats total = scenarioEntry.second.at("total");
            NormalizeMinima(total);

            const double efficiency = total.fecParity > 0
                ? static_cast<double>(total.fecRecovered) /
                    static_cast<double>(total.fecParity)
                : 0.0;
            const double guardRatio = total.samples > 0
                ? static_cast<double>(total.guardSamples) /
                    static_cast<double>(total.samples)
                : 0.0;
            const double recoveryWorkingRatio = total.samples > 0
                ? static_cast<double>(total.recoveryWorkingSamples) /
                    static_cast<double>(total.samples)
                : 0.0;
            const int qualityPenalty = QualityPenalty(
                total.minJpegQuality,
                total.minFps,
                total.minBitrateKbps);
            const std::string key = EvaluationKey(scenarioEntry.first, "total");
            const auto evaluationIt = totalEvaluations.find(key);
            const ReplayEvaluation evaluation = evaluationIt != totalEvaluations.end()
                ? evaluationIt->second
                : ReplayEvaluation{};

            out << "| " << scenarioEntry.first
                << " | " << evaluation.verdict
                << " | " << (evaluation.winner ? "yes" : "")
                << " | " << total.samples
                << " | " << total.nackExpired
                << " | " << total.fecRecovered
                << " | " << total.fecParity
                << " | " << std::fixed << std::setprecision(3) << efficiency
                << " | " << std::fixed << std::setprecision(1)
                << guardRatio * 100.0 << "%"
                << " | " << std::fixed << std::setprecision(1)
                << recoveryWorkingRatio * 100.0 << "%"
                << " | " << qualityPenalty
                << " | " << std::fixed << std::setprecision(1)
                << evaluation.score.finalScore
                << " | " << evaluation.reason
                << " | " << Dominant(total.decisionCounts)
                << " |\n";
        }

        out << "\n## Scenario Slice Breakdown\n\n";
        out << "| Scenario | Phase | Verdict | Winner | Samples | NACK Sent | NACK Expired | FEC Recovered | FEC Parity | FEC Eff | Quality Hold | g2 Samples | Quality Penalty | Final Score | Reason | Slice Note | Dominant Decision |\n";
        out << "|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|\n";

        for (const auto& scenarioEntry : byPhase) {
            for (const auto& phaseEntry : scenarioEntry.second) {
                ReplayPhaseStats stats = phaseEntry.second;
                NormalizeMinima(stats);

                const double efficiency = stats.fecParity > 0
                    ? static_cast<double>(stats.fecRecovered) /
                        static_cast<double>(stats.fecParity)
                    : 0.0;
                const int qualityPenalty = QualityPenalty(
                    stats.minJpegQuality,
                    stats.minFps,
                    stats.minBitrateKbps);
                const std::string key =
                    EvaluationKey(scenarioEntry.first, phaseEntry.first);
                const auto evaluationIt = phaseEvaluations.find(key);
                const ReplayEvaluation evaluation =
                    evaluationIt != phaseEvaluations.end()
                    ? evaluationIt->second
                    : ReplayEvaluation{};

                out << "| " << scenarioEntry.first
                    << " | " << phaseEntry.first
                    << " | " << evaluation.verdict
                    << " | " << (evaluation.winner ? "yes" : "")
                    << " | " << stats.samples
                    << " | " << stats.nackSent
                    << " | " << stats.nackExpired
                    << " | " << stats.fecRecovered
                    << " | " << stats.fecParity
                    << " | " << std::fixed << std::setprecision(3)
                    << efficiency
                    << " | " << stats.qualityHoldSamples
                    << " | " << stats.emergencyG2Samples
                    << " | " << qualityPenalty
                    << " | " << std::fixed << std::setprecision(1)
                    << evaluation.score.finalScore
                    << " | " << evaluation.reason
                    << " | " << VerdictFor(stats)
                    << " | " << Dominant(stats.decisionCounts)
                    << " |\n";
            }
        }

        out << "\n## Reporter-Style Score Breakdown\n\n";
        out << "Lower final score wins. This replay score mirrors the NetworkExperimentReporter default scoring weights so fast CSV analysis and full experiment reports point in the same direction.\n\n";
        out << "| Scenario | Phase | Latency | Frame Drop | Output Queue | NACK Expire | FEC Overhead | FEC Inefficiency | FEC Recovery Bonus | FPS Bonus | QoE Bonus | Quality Penalty | Final Score |\n";
        out << "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n";

        for (const auto& scenarioEntry : byPhase) {
            for (const auto& phaseEntry : scenarioEntry.second) {
                const std::string key =
                    EvaluationKey(scenarioEntry.first, phaseEntry.first);
                const auto evaluationIt = phaseEvaluations.find(key);
                if (evaluationIt == phaseEvaluations.end()) {
                    continue;
                }

                const ReplayScoreBreakdown& score = evaluationIt->second.score;
                out << "| " << scenarioEntry.first
                    << " | " << phaseEntry.first
                    << " | " << std::fixed << std::setprecision(1)
                    << score.latencyScore
                    << " | " << score.frameDropPenalty
                    << " | " << score.outputQueuePenalty
                    << " | " << score.nackExpirePenalty
                    << " | " << score.fecOverheadPenalty
                    << " | " << score.fecInefficiencyPenalty
                    << " | " << score.fecRecoveryBonus
                    << " | " << score.fpsBonus
                    << " | " << score.qoeBonus
                    << " | " << score.qualityPenalty
                    << " | " << score.finalScore
                    << " |\n";
            }
        }

        if (!consistencyRows.empty()) {
            uint32_t passCount = 0;
            uint32_t warnCount = 0;
            uint32_t failCount = 0;
            for (const ConsistencyRow& row : consistencyRows) {
                if (row.verdict == "PASS") {
                    passCount++;
                }
                else if (row.verdict == "WARN") {
                    warnCount++;
                }
                else if (row.verdict == "FAIL") {
                    failCount++;
                }
            }

            out << "\n## Replay vs Reporter Consistency\n\n";
            out << "- PASS: " << passCount
                << ", WARN: " << warnCount
                << ", FAIL: " << failCount << "\n\n";
            out << "| Scenario | Verdict | Replay Score | Reporter Score | Diff | Replay Winner | Reporter Winner | Reason |\n";
            out << "|---|---|---:|---:|---:|---|---|---|\n";

            for (const ConsistencyRow& row : consistencyRows) {
                out << "| " << row.scenario
                    << " | " << row.verdict
                    << " | " << std::fixed << std::setprecision(1)
                    << row.replayScore
                    << " | " << row.reporterScore
                    << " | " << row.scoreDiff
                    << " | " << (row.replayWinner ? "yes" : "")
                    << " | " << (row.reporterWinner ? "yes" : "")
                    << " | " << row.reason
                    << " |\n";
            }
        }

        return true;
    }

    bool WritePhaseCsv(
        const std::filesystem::path& path,
        const std::map<std::string, std::map<std::string, ReplayPhaseStats>>&
            byPhase) {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out) {
            return false;
        }

        const auto evaluations = BuildEvaluations(byPhase);

        out << "scenario,phase,samples,nackSent,nackExpired,fecRecovered,"
            << "fecParity,fecEfficiency,qualityHoldSamples,g2Samples,"
            << "qualityPenalty,finalScore,passVerdict,winner,reason,"
            << "sliceNote,dominantDecision,latencyScore,frameDropPenalty,"
            << "outputQueuePenalty,nackExpirePenalty,fecOverheadPenalty,"
            << "fecInefficiencyPenalty,fecRecoveryBonus,fpsBonus,qoeBonus\n";

        for (const auto& scenarioEntry : byPhase) {
            for (const auto& phaseEntry : scenarioEntry.second) {
                ReplayPhaseStats stats = phaseEntry.second;
                NormalizeMinima(stats);

                const double efficiency = stats.fecParity > 0
                    ? static_cast<double>(stats.fecRecovered) /
                        static_cast<double>(stats.fecParity)
                    : 0.0;
                const int qualityPenalty = QualityPenalty(
                    stats.minJpegQuality,
                    stats.minFps,
                    stats.minBitrateKbps);
                const std::string key =
                    EvaluationKey(scenarioEntry.first, phaseEntry.first);
                const auto evaluationIt = evaluations.find(key);
                const ReplayEvaluation evaluation =
                    evaluationIt != evaluations.end()
                    ? evaluationIt->second
                    : ReplayEvaluation{};

                out << EscapeCsv(scenarioEntry.first) << ','
                    << EscapeCsv(phaseEntry.first) << ','
                    << stats.samples << ','
                    << stats.nackSent << ','
                    << stats.nackExpired << ','
                    << stats.fecRecovered << ','
                    << stats.fecParity << ','
                    << std::fixed << std::setprecision(6) << efficiency << ','
                    << stats.qualityHoldSamples << ','
                    << stats.emergencyG2Samples << ','
                    << qualityPenalty << ','
                    << std::fixed << std::setprecision(3)
                    << evaluation.score.finalScore << ','
                    << EscapeCsv(evaluation.verdict) << ','
                    << (evaluation.winner ? 1 : 0) << ','
                    << EscapeCsv(evaluation.reason) << ','
                    << EscapeCsv(VerdictFor(stats)) << ','
                    << EscapeCsv(Dominant(stats.decisionCounts)) << ','
                    << std::fixed << std::setprecision(3)
                    << evaluation.score.latencyScore << ','
                    << evaluation.score.frameDropPenalty << ','
                    << evaluation.score.outputQueuePenalty << ','
                    << evaluation.score.nackExpirePenalty << ','
                    << evaluation.score.fecOverheadPenalty << ','
                    << evaluation.score.fecInefficiencyPenalty << ','
                    << evaluation.score.fecRecoveryBonus << ','
                    << evaluation.score.fpsBonus << ','
                    << evaluation.score.qoeBonus
                    << '\n';
            }
        }

        return true;
    }

    bool WriteConsistencyCsv(
        const std::filesystem::path& path,
        const std::vector<ConsistencyRow>& rows) {
        if (rows.empty()) {
            return true;
        }

        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out) {
            return false;
        }

        out << "scenario,verdict,replayScore,reporterScore,scoreDiff,"
            << "replayWinner,reporterWinner,reason\n";
        for (const ConsistencyRow& row : rows) {
            out << EscapeCsv(row.scenario) << ','
                << EscapeCsv(row.verdict) << ','
                << std::fixed << std::setprecision(3)
                << row.replayScore << ','
                << row.reporterScore << ','
                << row.scoreDiff << ','
                << (row.replayWinner ? 1 : 0) << ','
                << (row.reporterWinner ? 1 : 0) << ','
                << EscapeCsv(row.reason)
                << '\n';
        }

        return true;
    }

} // namespace

    bool RunNetworkExperimentReplayFromEnv(const std::string& outputDirectory) {
        const std::string replayCsv = ReadEnvString("TR2_NETWORK_EXPERIMENT_REPLAY");
        if (replayCsv.empty()) {
            return false;
        }

        const std::filesystem::path sourcePath =
            std::filesystem::absolute(std::filesystem::path(replayCsv));
        std::vector<ReplayRow> rows;
        if (!LoadRows(sourcePath, rows)) {
            std::cerr << "[NetworkExperimentReplay] Failed to load CSV: "
                << sourcePath.string() << "\n";
            return true;
        }

        std::map<std::string, std::vector<ReplayRow>> byScenario;
        for (ReplayRow& row : rows) {
            byScenario[row.scenario].push_back(std::move(row));
        }

        const std::filesystem::path summaryPath =
            FindSummaryPathForRaw(sourcePath);
        std::vector<SummaryRow> summaryRows;
        std::vector<ConsistencyRow> consistencyRows;
        if (!summaryPath.empty() && LoadSummaryRows(summaryPath, summaryRows)) {
            consistencyRows = BuildConsistencyRows(byScenario, summaryRows);
        }

        std::map<std::string, std::map<std::string, ReplayPhaseStats>> byPhase;
        for (const auto& scenarioEntry : byScenario) {
            const std::vector<ReplayRow>& scenarioRows = scenarioEntry.second;
            bool expiredSeen = false;

            for (size_t i = 0; i < scenarioRows.size(); ++i) {
                const ReplayRow* previous =
                    i > 0 ? &scenarioRows[i - 1] : nullptr;
                const std::string phase =
                    ClassifyPhase(scenarioRows[i], previous, expiredSeen);
                ReplayPhaseStats& phaseStats =
                    byPhase[scenarioEntry.first][phase];
                phaseStats.phase = phase;
                AddRowToPhase(phaseStats, scenarioRows[i], previous);

                if (previous &&
                    Delta(scenarioRows[i].nackExpired,
                        previous->nackExpired) > 0) {
                    expiredSeen = true;
                }
            }
        }

        std::error_code ec;
        std::filesystem::path outputPath =
            sourcePath.parent_path().empty()
            ? std::filesystem::absolute(std::filesystem::path(outputDirectory))
            : sourcePath.parent_path();
        const std::string outputOverride =
            ReadEnvString("TR2_NETWORK_EXPERIMENT_REPLAY_OUTPUT");
        if (!outputOverride.empty()) {
            outputPath = std::filesystem::absolute(
                std::filesystem::path(outputOverride));
        }
        std::filesystem::create_directories(outputPath, ec);
        if (ec) {
            std::cerr << "[NetworkExperimentReplay] Failed to create output directory: "
                << outputPath.string() << "\n";
            return true;
        }

        const std::string timestamp = MakeTimestamp();
        const std::filesystem::path markdownPath =
            outputPath /
            ("network_replay_" + timestamp + ".md");
        const std::filesystem::path csvPath =
            outputPath /
            ("network_replay_slices_" + timestamp + ".csv");
        const std::filesystem::path consistencyCsvPath =
            outputPath /
            ("network_replay_consistency_" + timestamp + ".csv");

        const bool markdownWritten =
            WriteMarkdown(
                markdownPath,
                sourcePath,
                byScenario,
                byPhase,
                summaryPath,
                consistencyRows);
        const bool csvWritten = WritePhaseCsv(csvPath, byPhase);
        const bool consistencyCsvWritten =
            WriteConsistencyCsv(consistencyCsvPath, consistencyRows);
        if (!markdownWritten || !csvWritten || !consistencyCsvWritten) {
            std::cerr << "[NetworkExperimentReplay] Failed to write replay output.\n";
            std::cerr << "[NetworkExperimentReplay] Markdown: "
                << markdownPath.string() << " written=" << markdownWritten << "\n";
            std::cerr << "[NetworkExperimentReplay] Phase CSV: "
                << csvPath.string() << " written=" << csvWritten << "\n";
            std::cerr << "[NetworkExperimentReplay] Consistency CSV: "
                << consistencyCsvPath.string()
                << " written=" << consistencyCsvWritten << "\n";
            return true;
        }

        std::cout << "[NetworkExperimentReplay] Report: "
            << markdownPath.string() << "\n";
        std::cout << "[NetworkExperimentReplay] Phase CSV: "
            << csvPath.string() << "\n";
        if (!consistencyRows.empty()) {
            std::cout << "[NetworkExperimentReplay] Consistency CSV: "
                << consistencyCsvPath.string() << "\n";
        }
        return true;
    }

} // namespace net
