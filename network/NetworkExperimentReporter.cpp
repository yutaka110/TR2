#include "NetworkExperimentReporter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <limits>
#include <sstream>
#include <ctime>
#include <unordered_map>

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

    std::string FormatDouble(double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3) << value;
        return oss.str();
    }

    std::string FormatPercent(double ratio) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (ratio * 100.0) << "%";
        return oss.str();
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
        if (cause == "DecodeLoad") {
            return 4;
        }
        if (cause == "DisplayLoad") {
            return 5;
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
            return "DecodeLoad";
        case 5:
            return "DisplayLoad";
        case 0:
        default:
            return "None";
        }
    }

} // namespace

    bool NetworkExperimentReporter::Start(const std::string& directory) {
        Stop();

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

        csvFile_.open(csvPath, std::ios::out | std::ios::trunc);
        if (!csvFile_) {
            csvFilePath_.clear();
            textFilePath_.clear();
            markdownFilePath_.clear();
            beforeAfterFilePath_.clear();
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
            previousSummaryCsvPath_.clear();
            generatedTimestamp_.clear();
            summaries_.clear();
            return false;
        }

        csvFilePath_ = csvPath.string();
        textFilePath_ = textPath.string();
        markdownFilePath_ = markdownPath.string();
        beforeAfterFilePath_ = beforeAfterPath.string();
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
        double appTimeSec
    ) {
        if (!csvFile_.is_open() || scenarioName.empty()) {
            return;
        }

        if (!hasCurrent_ || current_.name != scenarioName) {
            FinalizeCurrent();
            ResetCurrent();
            current_.name = scenarioName;
            current_.startTimeSec = appTimeSec;
            current_.minDisplayFps = (std::numeric_limits<double>::max)();
            current_.minTargetFps = (std::numeric_limits<int>::max)();
            current_.minTargetJpegQuality = (std::numeric_limits<int>::max)();
            current_.minTargetBitrateKbps = (std::numeric_limits<int>::max)();
            hasCurrent_ = true;
        }

        current_.endTimeSec = appTimeSec;
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

        current_.adaptiveCauseSamples[
            AdaptiveCauseIndex(stats.adaptiveDegradationCause)]++;

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
            << "endTimeSec,"
            << "durationSec,"
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
            << "simDroppedPackets,"
            << "minTargetFps,"
            << "minTargetJpegQuality,"
            << "minTargetBitrateKbps,"
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
                << summary.endTimeSec << ','
                << summary.durationSec << ','
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
                << summary.simDroppedPackets << ','
                << summary.minTargetFps << ','
                << summary.minTargetJpegQuality << ','
                << summary.minTargetBitrateKbps << ','
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
            textFile_ << "  adaptive dominant cause: "
                << (summary.dominantAdaptiveDegradationCause.empty()
                    ? "None"
                    : summary.dominantAdaptiveDegradationCause)
                << "\n";
            textFile_ << "  deadline nack sent/recovered/missingChunks: "
                << summary.deadlineNackSentFrames << " / "
                << summary.deadlineNackRecoveredFrames << " / "
                << summary.deadlineNackMissingChunks << "\n";
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
            worstP95LatencyMs =
                (std::max)(worstP95LatencyMs, summary.p95LatencyMs);
        }

        file << "# Network Experiment Report\n\n";
        file << "Generated: " << generatedTimestamp_ << "\n\n";

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
            file << "\n";
        }

        file << "## Scenario Results\n\n";
        file
            << "| Scenario | Verdict | Cause | Avg FPS | Min FPS | Avg Latency ms | P95 Latency ms | Deadline Drops | Output Drops | NACK Sent | NACK Recovered | Notes |\n"
            << "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |\n";

        for (const ScenarioSummary& summary : summaries_) {
            file << "| "
                << EscapeMarkdownTable(summary.name) << " | "
                << summary.verdict << " | "
                << EscapeMarkdownTable(
                    summary.dominantAdaptiveDegradationCause.empty()
                    ? "None"
                    : summary.dominantAdaptiveDegradationCause) << " | "
                << FormatDouble(summary.avgDisplayFps) << " | "
                << FormatDouble(summary.minDisplayFps) << " | "
                << FormatDouble(summary.avgLatencyMs) << " | "
                << FormatDouble(summary.p95LatencyMs) << " | "
                << summary.deadlineDroppedFrames << " | "
                << summary.outputQueueDroppedFrames << " | "
                << summary.deadlineNackSentFrames << " | "
                << summary.deadlineNackRecoveredFrames << " | "
                << EscapeMarkdownTable(
                    summary.notes.empty() ? "none" : summary.notes)
                << " |\n";
        }
        file << "\n";

        file << "## Baseline Comparison\n\n";
        if (summaries_.empty()) {
            file << "No baseline is available yet.\n\n";
        }
        else {
            const ScenarioSummary& baseline = summaries_.front();
            file
                << "| Scenario | FPS Delta | P95 Latency Delta ms | Drop Delta | Adaptive Min FPS | Adaptive Min JPEG Quality |\n"
                << "| --- | ---: | ---: | ---: | ---: | ---: |\n";

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
        }

        const double matchedCount =
            (std::max)(1.0, static_cast<double>(matched.size()));

        file << "# Network Before/After Report\n\n";
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
            const double avgFpsDelta =
                (afterFpsSum - beforeFpsSum) / matchedCount;
            const double avgP95Delta =
                (afterP95Sum - beforeP95Sum) / matchedCount;
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
                << " frames.\n\n";
        }

        file << "## Scenario Comparison\n\n";
        file
            << "| Scenario | Cause Before | Cause After | FPS Before | FPS After | FPS Delta | P95 Before ms | P95 After ms | P95 Delta ms | Drops Before | Drops After | Drop Delta | Drop Change | NACK Recovered Before | NACK Recovered After |\n"
            << "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";

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
                << pair.after.deadlineNackRecoveredFrames << " |\n";
        }
        file << "\n";

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

                if (!wroteFinding) {
                    file << "no material metric change";
                }

                file << ".\n";
            }
        }

        file << "\n## Interview-Ready Summary\n\n";
        file << "The engine can now compare two experiment runs directly from persisted CSV summaries. This makes the improvement loop explicit: run the old build, run the new build under the same scenarios, and produce a scenario-matched report showing FPS, latency, drop, and selective retransmission changes.\n";
        file.flush();
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
            summary.endTimeSec =
                ParseDoubleOrDefault(getCell(row, "endTimeSec"));
            summary.durationSec =
                ParseDoubleOrDefault(getCell(row, "durationSec"));
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
            summary.simDroppedPackets =
                ParseUint64OrDefault(getCell(row, "simDroppedPackets"));
            summary.minTargetFps =
                ParseIntOrDefault(getCell(row, "minTargetFps"));
            summary.minTargetJpegQuality =
                ParseIntOrDefault(getCell(row, "minTargetJpegQuality"));
            summary.minTargetBitrateKbps =
                ParseIntOrDefault(getCell(row, "minTargetBitrateKbps"));
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
        summary.endTimeSec = current.endTimeSec;
        summary.durationSec =
            (std::max)(0.0, current.endTimeSec - current.startTimeSec);

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

        summary.completedFrames = current.lastStats.completedFrames;
        summary.displayedFrames = current.lastStats.displayedFrames;
        summary.droppedFrames = current.lastStats.droppedFrames;
        summary.deadlineDroppedFrames =
            current.lastStats.deadlineDroppedFrames;
        summary.outputQueueDroppedFrames =
            current.lastStats.outputQueueDroppedFrames;
        summary.outputQueueDropEvents =
            current.lastStats.outputQueueDropEvents;
        summary.outputQueueDropBurstEvents =
            current.lastStats.outputQueueDropBurstEvents;
        summary.maxOutputQueueDropOldestAgeMs =
            current.lastStats.maxOutputQueueDropOldestAgeMs;
        summary.lastOutputQueueDropReason =
            current.lastStats.lastOutputQueueDropReason;
        summary.ackCount = current.lastStats.ackCount;
        summary.ackRetransmittedFrames =
            current.lastStats.ackRetransmittedFrames;
        summary.deadlineNackSentFrames =
            current.lastStats.deadlineNackSentFrames;
        summary.deadlineNackRecoveredFrames =
            current.lastStats.deadlineNackRecoveredFrames;
        summary.deadlineNackMissingChunks =
            current.lastStats.deadlineNackMissingChunks;
        summary.simDroppedPackets =
            current.lastStats.networkSimulation.droppedPackets;

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

        if (summary.minTargetFps <= 8 || summary.minTargetJpegQuality <= 40) {
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
