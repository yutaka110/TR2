#include "NetworkExperimentReporter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <ctime>

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

        csvFile_.open(csvPath, std::ios::out | std::ios::trunc);
        if (!csvFile_) {
            csvFilePath_.clear();
            textFilePath_.clear();
            return false;
        }

        textFile_.open(textPath, std::ios::out | std::ios::trunc);
        if (!textFile_) {
            csvFile_.close();
            csvFilePath_.clear();
            textFilePath_.clear();
            return false;
        }

        csvFilePath_ = csvPath.string();
        textFilePath_ = textPath.string();
        headerWritten_ = false;
        ResetCurrent();
        WriteHeader();

        textFile_ << "Network Experiment Summary\n";
        textFile_ << "Generated: " << timestamp << "\n\n";
        textFile_.flush();

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

    void NetworkExperimentReporter::ResetCurrent() {
        current_ = ScenarioAccumulator{};
        hasCurrent_ = false;
    }

    void NetworkExperimentReporter::FinalizeCurrent() {
        if (!hasCurrent_ || current_.sampleCount == 0) {
            return;
        }

        WriteSummary(BuildSummary(current_));
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
            << "simDroppedPackets,"
            << "minTargetFps,"
            << "minTargetJpegQuality,"
            << "minTargetBitrateKbps,"
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
                << summary.simDroppedPackets << ','
                << summary.minTargetFps << ','
                << summary.minTargetJpegQuality << ','
                << summary.minTargetBitrateKbps << ','
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
            textFile_ << "  verdict: " << summary.verdict;
            if (!summary.notes.empty()) {
                textFile_ << " (" << summary.notes << ")";
            }
            textFile_ << "\n\n";
            textFile_.flush();
        }
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
