#include "AppImGuiLayer.h"

#include "AppRuntimeState.h"
#include "EffectRuntime.h"
#include "PostProcessStack.h"
#include "../network/NetworkStats.h"
#include "../../externals/imgui/imgui.h"
#include "../../externals/imgui/imgui_impl_dx12.h"
#include "../../externals/imgui/imgui_impl_win32.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {
    constexpr size_t kEvaluationScenarioCount = 5;

    struct EvaluationScenarioPreset {
        const char* name = "";
        net::NetworkCondition condition{};
    };

    struct EvaluationScenarioMetrics {
        uint64_t completedFrames = 0;
        uint64_t droppedFrames = 0;
        uint64_t jitterBufferDroppedFrames = 0;

        double fpsSum = 0.0;
        double latencySumMs = 0.0;
        double jitterSumMs = 0.0;
        uint32_t samples = 0;

        std::vector<float> fpsHistory;
        std::vector<float> latencyHistory;
        std::vector<float> completionHistory;
    };

    struct EvaluationGraphState {
        std::array<EvaluationScenarioMetrics, kEvaluationScenarioCount> metrics{};
        net::NetworkStatsSnapshot lastStats{};
        bool hasLastStats = false;
        double lastSampleTime = 0.0;
    };

    EvaluationGraphState g_evaluationGraphState{};

    net::NetworkCondition MakeNetworkCondition(
        bool enabled,
        double lossRate,
        uint32_t minDelayMs,
        uint32_t maxDelayMs,
        uint32_t burstLossLength,
        double duplicateRate = 0.0,
        double reorderRate = 0.0) {
        net::NetworkCondition condition{};
        condition.enabled = enabled;
        condition.lossRate = lossRate;
        condition.duplicateRate = duplicateRate;
        condition.reorderRate = reorderRate;
        condition.minDelayMs = minDelayMs;
        condition.maxDelayMs = maxDelayMs;
        condition.burstLossLength = burstLossLength;
        return condition;
    }

    const std::array<EvaluationScenarioPreset, kEvaluationScenarioCount>& GetEvaluationPresets() {
        static const std::array<EvaluationScenarioPreset, kEvaluationScenarioCount> presets{ {
            { "Baseline", MakeNetworkCondition(false, 0.0, 0, 0, 0) },
            { "10% loss", MakeNetworkCondition(true, 0.10, 0, 0, 0) },
            { "50ms jitter", MakeNetworkCondition(true, 0.0, 0, 50, 0) },
            { "100ms delay", MakeNetworkCondition(true, 0.0, 100, 100, 0) },
            { "Burst loss", MakeNetworkCondition(true, 0.03, 0, 0, 8) },
        } };
        return presets;
    }

    bool NearlyEqual(double lhs, double rhs, double epsilon = 0.0001) {
        return std::fabs(lhs - rhs) <= epsilon;
    }

    bool MatchesEvaluationCondition(
        const net::NetworkCondition& lhs,
        const net::NetworkCondition& rhs) {
        if (lhs.enabled != rhs.enabled) {
            return false;
        }

        if (!lhs.enabled && !rhs.enabled) {
            return true;
        }

        return NearlyEqual(lhs.lossRate, rhs.lossRate) &&
            NearlyEqual(lhs.duplicateRate, rhs.duplicateRate) &&
            NearlyEqual(lhs.reorderRate, rhs.reorderRate) &&
            lhs.minDelayMs == rhs.minDelayMs &&
            lhs.maxDelayMs == rhs.maxDelayMs &&
            lhs.burstLossLength == rhs.burstLossLength;
    }

    int FindEvaluationScenarioIndex(const net::NetworkCondition& condition) {
        const auto& presets = GetEvaluationPresets();
        for (size_t i = 0; i < presets.size(); ++i) {
            if (MatchesEvaluationCondition(condition, presets[i].condition)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    uint64_t DeltaCounter(uint64_t current, uint64_t previous) {
        return current >= previous ? current - previous : 0;
    }

    void PushHistoryValue(std::vector<float>& values, float value) {
        constexpr size_t kMaxHistoryValues = 120;
        values.push_back(value);
        if (values.size() > kMaxHistoryValues) {
            values.erase(values.begin());
        }
    }

    double AverageMetric(double sum, uint32_t samples) {
        return samples > 0 ? sum / static_cast<double>(samples) : 0.0;
    }

    double FrameCompletionRate(const EvaluationScenarioMetrics& metrics) {
        const uint64_t totalFrames = metrics.completedFrames + metrics.droppedFrames;
        if (totalFrames == 0) {
            return 0.0;
        }
        return static_cast<double>(metrics.completedFrames) /
            static_cast<double>(totalFrames);
    }

    void ResetEvaluationGraph() {
        g_evaluationGraphState = EvaluationGraphState{};
    }

    void UpdateEvaluationGraphSamples(const net::NetworkStatsSnapshot& stats) {
        const double now = ImGui::GetTime();
        if (g_evaluationGraphState.hasLastStats &&
            now - g_evaluationGraphState.lastSampleTime < 1.0) {
            return;
        }

        const int scenarioIndex = FindEvaluationScenarioIndex(stats.networkCondition);
        if (scenarioIndex >= 0) {
            EvaluationScenarioMetrics& metrics =
                g_evaluationGraphState.metrics[static_cast<size_t>(scenarioIndex)];

            if (g_evaluationGraphState.hasLastStats) {
                metrics.completedFrames += DeltaCounter(
                    stats.completedFrames,
                    g_evaluationGraphState.lastStats.completedFrames);
                metrics.droppedFrames += DeltaCounter(
                    stats.droppedFrames,
                    g_evaluationGraphState.lastStats.droppedFrames);
                metrics.jitterBufferDroppedFrames += DeltaCounter(
                    stats.jitterBufferDroppedFrames,
                    g_evaluationGraphState.lastStats.jitterBufferDroppedFrames);
            }

            const double fps =
                stats.displayFps > 0.0 ? stats.displayFps : stats.receiveFps;
            metrics.fpsSum += fps;
            metrics.latencySumMs += stats.currentLatencyMs;
            metrics.jitterSumMs += stats.currentJitterMs;
            metrics.samples++;

            PushHistoryValue(metrics.fpsHistory, static_cast<float>(fps));
            PushHistoryValue(metrics.latencyHistory, static_cast<float>(stats.currentLatencyMs));
            PushHistoryValue(
                metrics.completionHistory,
                static_cast<float>(FrameCompletionRate(metrics) * 100.0));
        }

        g_evaluationGraphState.lastStats = stats;
        g_evaluationGraphState.hasLastStats = true;
        g_evaluationGraphState.lastSampleTime = now;
    }

    void DrawMetricBar(const char* label, float value, float maxValue, const char* format) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(115.0f);
        ImGui::ProgressBar(
            maxValue > 0.0f ? (std::min)(1.0f, value / maxValue) : 0.0f,
            ImVec2(135.0f, 0.0f),
            "");
        ImGui::SameLine();
        ImGui::Text(format, value);
    }

    ImVec4 MetricColor(double value, double warnThreshold, double badThreshold) {
        if (value >= badThreshold) {
            return ImVec4(1.0f, 0.35f, 0.30f, 1.0f);
        }
        if (value >= warnThreshold) {
            return ImVec4(1.0f, 0.78f, 0.25f, 1.0f);
        }
        return ImVec4(0.38f, 0.95f, 0.55f, 1.0f);
    }

    ImVec4 CauseColor(const std::string& cause) {
        if (cause == "None" || cause.empty()) {
            return ImVec4(0.38f, 0.95f, 0.55f, 1.0f);
        }
        if (cause == "DecodeLoad" || cause == "DisplayLoad") {
            return ImVec4(1.0f, 0.35f, 0.30f, 1.0f);
        }
        if (cause == "Bandwidth") {
            return ImVec4(0.75f, 0.86f, 1.0f, 1.0f);
        }
        return ImVec4(1.0f, 0.78f, 0.25f, 1.0f);
    }

    void DrawDashboardValue(
        const char* label,
        const char* value,
        const ImVec4& color
    ) {
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(145.0f);
        ImGui::TextColored(color, "%s", value);
    }

    void DrawDashboardDouble(
        const char* label,
        double value,
        const char* suffix,
        const ImVec4& color
    ) {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "%.2f%s", value, suffix);
        DrawDashboardValue(label, buffer, color);
    }

    void DrawDashboardUInt(
        const char* label,
        uint64_t value,
        const ImVec4& color
    ) {
        char buffer[64]{};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%llu",
            static_cast<unsigned long long>(value));
        DrawDashboardValue(label, buffer, color);
    }

    void DrawDashboardSectionHeader(const char* label) {
        ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.0f), "%s", label);
        ImGui::Separator();
    }

    int AdaptiveModeIndexFromName(const std::string& mode) {
        if (mode == "Fixed Quality") {
            return 0;
        }
        if (mode == "Loss Reactive") {
            return 1;
        }
        return 2;
    }

    int CongestionModeIndexFromName(const std::string& mode) {
        if (mode == "Loss Based") {
            return 0;
        }
        if (mode == "Delay Based") {
            return 1;
        }
        return 2;
    }

    int NetworkModeIndexFromMode(net::NetworkRuntimeMode mode) {
        return static_cast<int>(mode);
    }

    void DrawNetworkDemoDashboard(
        AppRuntimeState& runtimeState,
        const net::NetworkStatsSnapshot& stats,
        const std::function<void(int)>& onAdaptiveControlModeChanged,
        const std::function<void(int)>& onCongestionControlModeChanged) {
        if (!ImGui::CollapsingHeader("Demo Dashboard", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }

        const char* networkModes[] = {
            "Loopback",
            "Sender",
            "Receiver",
            "Monitor"
        };
        int networkModeIndex =
            NetworkModeIndexFromMode(runtimeState.networkRuntimeMode);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo(
            "Network Mode",
            &networkModeIndex,
            networkModes,
            IM_ARRAYSIZE(networkModes))) {
            networkModeIndex =
                (std::max)(0, (std::min)(3, networkModeIndex));
            runtimeState.networkRuntimeMode =
                static_cast<net::NetworkRuntimeMode>(networkModeIndex);
            const bool receiveVideo =
                net::NetworkModeCanReceiveVideo(runtimeState.networkRuntimeMode);
            runtimeState.showReceivedVideoInGame = receiveVideo;
            runtimeState.showReceivedVideoPreviewWindow = receiveVideo;
            if (!net::NetworkModeCanRunExperiment(
                runtimeState.networkRuntimeMode)) {
                runtimeState.networkExperimentMode = false;
            }
        }
        DrawDashboardValue(
            "Role",
            stats.networkModeSendingEnabled &&
                stats.networkModeReceivingEnabled
                ? "Sender + Receiver"
                : stats.networkModeSendingEnabled
                ? "Sender"
                : stats.networkModeReceivingEnabled
                ? "Receiver"
                : "Monitor",
            ImVec4(0.75f, 0.86f, 1.0f, 1.0f));
        DrawDashboardValue(
            "Endpoint",
            "listen :50000 / remote 127.0.0.1:50000",
            ImVec4(0.55f, 0.65f, 0.75f, 1.0f));

        const char* adaptiveModes[] = {
            "Fixed Quality",
            "Loss Reactive",
            "QoE/Deadline Adaptive"
        };
        int adaptiveModeIndex =
            AdaptiveModeIndexFromName(stats.adaptiveControlMode);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo(
            "Adaptive Mode",
            &adaptiveModeIndex,
            adaptiveModes,
            IM_ARRAYSIZE(adaptiveModes))) {
            if (onAdaptiveControlModeChanged) {
                onAdaptiveControlModeChanged(adaptiveModeIndex);
            }
        }

        const char* congestionModes[] = {
            "Loss Based",
            "Delay Based",
            "Hybrid"
        };
        int congestionModeIndex =
            CongestionModeIndexFromName(stats.adaptiveCongestionControlMode);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::Combo(
            "Congestion Mode",
            &congestionModeIndex,
            congestionModes,
            IM_ARRAYSIZE(congestionModes))) {
            if (onCongestionControlModeChanged) {
                onCongestionControlModeChanged(congestionModeIndex);
            }
        }
        if (stats.networkExperimentActive) {
            ImGui::Text(
                "Experiment Step: %u/%u  %.1f sec left",
                stats.networkExperimentStepIndex,
                stats.networkExperimentStepCount,
                stats.networkExperimentRemainingSec);
            ImGui::Text("Current: %s",
                stats.networkExperimentScenarioName.empty()
                ? "unknown"
                : stats.networkExperimentScenarioName.c_str());
        }
        else {
            ImGui::TextDisabled("Experiment Step: inactive");
        }

        const ImGuiTableFlags flags =
            ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_SizingStretchSame |
            ImGuiTableFlags_RowBg;

        if (!ImGui::BeginTable("NetworkDemoDashboardTable", 2, flags)) {
            return;
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawDashboardSectionHeader("Transport");
        DrawDashboardDouble(
            "Packet Loss",
            stats.packetLossRate * 100.0,
            "%",
            MetricColor(stats.packetLossRate, 0.03, 0.08));
        DrawDashboardDouble(
            "RTT",
            stats.currentRttMs,
            " ms",
            MetricColor(stats.currentRttMs, 100.0, 180.0));
        DrawDashboardDouble(
            "Jitter",
            stats.currentJitterMs,
            " ms",
            MetricColor(stats.currentJitterMs, 20.0, 40.0));
        DrawDashboardDouble(
            "Feedback Loss",
            stats.transportFeedbackLossRate * 100.0,
            "%",
            MetricColor(stats.transportFeedbackLossRate, 0.03, 0.08));
        DrawDashboardDouble(
            "Throughput",
            stats.throughputMbps,
            " Mbps",
            ImVec4(0.75f, 0.86f, 1.0f, 1.0f));
        DrawDashboardDouble(
            "Est. BW",
            static_cast<double>(stats.estimatedBandwidthBps) /
            1000.0 / 1000.0,
            " Mbps",
            ImVec4(0.75f, 0.86f, 1.0f, 1.0f));
        DrawDashboardUInt(
            "Pacing Queue",
            stats.pacingQueuedPackets,
            stats.pacingQueuedPackets > 32
            ? ImVec4(1.0f, 0.78f, 0.25f, 1.0f)
            : ImVec4(0.75f, 0.86f, 1.0f, 1.0f));

        ImGui::TableNextColumn();
        DrawDashboardSectionHeader("Frame Pipeline");
        DrawDashboardDouble(
            "Receive FPS",
            stats.receiveFps,
            "",
            MetricColor(stats.adaptiveTargetFps > 0
                ? static_cast<double>(stats.adaptiveTargetFps) - stats.receiveFps
                : 0.0,
                5.0,
                10.0));
        DrawDashboardDouble(
            "Decode FPS",
            stats.decodeFps,
            "",
            MetricColor(stats.receiveFps - stats.decodeFps, 3.0, 8.0));
        DrawDashboardDouble(
            "Display FPS",
            stats.displayFps,
            "",
            MetricColor(stats.adaptiveTargetFps > 0
                ? static_cast<double>(stats.adaptiveTargetFps) - stats.displayFps
                : 0.0,
                5.0,
                10.0));
        DrawDashboardUInt(
            "Dropped Frames",
            stats.droppedFrames,
            stats.droppedFrames > 20
            ? ImVec4(1.0f, 0.35f, 0.30f, 1.0f)
            : stats.droppedFrames > 0
            ? ImVec4(1.0f, 0.78f, 0.25f, 1.0f)
            : ImVec4(0.38f, 0.95f, 0.55f, 1.0f));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawDashboardSectionHeader("Adaptive Control");
        DrawDashboardValue(
            "Mode",
            stats.adaptiveControlMode.empty()
            ? "QoE/Deadline Adaptive"
            : stats.adaptiveControlMode.c_str(),
            ImVec4(0.75f, 0.86f, 1.0f, 1.0f));
        DrawDashboardValue(
            "Degradation Cause",
            stats.adaptiveDegradationCause.empty()
            ? "None"
            : stats.adaptiveDegradationCause.c_str(),
            CauseColor(stats.adaptiveDegradationCause));
        DrawDashboardDouble(
            "QoE Score",
            stats.adaptiveLastQoeScore,
            "",
            MetricColor(stats.adaptiveLastQoeScore, 1.0, 2.0));
        DrawDashboardUInt(
            "Target FPS",
            static_cast<uint64_t>((std::max)(0, stats.adaptiveTargetFps)),
            ImVec4(0.75f, 0.86f, 1.0f, 1.0f));
        char qualityBuffer[64]{};
        std::snprintf(
            qualityBuffer,
            sizeof(qualityBuffer),
            "Q%d  %dx%d",
            stats.adaptiveTargetJpegQuality,
            stats.adaptiveTargetWidth,
            stats.adaptiveTargetHeight);
        DrawDashboardValue(
            "Quality / Resolution",
            qualityBuffer,
            ImVec4(0.75f, 0.86f, 1.0f, 1.0f));

        ImGui::TableNextColumn();
        DrawDashboardSectionHeader("Recovery");
        DrawDashboardUInt(
            "ACK",
            stats.ackCount,
            ImVec4(0.75f, 0.86f, 1.0f, 1.0f));
        DrawDashboardUInt(
            "NACK Sent",
            stats.deadlineNackSentFrames,
            stats.deadlineNackSentFrames > 0
            ? ImVec4(1.0f, 0.78f, 0.25f, 1.0f)
            : ImVec4(0.38f, 0.95f, 0.55f, 1.0f));
        DrawDashboardUInt(
            "Retrans Chunks",
            stats.ackRetransmittedChunks,
            ImVec4(0.75f, 0.86f, 1.0f, 1.0f));
        DrawDashboardUInt(
            "Recovered / Expired",
            stats.deadlineNackRecoveredFrames,
            ImVec4(0.38f, 0.95f, 0.55f, 1.0f));
        ImGui::SameLine();
        ImGui::TextColored(
            stats.deadlineNackExpiredDroppedFrames > 0
            ? ImVec4(1.0f, 0.78f, 0.25f, 1.0f)
            : ImVec4(0.55f, 0.65f, 0.75f, 1.0f),
            "/ %llu",
            static_cast<unsigned long long>(stats.deadlineNackExpiredDroppedFrames));
        DrawDashboardUInt(
            "KeyFrameRequest",
            stats.ackKeyFrameRequests,
            stats.ackKeyFrameRequests > 0
            ? ImVec4(1.0f, 0.78f, 0.25f, 1.0f)
            : ImVec4(0.55f, 0.65f, 0.75f, 1.0f));

        ImGui::EndTable();
    }

    void DrawEvaluationGraphBaseline(
        const net::NetworkStatsSnapshot& stats,
        const std::function<void(const net::NetworkCondition&)>& onNetworkConditionChanged) {
        UpdateEvaluationGraphSamples(stats);

        if (!ImGui::CollapsingHeader("Evaluation Graph Baseline", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }

        const auto& presets = GetEvaluationPresets();
        const int currentScenarioIndex = FindEvaluationScenarioIndex(stats.networkCondition);

        ImGui::Text("Current: %s",
            currentScenarioIndex >= 0
            ? presets[static_cast<size_t>(currentScenarioIndex)].name
            : "Custom condition");

        for (size_t i = 0; i < presets.size(); ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }

            if (ImGui::Button(presets[i].name)) {
                if (onNetworkConditionChanged) {
                    onNetworkConditionChanged(presets[i].condition);
                }
                g_evaluationGraphState.lastStats = stats;
                g_evaluationGraphState.hasLastStats = true;
                g_evaluationGraphState.lastSampleTime = ImGui::GetTime();
            }
        }

        if (ImGui::Button("Reset Evaluation")) {
            ResetEvaluationGraph();
        }

        ImGui::Separator();

        if (ImGui::BeginTable(
            "EvaluationGraphTable",
            6,
            ImGuiTableFlags_Borders |
            ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Condition");
            ImGui::TableSetupColumn("Samples");
            ImGui::TableSetupColumn("FPS");
            ImGui::TableSetupColumn("Latency");
            ImGui::TableSetupColumn("JB Drop");
            ImGui::TableSetupColumn("Complete");
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < presets.size(); ++i) {
                const EvaluationScenarioMetrics& metrics = g_evaluationGraphState.metrics[i];

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(presets[i].name);

                ImGui::TableNextColumn();
                ImGui::Text("%u", metrics.samples);

                ImGui::TableNextColumn();
                ImGui::Text("%.1f", AverageMetric(metrics.fpsSum, metrics.samples));

                ImGui::TableNextColumn();
                ImGui::Text("%.1f ms", AverageMetric(metrics.latencySumMs, metrics.samples));

                ImGui::TableNextColumn();
                ImGui::Text("%llu",
                    static_cast<unsigned long long>(metrics.jitterBufferDroppedFrames));

                ImGui::TableNextColumn();
                ImGui::Text("%.1f%%", FrameCompletionRate(metrics) * 100.0);
            }

            ImGui::EndTable();
        }

        ImGui::Separator();

        const char* previewName =
            currentScenarioIndex >= 0
            ? presets[static_cast<size_t>(currentScenarioIndex)].name
            : "Custom condition";
        const EvaluationScenarioMetrics* currentMetrics =
            currentScenarioIndex >= 0
            ? &g_evaluationGraphState.metrics[static_cast<size_t>(currentScenarioIndex)]
            : nullptr;

        ImGui::Text("Live Graph: %s", previewName);
        if (currentMetrics && !currentMetrics->fpsHistory.empty()) {
            ImGui::PlotLines(
                "FPS",
                currentMetrics->fpsHistory.data(),
                static_cast<int>(currentMetrics->fpsHistory.size()),
                0,
                nullptr,
                0.0f,
                60.0f,
                ImVec2(0.0f, 42.0f));
            ImGui::PlotLines(
                "Latency",
                currentMetrics->latencyHistory.data(),
                static_cast<int>(currentMetrics->latencyHistory.size()),
                0,
                nullptr,
                0.0f,
                200.0f,
                ImVec2(0.0f, 42.0f));
            ImGui::PlotLines(
                "Complete",
                currentMetrics->completionHistory.data(),
                static_cast<int>(currentMetrics->completionHistory.size()),
                0,
                nullptr,
                0.0f,
                100.0f,
                ImVec2(0.0f, 42.0f));
        } else {
            ImGui::TextDisabled("Select a preset and let it run for a few seconds.");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Comparison Bars");
        for (size_t i = 0; i < presets.size(); ++i) {
            const EvaluationScenarioMetrics& metrics = g_evaluationGraphState.metrics[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextUnformatted(presets[i].name);
            DrawMetricBar(
                "FPS",
                static_cast<float>(AverageMetric(metrics.fpsSum, metrics.samples)),
                60.0f,
                "%.1f");
            DrawMetricBar(
                "Latency",
                static_cast<float>(AverageMetric(metrics.latencySumMs, metrics.samples)),
                200.0f,
                "%.1f ms");
            DrawMetricBar(
                "Complete",
                static_cast<float>(FrameCompletionRate(metrics) * 100.0),
                100.0f,
                "%.1f%%");
            ImGui::PopID();
        }
    }

    void DrawNetworkMonitorContents(AppRuntimeState& runtimeState,
        const net::NetworkStatsSnapshot& stats,
        const std::function<void(uint32_t)>& onJitterBufferTargetDelayChanged,
        const std::function<void(bool)>& onJitterBufferAutoModeChanged,
        const std::function<void(const net::NetworkCondition&)>& onNetworkConditionChanged,
        const std::function<void(int)>& onAdaptiveControlModeChanged,
        const std::function<void(int)>& onCongestionControlModeChanged) {
        DrawNetworkDemoDashboard(
            runtimeState,
            stats,
            onAdaptiveControlModeChanged,
            onCongestionControlModeChanged);

        if (ImGui::CollapsingHeader("Network Condition Simulator", ImGuiTreeNodeFlags_DefaultOpen)) {
            net::NetworkCondition condition = stats.networkCondition;
            bool changed = false;

            bool enabled = condition.enabled;
            if (ImGui::Checkbox("Enable Simulation", &enabled)) {
                condition.enabled = enabled;
                changed = true;
            }

            float lossPercent = static_cast<float>(condition.lossRate * 100.0);
            if (ImGui::SliderFloat("Loss (%)", &lossPercent, 0.0f, 50.0f, "%.1f")) {
                condition.lossRate = lossPercent / 100.0;
                changed = true;
            }

            float duplicatePercent = static_cast<float>(condition.duplicateRate * 100.0);
            if (ImGui::SliderFloat("Duplicate (%)", &duplicatePercent, 0.0f, 50.0f, "%.1f")) {
                condition.duplicateRate = duplicatePercent / 100.0;
                changed = true;
            }

            float reorderPercent = static_cast<float>(condition.reorderRate * 100.0);
            if (ImGui::SliderFloat("Reorder (%)", &reorderPercent, 0.0f, 50.0f, "%.1f")) {
                condition.reorderRate = reorderPercent / 100.0;
                changed = true;
            }

            int minDelayMs = static_cast<int>(condition.minDelayMs);
            if (ImGui::SliderInt("Min Delay (ms)", &minDelayMs, 0, 500)) {
                condition.minDelayMs = static_cast<uint32_t>((std::max)(0, minDelayMs));
                changed = true;
            }

            int maxDelayMs = static_cast<int>(condition.maxDelayMs);
            if (ImGui::SliderInt("Max Delay / Jitter (ms)", &maxDelayMs, 0, 500)) {
                condition.maxDelayMs = static_cast<uint32_t>((std::max)(0, maxDelayMs));
                changed = true;
            }

            int burstLossLength = static_cast<int>(condition.burstLossLength);
            if (ImGui::SliderInt("Burst Loss Length", &burstLossLength, 0, 32)) {
                condition.burstLossLength = static_cast<uint32_t>((std::max)(0, burstLossLength));
                changed = true;
            }

            if (condition.maxDelayMs < condition.minDelayMs) {
                condition.maxDelayMs = condition.minDelayMs;
                changed = true;
            }

            if (changed && onNetworkConditionChanged) {
                onNetworkConditionChanged(condition);
            }

            ImGui::Separator();

            ImGui::Text("Submitted: %llu",
                static_cast<unsigned long long>(stats.networkSimulation.submittedPackets));
            ImGui::Text("Sent: %llu",
                static_cast<unsigned long long>(stats.networkSimulation.sentPackets));
            ImGui::Text("Dropped: %llu",
                static_cast<unsigned long long>(stats.networkSimulation.droppedPackets));
            ImGui::Text("Duplicated: %llu",
                static_cast<unsigned long long>(stats.networkSimulation.duplicatedPackets));
            ImGui::Text("Reordered: %llu",
                static_cast<unsigned long long>(stats.networkSimulation.reorderedPackets));
            ImGui::Text("Burst Events: %llu",
                static_cast<unsigned long long>(stats.networkSimulation.burstLossEvents));
            ImGui::Text("Pending: %u", stats.networkSimulation.pendingPackets);
        }

        DrawEvaluationGraphBaseline(stats, onNetworkConditionChanged);

        if (ImGui::CollapsingHeader("Packet", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Received Packets: %llu",
                static_cast<unsigned long long>(stats.receivedPackets));

            ImGui::Text("Missing Packets: %llu",
                static_cast<unsigned long long>(stats.missingPackets));

            ImGui::Text("Duplicate Packets: %llu",
                static_cast<unsigned long long>(stats.duplicatePackets));

            ImGui::Text("Reordered Packets: %llu",
                static_cast<unsigned long long>(stats.reorderedPackets));

            ImGui::Text("Received Bytes: %llu",
                static_cast<unsigned long long>(stats.receivedBytes));

            ImGui::Text("Packet Loss Rate: %.2f %%", stats.packetLossRate * 100.0);
        }

        if (ImGui::CollapsingHeader("Frame", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Latest Frame ID: %u", stats.latestFrameId);

            ImGui::Text("Completed Frames: %llu",
                static_cast<unsigned long long>(stats.completedFrames));

            ImGui::Text("Dropped Frames: %llu",
                static_cast<unsigned long long>(stats.droppedFrames));

            ImGui::Text("Deadline Drops: %llu",
                static_cast<unsigned long long>(stats.deadlineDroppedFrames));

            ImGui::Text("Output Queue Drops: %llu",
                static_cast<unsigned long long>(stats.outputQueueDroppedFrames));

            ImGui::Text("Output Drop Events: %llu",
                static_cast<unsigned long long>(stats.outputQueueDropEvents));

            ImGui::Text("Output Drop Burst Events: %llu",
                static_cast<unsigned long long>(stats.outputQueueDropBurstEvents));

            ImGui::Text("Last Output Drop Reason: %s",
                stats.lastOutputQueueDropReason.empty()
                ? "none"
                : stats.lastOutputQueueDropReason.c_str());

            ImGui::Text("Last Output Drop Count/Queue: %u / %u",
                stats.lastOutputQueueDropFrameCount,
                stats.lastOutputQueueDropQueueSize);

            ImGui::Text("Last Output Drop Age old/new: %.2f / %.2f ms",
                stats.lastOutputQueueDropOldestAgeMs,
                stats.lastOutputQueueDropNewestAgeMs);

            ImGui::Text("Max Output Drop Age: %.2f ms",
                stats.maxOutputQueueDropOldestAgeMs);

            ImGui::Text("Frame Drop Rate: %.2f %%", stats.frameDropRate * 100.0);
        }

        if (ImGui::CollapsingHeader("Latency", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Latency Now: %.2f ms", stats.currentLatencyMs);
            ImGui::Text("Latency Avg: %.2f ms", stats.averageLatencyMs);
            ImGui::Text("Latency Max: %.2f ms", stats.maxLatencyMs);
        }

        if (ImGui::CollapsingHeader("RTT", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("RTT Now: %.2f ms", stats.currentRttMs);
            ImGui::Text("RTT Avg: %.2f ms", stats.averageRttMs);
            ImGui::Text("RTT Max: %.2f ms", stats.maxRttMs);

            ImGui::Text("RTT Samples: %llu",
                static_cast<unsigned long long>(stats.rttSamples));
        }

        if (ImGui::CollapsingHeader("Sender ACK", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("ACK Count: %llu",
                static_cast<unsigned long long>(stats.ackCount));

            ImGui::Text("Last ACK Frame ID: %u", stats.lastAckFrameId);

            ImGui::Text("ACK Received Chunks: %u", stats.lastAckReceivedChunks);

            ImGui::Text("ACK Missing Chunks: %u", stats.lastAckMissingChunks);

            ImGui::Text("ACK Missing Rate: %.2f %%", stats.lastAckMissingRate * 100.0);

            ImGui::Separator();

            ImGui::Text("Retransmitted Frames: %llu",
                static_cast<unsigned long long>(stats.ackRetransmittedFrames));

            ImGui::Text("Retransmitted Chunks: %llu",
                static_cast<unsigned long long>(stats.ackRetransmittedChunks));

            ImGui::Text("Stale ACK Drops: %llu",
                static_cast<unsigned long long>(stats.ackStaleDroppedFrames));

            ImGui::Text("KeyFrame Requests: %llu",
                static_cast<unsigned long long>(stats.ackKeyFrameRequests));

            ImGui::Text("KeyFrame Pending: %s",
                stats.ackKeyFramePending ? "true" : "false");

            ImGui::Separator();

            ImGui::Text("Deadline NACK Sent Frames: %llu",
                static_cast<unsigned long long>(stats.deadlineNackSentFrames));

            ImGui::Text("Deadline NACK Recovered Frames: %llu",
                static_cast<unsigned long long>(stats.deadlineNackRecoveredFrames));

            ImGui::Text("Deadline NACK Missing Chunks: %llu",
                static_cast<unsigned long long>(stats.deadlineNackMissingChunks));

            ImGui::Text("Deadline NACK Expired Drops: %llu",
                static_cast<unsigned long long>(stats.deadlineNackExpiredDroppedFrames));

            ImGui::Text("Expired After NACK: %llu",
                static_cast<unsigned long long>(stats.deadlineNackExpiredAfterNackFrames));

            ImGui::Text("Expired Missing Chunks: %llu",
                static_cast<unsigned long long>(stats.deadlineNackExpiredMissingChunks));
        }

        if (ImGui::CollapsingHeader("Pacing", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Enabled: %s", stats.pacingEnabled ? "true" : "false");
            ImGui::Text("Target Bitrate: %.2f Mbps",
                static_cast<double>(stats.pacingTargetBitrateBps) /
                1000.0 / 1000.0);
            ImGui::Text("Queue: %u packets", stats.pacingQueuedPackets);
            ImGui::Text("Queue high/normal: %u / %u",
                stats.pacingHighPriorityQueuedPackets,
                stats.pacingNormalQueuedPackets);
            ImGui::Text("Queue Delay now/max: %.2f / %.2f ms",
                stats.pacingCurrentQueueDelayMs,
                stats.pacingMaxQueueDelayMs);
            ImGui::Text("Enqueued/Sent: %llu / %llu",
                static_cast<unsigned long long>(stats.pacingEnqueuedPackets),
                static_cast<unsigned long long>(stats.pacingSentPackets));
            ImGui::Text("Sent Bytes: %llu",
                static_cast<unsigned long long>(stats.pacingSentBytes));
            ImGui::Text("Dropped total/deadline/overflow: %llu / %llu / %llu",
                static_cast<unsigned long long>(stats.pacingDroppedPackets),
                static_cast<unsigned long long>(stats.pacingDeadlineDroppedPackets),
                static_cast<unsigned long long>(stats.pacingOverflowDroppedPackets));
        }

        if (ImGui::CollapsingHeader("Transport Feedback", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Feedback Packets: %llu",
                static_cast<unsigned long long>(stats.transportFeedbackPackets));
            ImGui::Text("Packet Statuses: %llu",
                static_cast<unsigned long long>(stats.transportFeedbackPacketStatuses));
            ImGui::Text("Received/Missing: %llu / %llu",
                static_cast<unsigned long long>(stats.transportFeedbackReceivedPackets),
                static_cast<unsigned long long>(stats.transportFeedbackMissingPackets));
            ImGui::Text("Feedback Loss: %.2f %%",
                stats.transportFeedbackLossRate * 100.0);
            ImGui::Text("Arrival Jitter: %.2f ms",
                stats.transportFeedbackArrivalJitterMs);
            ImGui::Text("Queue Delay Trend: %.2f ms",
                stats.transportFeedbackQueueDelayTrendMs);
            ImGui::Text("Last Feedback Seq: %u",
                stats.transportFeedbackLastSequence);
        }

        if (ImGui::CollapsingHeader("Bandwidth Estimator", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Estimated Bandwidth: %.2f Mbps",
                static_cast<double>(stats.estimatedBandwidthBps) /
                1000.0 / 1000.0);
            ImGui::Text("Delivery Rate: %.2f Mbps",
                static_cast<double>(stats.deliveryRateBps) /
                1000.0 / 1000.0);
            ImGui::Text("Queue Delay Trend: %.2f ms",
                stats.bandwidthQueueDelayMs);
            ImGui::Text("RTT Trend: %.2f ms",
                stats.bandwidthRttTrendMs);
            ImGui::Text("Loss Trend: %.2f %%",
                stats.bandwidthLossTrend * 100.0);
            ImGui::Text("Jitter Trend: %.2f ms",
                stats.bandwidthJitterTrendMs);
            ImGui::Text("Feedback Samples: %llu",
                static_cast<unsigned long long>(
                    stats.bandwidthFeedbackSamples));
        }

        if (ImGui::CollapsingHeader("Adaptive Streaming", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Enabled: %s", stats.adaptiveEnabled ? "true" : "false");
            ImGui::Text("Control Mode: %s",
                stats.adaptiveControlMode.empty()
                ? "QoE/Deadline Adaptive"
                : stats.adaptiveControlMode.c_str());
            ImGui::Text("Congestion Mode: %s",
                stats.adaptiveCongestionControlMode.empty()
                ? "Hybrid"
                : stats.adaptiveCongestionControlMode.c_str());

            ImGui::Separator();

            ImGui::Text("Target JPEG Quality: %d", stats.adaptiveTargetJpegQuality);
            ImGui::Text("Target FPS: %d", stats.adaptiveTargetFps);
            ImGui::Text("Target Bitrate: %d kbps", stats.adaptiveTargetBitrateKbps);
            ImGui::Text("Bandwidth Ceiling: %d kbps",
                stats.adaptiveBandwidthCeilingKbps);
            ImGui::Text("Target Resolution: %dx%d",
                stats.adaptiveTargetWidth,
                stats.adaptiveTargetHeight);
            ImGui::Text("Encoded Size: %llu bytes",
                static_cast<unsigned long long>(stats.adaptiveEncodedFrameBytes));
            ImGui::Text("Raw Size: %llu bytes",
                static_cast<unsigned long long>(stats.adaptiveRawFrameBytes));
            ImGui::Text("Compression Ratio: %.2f %%",
                stats.adaptiveCompressionRatio * 100.0);

            ImGui::Separator();

            ImGui::Text("Quality Changed: %s", stats.adaptiveQualityChanged ? "true" : "false");
            ImGui::Text("FPS Changed: %s", stats.adaptiveFpsChanged ? "true" : "false");
            ImGui::Text("Bitrate Changed: %s", stats.adaptiveBitrateChanged ? "true" : "false");
            ImGui::Text("Resolution Changed: %s", stats.adaptiveResolutionChanged ? "true" : "false");

            ImGui::Separator();

            ImGui::Text("Input ACK Missing Rate: %.2f %%",
                stats.adaptiveLastAckMissingRate * 100.0);

            ImGui::Text("Input Packet Loss Rate: %.2f %%",
                stats.adaptiveLastPacketLossRate * 100.0);

            ImGui::Text("Input RTT: %.2f ms", stats.adaptiveLastRttMs);
            ImGui::Text("Input Latency: %.2f ms", stats.adaptiveLastLatencyMs);
            ImGui::Text("Input Jitter: %.2f ms", stats.adaptiveLastJitterMs);
            ImGui::Text("Input Receive FPS: %.2f", stats.adaptiveLastReceiveFps);
            ImGui::Text("Input Decode FPS: %.2f", stats.adaptiveLastDecodeFps);
            ImGui::Text("Input Display FPS: %.2f", stats.adaptiveLastDisplayFps);
            ImGui::Text("QoE Score: %.2f", stats.adaptiveLastQoeScore);
            ImGui::Text("Degradation Cause: %s",
                stats.adaptiveDegradationCause.empty()
                ? "None"
                : stats.adaptiveDegradationCause.c_str());
        }

        if (ImGui::CollapsingHeader("Jitter", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Jitter Now: %.2f ms", stats.currentJitterMs);
            ImGui::Text("Jitter Avg: %.2f ms", stats.averageJitterMs);
            ImGui::Text("Jitter Max: %.2f ms", stats.maxJitterMs);
        }

        if (ImGui::CollapsingHeader("Jitter Buffer", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool autoMode = stats.jitterBufferAutoModeEnabled;

            if (ImGui::Checkbox("Auto Mode", &autoMode)) {
                if (onJitterBufferAutoModeChanged) {
                    onJitterBufferAutoModeChanged(autoMode);
                }
            }

            ImGui::Separator();

            int targetDelayMs =
                static_cast<int>(stats.jitterBufferTargetDelayMs);

            if (stats.jitterBufferAutoModeEnabled) {
                ImGui::Text("Target Delay: %u ms", stats.jitterBufferTargetDelayMs);
                ImGui::Text("Auto Calculated Delay: %u ms",
                    stats.jitterBufferAutoCalculatedDelayMs);
            }
            else {
                if (ImGui::SliderInt(
                    "Target Delay (ms)",
                    &targetDelayMs,
                    0,
                    120
                )) {
                    if (onJitterBufferTargetDelayChanged) {
                        onJitterBufferTargetDelayChanged(
                            static_cast<uint32_t>(targetDelayMs)
                        );
                    }
                }

                ImGui::Text("Current Target Delay: %u ms", stats.jitterBufferTargetDelayMs);
            }

            ImGui::Text("Buffered Frames: %u", stats.jitterBufferBufferedFrames);

            ImGui::Text("Released Frames: %llu",
                static_cast<unsigned long long>(stats.jitterBufferReleasedFrames));

            ImGui::Text("Dropped Frames: %llu",
                static_cast<unsigned long long>(stats.jitterBufferDroppedFrames));

            ImGui::Separator();

            if (stats.jitterBufferAutoModeEnabled) {
                ImGui::Text("Mode: Auto adaptive jitter control");
            }
            else if (stats.jitterBufferTargetDelayMs <= 10) {
                ImGui::Text("Mode: Ultra low latency / weak jitter tolerance");
            }
            else if (stats.jitterBufferTargetDelayMs <= 40) {
                ImGui::Text("Mode: Balanced realtime");
            }
            else {
                ImGui::Text("Mode: Stability priority / higher latency");
            }

            if (stats.jitterBufferBufferedFrames == 0) {
                ImGui::Text("State: Low buffer / realtime");
            }
            else if (stats.jitterBufferBufferedFrames <= 2) {
                ImGui::Text("State: Stable");
            }
            else {
                ImGui::Text("State: Absorbing jitter");
            }
        }

        if (ImGui::CollapsingHeader("FPS", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Receive FPS: %.2f", stats.receiveFps);
            ImGui::Text("Decode FPS: %.2f", stats.decodeFps);
            ImGui::Text("Display FPS: %.2f", stats.displayFps);
        }

        if (ImGui::CollapsingHeader("Bandwidth", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Bitrate: %.2f Mbps", stats.bitrateMbps);
            ImGui::Text("Throughput: %.2f Mbps", stats.throughputMbps);
            ImGui::Text("Estimated Bandwidth: %.2f Mbps",
                static_cast<double>(stats.estimatedBandwidthBps) /
                1000.0 / 1000.0);
            ImGui::Text("Delivery Rate: %.2f Mbps",
                static_cast<double>(stats.deliveryRateBps) /
                1000.0 / 1000.0);
        }
    }

const char* EffectTypeLabel(EffectComponentType type) {
    switch (type) {
    case EffectComponentType::Particle:
        return "Particle";
    case EffectComponentType::Trail:
        return "Trail";
    case EffectComponentType::Beam:
        return "Beam";
    case EffectComponentType::Distortion:
        return "Distortion";
    }
    return "Unknown";
}

bool DrawEffectTypeControls(EffectParticleSettings& particle) {
    bool changed = false;
    changed |= ImGui::SliderFloat("Depth Fade", &particle.depthFadeSoftness, 0.001f, 0.1f, "%.3f");
    changed |= ImGui::SliderFloat("Particle Edge", &particle.edgeSoftness, 0.0f, 1.0f, "%.2f");
    return changed;
}

bool DrawEffectTypeControls(EffectTrailSettings& trail) {
    bool changed = false;
    changed |= ImGui::SliderFloat("Depth Fade", &trail.depthFadeSoftness, 0.001f, 0.1f, "%.3f");
    changed |= ImGui::SliderFloat("Trail Tail", &trail.trailTailFade, 0.1f, 4.0f, "%.2f");
    return changed;
}

bool DrawEffectTypeControls(EffectDistortionSettings& distortion) {
    bool changed = false;
    changed |= ImGui::SliderFloat("Depth Fade", &distortion.depthFadeSoftness, 0.001f, 0.1f, "%.3f");
    changed |= ImGui::SliderFloat("Distortion Attenuation", &distortion.depthAttenuation, 0.1f, 4.0f, "%.2f");
    return changed;
}

bool DrawEffectTypeControls(EffectBeamSettings&) {
    ImGui::TextDisabled("Beam has no type-specific live controls yet.");
    return false;
}

void ResetEffectTypeToAssetDefault(EffectParticleSettings& particle, const EffectAsset& asset) {
    particle.depthFadeSoftness = asset.defaultParticle.depthFadeSoftness;
    particle.edgeSoftness = asset.defaultParticle.edgeSoftness;
}

void ResetEffectTypeToAssetDefault(EffectTrailSettings& trail, const EffectAsset& asset) {
    trail.depthFadeSoftness = asset.defaultTrail.depthFadeSoftness;
    trail.trailTailFade = asset.defaultTrail.trailTailFade;
}

void ResetEffectTypeToAssetDefault(EffectDistortionSettings& distortion, const EffectAsset& asset) {
    distortion.depthFadeSoftness = asset.defaultDistortion.depthFadeSoftness;
    distortion.depthAttenuation = asset.defaultDistortion.depthAttenuation;
}

template <typename ComponentAsset>
bool DrawEffectComponentNode(ComponentAsset& component) {
    if (component.common.id == 0 && component.common.name.empty()) {
        return false;
    }

    bool changed = false;
    ImGui::PushID(static_cast<int>(component.common.id));
    if (ImGui::TreeNode(component.common.name.c_str())) {
        ImGui::Text("tech=%d renderer=%d sim=%d layer=%d queue=%u start=%.2f duration=%.2f",
            static_cast<int>(component.common.technique),
            static_cast<int>(component.common.rendererType),
            static_cast<int>(component.common.simulationType),
            static_cast<int>(component.common.layer),
            component.common.passState.renderQueue,
            component.common.startTime,
            component.common.duration);
        changed = DrawEffectTypeControls(component.settings);
        ImGui::TreePop();
    }
    ImGui::PopID();
    return changed;
}

ParticleComponentAsset ToComponentAsset(const ParticleComponentAssetView& view) {
    if (!view) {
        return {};
    }
    return {*view.common, *view.settings};
}

TrailComponentAsset ToComponentAsset(const TrailComponentAssetView& view) {
    if (!view) {
        return {};
    }
    return {*view.common, *view.settings};
}

BeamComponentAsset ToComponentAsset(const BeamComponentAssetView& view) {
    if (!view) {
        return {};
    }
    return {*view.common, *view.settings};
}

DistortionComponentAsset ToComponentAsset(const DistortionComponentAssetView& view) {
    if (!view) {
        return {};
    }
    return {*view.common, *view.settings};
}

template <typename ComponentAsset, typename DrawFn>
void QueueEditedComponent(
    std::vector<ComponentAsset>& replacements,
    ComponentAsset component,
    DrawFn drawFn) {
    if (drawFn(component)) {
        replacements.push_back(component);
    }
}

void ApplyReplacements(
    EffectAsset& asset,
    const std::vector<ParticleComponentAsset>& replacements) {
    const MutableParticleComponentStorageView storage = asset.MutableComponents().MutableParticleStorageView();
    for (const ParticleComponentAsset& replacement : replacements) {
        ReplaceParticleComponentAndSyncPacked(storage, replacement);
    }
}

void ApplyReplacements(
    EffectAsset& asset,
    const std::vector<TrailComponentAsset>& replacements) {
    const MutableTrailComponentStorageView storage = asset.MutableComponents().MutableTrailStorageView();
    for (const TrailComponentAsset& replacement : replacements) {
        ReplaceTrailComponentAndSyncPacked(storage, replacement);
    }
}

void ApplyReplacements(
    EffectAsset& asset,
    const std::vector<BeamComponentAsset>& replacements) {
    const MutableBeamComponentStorageView storage = asset.MutableComponents().MutableBeamStorageView();
    for (const BeamComponentAsset& replacement : replacements) {
        ReplaceBeamComponentAndSyncPacked(storage, replacement);
    }
}

void ApplyReplacements(
    EffectAsset& asset,
    const std::vector<DistortionComponentAsset>& replacements) {
    const MutableDistortionComponentStorageView storage = asset.MutableComponents().MutableDistortionStorageView();
    for (const DistortionComponentAsset& replacement : replacements) {
        ReplaceDistortionComponentAndSyncPacked(storage, replacement);
    }
}

void DrawParticleTypeSection(EffectAsset& asset) {
    if (!HasParticleComponents(asset)) {
        return;
    }

    if (ImGui::TreeNode(EffectTypeLabel(EffectComponentType::Particle))) {
        std::vector<ParticleComponentAsset> replacements;
        if (ImGui::Button("Reset to Asset Default")) {
            ForEachParticleComponent(asset.Components().ParticleStorageView(), [&asset, &replacements](const ParticleComponentAssetView& particle) {
                ParticleComponentAsset replacement = ToComponentAsset(particle);
                ResetEffectTypeToAssetDefault(replacement.settings, asset);
                replacements.push_back(replacement);
            });
        }
        ImGui::Separator();
        ForEachParticleComponent(asset.Components().ParticleStorageView(), [&replacements](const ParticleComponentAssetView& particle) {
            QueueEditedComponent(replacements, ToComponentAsset(particle), DrawEffectComponentNode<ParticleComponentAsset>);
        });
        ApplyReplacements(asset, replacements);
        ImGui::TreePop();
    }
}

void DrawTrailTypeSection(EffectAsset& asset) {
    if (!HasTrailComponents(asset)) {
        return;
    }

    if (ImGui::TreeNode(EffectTypeLabel(EffectComponentType::Trail))) {
        std::vector<TrailComponentAsset> replacements;
        if (ImGui::Button("Reset to Asset Default")) {
            ForEachTrailComponent(asset.Components().TrailStorageView(), [&asset, &replacements](const TrailComponentAssetView& trail) {
                TrailComponentAsset replacement = ToComponentAsset(trail);
                ResetEffectTypeToAssetDefault(replacement.settings, asset);
                replacements.push_back(replacement);
            });
        }
        ImGui::Separator();
        ForEachTrailComponent(asset.Components().TrailStorageView(), [&replacements](const TrailComponentAssetView& trail) {
            QueueEditedComponent(replacements, ToComponentAsset(trail), DrawEffectComponentNode<TrailComponentAsset>);
        });
        ApplyReplacements(asset, replacements);
        ImGui::TreePop();
    }
}

void DrawDistortionTypeSection(EffectAsset& asset) {
    if (!HasDistortionComponents(asset)) {
        return;
    }

    if (ImGui::TreeNode(EffectTypeLabel(EffectComponentType::Distortion))) {
        std::vector<DistortionComponentAsset> replacements;
        if (ImGui::Button("Reset to Asset Default")) {
            ForEachDistortionComponent(asset.Components().DistortionStorageView(), [&asset, &replacements](const DistortionComponentAssetView& distortion) {
                DistortionComponentAsset replacement = ToComponentAsset(distortion);
                ResetEffectTypeToAssetDefault(replacement.settings, asset);
                replacements.push_back(replacement);
            });
        }
        ImGui::Separator();
        ForEachDistortionComponent(asset.Components().DistortionStorageView(), [&replacements](const DistortionComponentAssetView& distortion) {
            QueueEditedComponent(replacements, ToComponentAsset(distortion), DrawEffectComponentNode<DistortionComponentAsset>);
        });
        ApplyReplacements(asset, replacements);
        ImGui::TreePop();
    }
}

void DrawBeamTypeSection(EffectAsset& asset) {
    if (!HasBeamComponents(asset)) {
        return;
    }

    if (ImGui::TreeNode(EffectTypeLabel(EffectComponentType::Beam))) {
        std::vector<BeamComponentAsset> replacements;
        ForEachBeamComponent(asset.Components().BeamStorageView(), [&replacements](const BeamComponentAssetView& beam) {
            QueueEditedComponent(replacements, ToComponentAsset(beam), DrawEffectComponentNode<BeamComponentAsset>);
        });
        ApplyReplacements(asset, replacements);
        ImGui::TreePop();
    }
}

void DrawEffectTypeSection(EffectAsset& asset, EffectComponentType type) {
    switch (type) {
    case EffectComponentType::Particle:
        DrawParticleTypeSection(asset);
        break;
    case EffectComponentType::Trail:
        DrawTrailTypeSection(asset);
        break;
    case EffectComponentType::Distortion:
        DrawDistortionTypeSection(asset);
        break;
    case EffectComponentType::Beam:
        DrawBeamTypeSection(asset);
        break;
    }
}

void DrawPreviewImage(const char* label, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    if (handle.ptr == 0) {
        ImGui::TextDisabled("%s unavailable", label);
        return;
    }

    ImGui::Text("%s", label);
    ImGui::Image(
        reinterpret_cast<ImTextureID>(handle.ptr),
        ImVec2(160.0f, 90.0f));
}

void DrawReceivedVideoBackgroundOverlay(
    const AppRuntimeState& runtimeState,
    D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    if (!runtimeState.showReceivedVideoInGame || handle.ptr == 0) {
        return;
    }

    const float width = (std::max)(32.0f, runtimeState.transformSprite.scale.x);
    const float height = (std::max)(18.0f, runtimeState.transformSprite.scale.y);
    const ImVec2 center(
        runtimeState.transformSprite.translate.x,
        runtimeState.transformSprite.translate.y);
    const ImVec2 half(width * 0.5f, height * 0.5f);
    const ImVec2 topLeft(center.x - half.x, center.y - half.y);
    const ImVec2 bottomRight(center.x + half.x, center.y + half.y);

    ImGui::GetBackgroundDrawList()->AddImage(
        reinterpret_cast<ImTextureID>(handle.ptr),
        topLeft,
        bottomRight);
}

void ApplyNetworkExperimentPreset(AppRuntimeState& runtimeState, EffectRuntime& effectRuntime) {
    if (runtimeState.networkExperimentMode) {
        runtimeState.enableVfxRenderPasses = false;
        runtimeState.enablePostProcessPasses = false;
        runtimeState.enableDebugPreviewPasses = false;
        runtimeState.showReceivedVideoPreviewWindow = false;
        runtimeState.showReceivedVideoInGame = true;
        runtimeState.autoPlayVfxDemo = false;
        runtimeState.enableParticles = false;
        effectRuntime.ClearInstances();
    } else {
        runtimeState.enableVfxRenderPasses = true;
        runtimeState.enablePostProcessPasses = true;
        runtimeState.enableDebugPreviewPasses = true;
        runtimeState.showReceivedVideoPreviewWindow = true;
        runtimeState.showReceivedVideoInGame = true;
    }
}

const char* AccessTypeShortLabel(ge3::graphics::RenderResourceAccessType type) {
    using ge3::graphics::RenderResourceAccessType;
    switch (type) {
    case RenderResourceAccessType::ReadSrv:
        return "SRV";
    case RenderResourceAccessType::ReadUav:
        return "UAV";
    case RenderResourceAccessType::ReadDepth:
        return "Depth";
    case RenderResourceAccessType::ReadIndirect:
        return "Indirect";
    case RenderResourceAccessType::WriteRtv:
        return "RT";
    case RenderResourceAccessType::WriteDepth:
        return "DepthW";
    case RenderResourceAccessType::WriteUav:
        return "UAVW";
    }
    return "?";
}

std::string BuildPassOutputsSummary(const ge3::graphics::RenderPassDebugInfo& info) {
    std::ostringstream stream;
    bool first = true;
    for (const auto& access : info.accesses) {
        if (access.type != ge3::graphics::RenderResourceAccessType::WriteRtv &&
            access.type != ge3::graphics::RenderResourceAccessType::WriteUav &&
            access.type != ge3::graphics::RenderResourceAccessType::WriteDepth) {
            continue;
        }
        if (!first) {
            stream << ", ";
        }
        first = false;
        stream << AccessTypeShortLabel(access.type) << ":" << access.resource;
    }
    if (!info.depthTarget.empty()) {
        if (!first) {
            stream << ", ";
        }
        stream << "DSV:" << info.depthTarget;
    }
    return stream.str();
}

std::string BuildPassTooltip(const ge3::graphics::RenderPassDebugInfo& info) {
    std::ostringstream stream;
    stream << ge3::graphics::ToString(info.layer) << "." << info.name << "\n";
    stream << (info.executed ? "Executed" : "Culled");
    if (info.executionIndex >= 0) {
        stream << " #" << info.executionIndex;
    }
    if (!info.reason.empty()) {
        stream << " - " << info.reason;
    }
    stream << "\n";
    if (!info.accesses.empty()) {
        stream << "Accesses:\n";
        for (const auto& access : info.accesses) {
            stream << "  " << AccessTypeShortLabel(access.type) <<
                " " << access.resource << "\n";
        }
    }
    if (!info.depthTarget.empty()) {
        stream << "Depth Target: " << info.depthTarget << "\n";
    }
    return stream.str();
}
} // namespace

bool AppImGuiLayer::Initialize(HWND hwnd,
    ID3D12Device* device,
    int bufferCount,
    DXGI_FORMAT rtvFormat,
    ID3D12DescriptorHeap* srvHeap) {
    if (initialized_) {
        return true;
    }

    if (!hwnd || !device || !srvHeap || bufferCount <= 0) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(hwnd)) {
        ImGui::DestroyContext();
        return false;
    }

    if (!ImGui_ImplDX12_Init(device,
        bufferCount,
        rtvFormat,
        srvHeap,
        srvHeap->GetCPUDescriptorHandleForHeapStart(),
        srvHeap->GetGPUDescriptorHandleForHeapStart())) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    initialized_ = true;
    return true;
}

void AppImGuiLayer::BeginFrame() {
    if (!initialized_) {
        return;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void AppImGuiLayer::BuildUi(
    AppRuntimeState& runtimeState,
    EffectRuntime& effectRuntime,
    PostProcessStack& postProcessStack,
    const std::string& renderGraphDescription,
    const std::string& renderGraphError,
    const std::vector<ge3::graphics::RenderPassDebugInfo>& renderPassDebugInfo,
    uint32_t transientTargetCount,
    uint32_t transientTargetStorageCount,
    uint32_t transientBufferCount,
    uint32_t transientBufferStorageCount,
    D3D12_GPU_DESCRIPTOR_HANDLE sceneColorPreview,
    D3D12_GPU_DESCRIPTOR_HANDLE vfxAccumulationPreview,
    D3D12_GPU_DESCRIPTOR_HANDLE postColorPreview,
    D3D12_GPU_DESCRIPTOR_HANDLE depthPreview,
    D3D12_GPU_DESCRIPTOR_HANDLE emissivePreview,
    D3D12_GPU_DESCRIPTOR_HANDLE receivedVideoPreview,
    const net::NetworkStatsSnapshot* networkStats,
    const std::function<void(uint32_t)>& onJitterBufferTargetDelayChanged,
    const std::function<void(bool)>& onJitterBufferAutoModeChanged,
    const std::function<void(const net::NetworkCondition&)>& onNetworkConditionChanged,
    const std::function<void(int)>& onAdaptiveControlModeChanged,
    const std::function<void(int)>& onCongestionControlModeChanged,
    const std::function<void()>& onAddParticle) {
    if (!initialized_) {
        return;
    }

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const float toolbarHeight = 28.0f;
    const float gap = 8.0f;
    const float leftPanelWidth = 360.0f;
    const float rightPanelWidth = 430.0f;
    const float bottomPanelHeight = 285.0f;
    const ImGuiWindowFlags panelFlags =
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse;

    DrawReceivedVideoBackgroundOverlay(runtimeState, receivedVideoPreview);

    if (!runtimeState.showImGui) {
        return;
    }

    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextUnformatted("TR2 Realtime Streaming Lab");
        ImGui::Separator();
        ImGui::Checkbox("UI (F1)", &runtimeState.showImGui);
        ImGui::Separator();
        bool networkExperimentMode = runtimeState.networkExperimentMode;
        if (!net::NetworkModeCanRunExperiment(runtimeState.networkRuntimeMode)) {
            networkExperimentMode = false;
            runtimeState.networkExperimentMode = false;
        }
        if (ImGui::Checkbox("Network Experiment", &networkExperimentMode)) {
            runtimeState.networkExperimentMode = networkExperimentMode;
            if (!net::NetworkModeCanRunExperiment(runtimeState.networkRuntimeMode)) {
                runtimeState.networkExperimentMode = false;
            }
            ApplyNetworkExperimentPreset(runtimeState, effectRuntime);
        }
        ImGui::Separator();
        ImGui::Checkbox("VFX", &runtimeState.enableVfxRenderPasses);
        ImGui::Checkbox("PostFX", &runtimeState.enablePostProcessPasses);
        ImGui::Checkbox("Debug", &runtimeState.enableDebugPreviewPasses);
        ImGui::Checkbox("Video", &runtimeState.showReceivedVideoInGame);
        ImGui::EndMainMenuBar();
    }

    ImGui::SetNextWindowPos(ImVec2(gap, toolbarHeight + gap), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(leftPanelWidth, displaySize.y - toolbarHeight - gap * 2.0f),
        ImGuiCond_Always);
    if (ImGui::Begin("Network Lab", nullptr, panelFlags)) {
        if (runtimeState.showReceivedVideoPreviewWindow) {
            DrawPreviewImage("RNVP Received Texture", receivedVideoPreview);
            ImGui::Separator();
        }
        if (networkStats) {
            DrawNetworkMonitorContents(
                runtimeState,
                *networkStats,
                onJitterBufferTargetDelayChanged,
                onJitterBufferAutoModeChanged,
                onNetworkConditionChanged,
                onAdaptiveControlModeChanged,
                onCongestionControlModeChanged);
        } else {
            ImGui::TextDisabled("Network stats unavailable.");
        }
    }
    ImGui::End();

    ImGui::SetNextWindowPos(
        ImVec2(displaySize.x - rightPanelWidth - gap, toolbarHeight + gap),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(rightPanelWidth, 360.0f),
        ImGuiCond_Always);
    if (ImGui::Begin("Inspector", nullptr, panelFlags)) {
        if (ImGui::BeginTabBar("InspectorTabs")) {
            if (ImGui::BeginTabItem("Scene")) {
                ImGui::ColorEdit4("Material Color",
                    reinterpret_cast<float*>(&runtimeState.materialData.color));
                ImGui::Checkbox("Enable Lighting", reinterpret_cast<bool*>(&runtimeState.materialData.enableLighting));
                ImGui::SliderFloat("Shininess", &runtimeState.materialData.shininess, 1.0f, 64.0f);
                ImGui::Separator();
                ImGui::DragFloat3("Scale", reinterpret_cast<float*>(&runtimeState.transform.scale), 0.01f, 0.01f, 10.0f);
                ImGui::DragFloat3("Rotate", reinterpret_cast<float*>(&runtimeState.transform.rotate), 0.01f, -3.14f, 3.14f);
                ImGui::DragFloat3("Translate", reinterpret_cast<float*>(&runtimeState.transform.translate), 0.01f, -100.0f, 100.0f);
                ImGui::Separator();
                ImGui::ColorEdit3("Light Color", reinterpret_cast<float*>(&runtimeState.directionalLightData.color));
                ImGui::SliderFloat3("Light Direction", reinterpret_cast<float*>(&runtimeState.directionalLightData.direction), -1.0f, 1.0f);
                ImGui::SliderFloat("Intensity", &runtimeState.directionalLightData.intensity, 0.0f, 10.0f);
                ImGui::DragFloat3("Point Pos", &runtimeState.pointLightData.position.x, 0.05f);
                ImGui::DragFloat("Point Intensity", &runtimeState.pointLightData.intensity, 0.05f, 0.0f, 50.0f);
                ImGui::DragFloat("Point Radius", &runtimeState.pointLightData.radius, 0.1f, 0.1f, 100.0f);
                ImGui::DragFloat("Point Decay", &runtimeState.pointLightData.decay, 0.05f, 0.1f, 8.0f);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Video")) {
                ImGui::Checkbox("Show RNVP Video In Game", &runtimeState.showReceivedVideoInGame);
                ImGui::Checkbox("Preview Window", &runtimeState.showReceivedVideoPreviewWindow);
                ImGui::DragFloat("Video Scale X", &runtimeState.transformSprite.scale.x, 1.0f, 32.0f, 1280.0f);
                ImGui::DragFloat("Video Scale Y", &runtimeState.transformSprite.scale.y, 1.0f, 32.0f, 720.0f);
                ImGui::DragFloat("Video Pos X", &runtimeState.transformSprite.translate.x, 1.0f, -1000.0f, 2000.0f);
                ImGui::DragFloat("Video Pos Y", &runtimeState.transformSprite.translate.y, 1.0f, -1000.0f, 2000.0f);
                ImGui::Separator();
                ImGui::DragFloat2("UVTranslate", &runtimeState.uvTransformSprite.translate.x, 0.01f, -10.0f, 10.0f);
                ImGui::DragFloat2("UVScale", &runtimeState.uvTransformSprite.scale.x, 0.01f, -10.0f, 10.0f);
                ImGui::SliderAngle("UVRotate", &runtimeState.uvTransformSprite.rotate.z);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Emitter")) {
                ImGui::Checkbox("Show Particles", &runtimeState.enableParticles);
                ImGui::DragFloat3("EmitterTranslate", &runtimeState.emitter.transform.translate.x, 0.01f, -100.0f, 100.0f);
                ImGui::DragFloat3("Field Accel", &runtimeState.accelerationField.acceleration.x, 0.1f);
                ImGui::DragFloat3("Field Min", &runtimeState.accelerationField.area.min.x, 0.1f);
                ImGui::DragFloat3("Field Max", &runtimeState.accelerationField.area.max.x, 0.1f);
                if (ImGui::Button("Add Particle (Emitter)") && onAddParticle) {
                    onAddParticle();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    const float profilerX = leftPanelWidth + gap * 2.0f;
    const float profilerWidth =
        (std::max)(320.0f, displaySize.x - leftPanelWidth - rightPanelWidth - gap * 4.0f);
    const float profilerY = displaySize.y - bottomPanelHeight - gap;
    ImGui::SetNextWindowPos(ImVec2(profilerX, profilerY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(profilerWidth, bottomPanelHeight), ImGuiCond_Always);
    if (ImGui::Begin("Profiler", nullptr, panelFlags)) {
        int executedPassCount = 0;
        for (const auto& pass : renderPassDebugInfo) {
            if (pass.executed) {
                ++executedPassCount;
            }
        }
        if (renderGraphError.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "RenderGraph: OK");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "RenderGraph: %s", renderGraphError.c_str());
        }
        ImGui::SameLine();
        ImGui::Text("Passes %d/%d  RT %u/%u  Buffers %u/%u",
            executedPassCount,
            static_cast<int>(renderPassDebugInfo.size()),
            transientTargetCount,
            transientTargetStorageCount,
            transientBufferCount,
            transientBufferStorageCount);

        if (ImGui::BeginTable("ProfilerPassActivity", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 165.0f))) {
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, 95.0f);
            ImGui::TableSetupColumn("Pass");
            ImGui::TableSetupColumn("Targets");
            ImGui::TableSetupColumn("Reason");
            ImGui::TableHeadersRow();
            for (const auto& pass : renderPassDebugInfo) {
                const std::string targets = BuildPassOutputsSummary(pass);
                const std::string tooltip = BuildPassTooltip(pass);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(
                    pass.executed ? ImVec4(0.45f, 1.0f, 0.45f, 1.0f) : ImVec4(0.65f, 0.65f, 0.65f, 1.0f),
                    pass.executed ? "ON" : "OFF");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ge3::graphics::ToString(pass.layer));
                ImGui::TableNextColumn();
                ImGui::Text("%s%s", pass.executed ? "" : "(culled) ", pass.name.c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", tooltip.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(targets.empty() ? "-" : targets.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(pass.reason.empty() ? "-" : pass.reason.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();

    ImGui::SetNextWindowPos(
        ImVec2(displaySize.x - rightPanelWidth - gap, toolbarHeight + 376.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(rightPanelWidth, displaySize.y - toolbarHeight - 384.0f - gap),
        ImGuiCond_Always);
    ImGui::Begin("VFX Inspector", nullptr, panelFlags);
    if (ImGui::CollapsingHeader("Render Load Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool networkExperimentMode = runtimeState.networkExperimentMode;
        if (!net::NetworkModeCanRunExperiment(runtimeState.networkRuntimeMode)) {
            networkExperimentMode = false;
            runtimeState.networkExperimentMode = false;
        }
        if (ImGui::Checkbox("Network Experiment Mode", &networkExperimentMode)) {
            runtimeState.networkExperimentMode = networkExperimentMode;
            if (!net::NetworkModeCanRunExperiment(runtimeState.networkRuntimeMode)) {
                runtimeState.networkExperimentMode = false;
            }
            if (runtimeState.networkExperimentMode) {
                runtimeState.enableVfxRenderPasses = false;
                runtimeState.enablePostProcessPasses = false;
                runtimeState.enableDebugPreviewPasses = false;
                runtimeState.showReceivedVideoPreviewWindow = false;
                runtimeState.showReceivedVideoInGame = true;
                runtimeState.autoPlayVfxDemo = false;
                runtimeState.enableParticles = false;
                effectRuntime.ClearInstances();
            } else {
                runtimeState.enableVfxRenderPasses = true;
                runtimeState.enablePostProcessPasses = true;
                runtimeState.enableDebugPreviewPasses = true;
                runtimeState.showReceivedVideoPreviewWindow = true;
                runtimeState.showReceivedVideoInGame = true;
            }
        }

        bool enableVfx = runtimeState.enableVfxRenderPasses;
        if (ImGui::Checkbox("VFX RenderGraph Passes", &enableVfx)) {
            runtimeState.enableVfxRenderPasses = enableVfx;
            runtimeState.networkExperimentMode = false;
            if (!runtimeState.enableVfxRenderPasses) {
                runtimeState.autoPlayVfxDemo = false;
                runtimeState.enableParticles = false;
                effectRuntime.ClearInstances();
            }
        }

        bool enablePostProcess = runtimeState.enablePostProcessPasses;
        if (ImGui::Checkbox("PostProcess Passes", &enablePostProcess)) {
            runtimeState.enablePostProcessPasses = enablePostProcess;
            runtimeState.networkExperimentMode = false;
        }

        bool enableDebugPreview = runtimeState.enableDebugPreviewPasses;
        if (ImGui::Checkbox("Debug Preview Passes", &enableDebugPreview)) {
            runtimeState.enableDebugPreviewPasses = enableDebugPreview;
            runtimeState.networkExperimentMode = false;
        }

        bool showPreviewWindow = runtimeState.showReceivedVideoPreviewWindow;
        if (ImGui::Checkbox("Received Video Preview Window", &showPreviewWindow)) {
            runtimeState.showReceivedVideoPreviewWindow = showPreviewWindow;
            runtimeState.networkExperimentMode = false;
        }

        bool showReceivedVideoInGame = runtimeState.showReceivedVideoInGame;
        if (ImGui::Checkbox("Received Video In Game", &showReceivedVideoInGame)) {
            runtimeState.showReceivedVideoInGame = showReceivedVideoInGame;
            runtimeState.networkExperimentMode = false;
        }
    }

    bool runtimePaused = effectRuntime.IsPaused();
    if (ImGui::Checkbox("Pause Effect Runtime", &runtimePaused)) {
        effectRuntime.SetPaused(runtimePaused);
    }
    float runtimeSpeed = effectRuntime.SpeedMultiplier();
    if (ImGui::SliderFloat("Effect Runtime Speed", &runtimeSpeed, 0.0f, 4.0f)) {
        effectRuntime.SetSpeedMultiplier(runtimeSpeed);
    }
    if (!runtimeState.enableVfxRenderPasses && runtimeState.autoPlayVfxDemo) {
        runtimeState.autoPlayVfxDemo = false;
    }
    ImGui::BeginDisabled(!runtimeState.enableVfxRenderPasses);
    ImGui::Checkbox("Auto Play VFX Demo", &runtimeState.autoPlayVfxDemo);
    ImGui::SliderFloat("Demo Spawn Interval", &runtimeState.autoPlayVfxInterval, 0.1f, 2.0f, "%.2f");
    ImGui::SliderFloat("Demo Spawn Radius", &runtimeState.autoPlayVfxRadius, 0.0f, 8.0f, "%.2f");
    if (ImGui::Button("Play warp_core")) {
        effectRuntime.PlayEffectWithParams(
            "warp_core",
            runtimeState.emitter.transform.translate,
            {1.0f, 0.75f, 0.35f, 1.0f},
            {1.0f, 1.0f, 1.0f});
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear Effects")) {
        effectRuntime.ClearInstances();
    }

    if (ImGui::CollapsingHeader("Effect Assets", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (auto& [name, asset] : effectRuntime.MutableAssets()) {
            ImGui::PushID(name.c_str());
            if (ImGui::TreeNode(name.c_str())) {
                ImGui::Text("shader=%s tex=%s life=%.2f particleEmissive=%.2f",
                    asset.shader.c_str(),
                    asset.texture.c_str(),
                    asset.lifetime,
                    asset.defaultParticle.emissive);
                ImGui::Text("layer=%d queue=%u noise=%.2f pulse=%.2f radius=%.2f",
                    static_cast<int>(asset.layer),
                    asset.passState.renderQueue,
                    asset.defaultParticle.noiseStrength,
                    asset.defaultParticle.pulseSpeed,
                    asset.defaultParticle.spawnRadius);

                if (ImGui::TreeNode("Asset Defaults")) {
                    ImGui::SliderFloat("Default Particle Depth Fade", &asset.defaultParticle.depthFadeSoftness, 0.001f, 0.1f, "%.3f");
                    ImGui::SliderFloat("Default Particle Edge", &asset.defaultParticle.edgeSoftness, 0.0f, 1.0f, "%.2f");
                    ImGui::SliderFloat("Default Trail Depth Fade", &asset.defaultTrail.depthFadeSoftness, 0.001f, 0.1f, "%.3f");
                    ImGui::SliderFloat("Default Trail Tail", &asset.defaultTrail.trailTailFade, 0.1f, 4.0f, "%.2f");
                    ImGui::SliderFloat("Default Distortion Depth Fade", &asset.defaultDistortion.depthFadeSoftness, 0.001f, 0.1f, "%.3f");
                    ImGui::SliderFloat("Default Distortion Attenuation", &asset.defaultDistortion.depthAttenuation, 0.1f, 4.0f, "%.2f");
                    ImGui::TreePop();
                }

                const EffectComponentType orderedTypes[] = {
                    EffectComponentType::Particle,
                    EffectComponentType::Trail,
                    EffectComponentType::Distortion,
                    EffectComponentType::Beam,
                };

                for (EffectComponentType type : orderedTypes) {
                    DrawEffectTypeSection(asset, type);
                }
                ImGui::TreePop();
            }
            ImGui::Separator();
            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader("Runtime Queues", ImGuiTreeNodeFlags_DefaultOpen)) {
        const EffectRuntimeFrame runtimeFrame = effectRuntime.BuildFrame();
        ImGui::Text("active effects=%u active components=%u",
            runtimeFrame.activeInstanceCount,
            runtimeFrame.activeComponentCount);
        ImGui::Text("particles=%u trails=%u beams=%u distortion=%u",
            static_cast<unsigned int>(runtimeFrame.particleQueue.size()),
            static_cast<unsigned int>(runtimeFrame.trailQueue.size()),
            static_cast<unsigned int>(runtimeFrame.beamQueue.size()),
            static_cast<unsigned int>(runtimeFrame.distortionQueue.size()));
    }

    if (ImGui::CollapsingHeader("RenderGraph")) {
        int executedPassCount = 0;
        for (const auto& pass : renderPassDebugInfo) {
            if (pass.executed) {
                ++executedPassCount;
            }
        }
        if (renderGraphError.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Validation: OK");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Validation: %s", renderGraphError.c_str());
        }
        ImGui::Text("Passes: %d / %d executed", executedPassCount, static_cast<int>(renderPassDebugInfo.size()));
        ImGui::Text("Transient Targets: %u (storages: %u)", transientTargetCount, transientTargetStorageCount);
        ImGui::Text("Transient Buffers: %u (storages: %u)", transientBufferCount, transientBufferStorageCount);
        if (ImGui::TreeNodeEx("Pass Activity", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginTable("RenderPassActivity", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 220.0f))) {
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 55.0f);
                ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthFixed, 95.0f);
                ImGui::TableSetupColumn("Pass");
                ImGui::TableSetupColumn("Targets");
                ImGui::TableSetupColumn("Reason");
                ImGui::TableHeadersRow();
                for (const auto& pass : renderPassDebugInfo) {
                    const std::string targets = BuildPassOutputsSummary(pass);
                    const std::string tooltip = BuildPassTooltip(pass);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (pass.executed) {
                        ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.45f, 1.0f), "ON");
                    } else {
                        ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "OFF");
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(ge3::graphics::ToString(pass.layer));
                    ImGui::TableNextColumn();
                    ImGui::Text("%s%s", pass.executed ? "" : "(culled) ", pass.name.c_str());
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", tooltip.c_str());
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(targets.empty() ? "-" : targets.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(pass.reason.empty() ? "-" : pass.reason.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        ImGui::Separator();
        ImGui::BeginChild("RenderGraphDescription", ImVec2(0.0f, 220.0f), true);
        ImGui::TextUnformatted(renderGraphDescription.c_str());
        ImGui::EndChild();
    }

    if (ImGui::CollapsingHeader("Render Targets")) {
        DrawPreviewImage("SceneColor", sceneColorPreview);
        DrawPreviewImage("VfxAccumulation", vfxAccumulationPreview);
        DrawPreviewImage("PostColor", postColorPreview);
        DrawPreviewImage("SceneDepth (Debug)", depthPreview);
        DrawPreviewImage("Emissive Isolation", emissivePreview);
    }

    if (ImGui::CollapsingHeader("Debug Views", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("Depth Preview Near", &runtimeState.debugDepthPreviewNear, 0.01f, 5.0f, "%.2f");
        ImGui::SliderFloat("Depth Preview Far", &runtimeState.debugDepthPreviewFar, 1.0f, 100.0f, "%.1f");
        if (runtimeState.debugDepthPreviewFar <= runtimeState.debugDepthPreviewNear + 0.01f) {
            runtimeState.debugDepthPreviewFar = runtimeState.debugDepthPreviewNear + 0.01f;
        }
        ImGui::SliderFloat("Depth Preview Power", &runtimeState.debugDepthPreviewPower, 0.2f, 4.0f, "%.2f");
        ImGui::SliderFloat("Emissive Preview Boost", &runtimeState.debugEmissivePreviewBoost, 0.1f, 8.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Effect Instances")) {
        std::vector<EffectInstance*> instances;
        for (EffectInstance& instance : effectRuntime.MutableInstances()) {
            if (instance.asset != nullptr) {
                instances.push_back(&instance);
            }
        }

        std::sort(
            instances.begin(),
            instances.end(),
            [](const EffectInstance* lhs, const EffectInstance* rhs) {
                return lhs->id < rhs->id;
            });

        bool selectedInstanceExists = false;
        for (const EffectInstance* instance : instances) {
            if (instance->id == selectedEffectInstanceId_) {
                selectedInstanceExists = true;
                break;
            }
        }
        if (!selectedInstanceExists) {
            selectedEffectInstanceId_ = !instances.empty() ? instances.front()->id : 0;
        }

        if (selectedEffectInstanceId_ != 0) {
            const auto selectedIt = std::find_if(
                instances.begin(),
                instances.end(),
                [this](const EffectInstance* instance) {
                    return instance->id == selectedEffectInstanceId_;
                });
            if (selectedIt != instances.end() && selectedIt != instances.begin()) {
                std::rotate(instances.begin(), selectedIt, selectedIt + 1);
            }
        }

        if (ImGui::BeginTable("EffectInstancesLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Instances", ImGuiTableColumnFlags_WidthFixed, 240.0f);
            ImGui::TableSetupColumn("Selected", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::BeginChild("EffectInstanceList", ImVec2(0.0f, 240.0f), true);
            for (EffectInstance* instance : instances) {
                const bool isSelected = (selectedEffectInstanceId_ == instance->id);
                std::string label = isSelected ?
                    "[Pinned] id=" + std::to_string(instance->id) + " " + instance->asset->name :
                    "id=" + std::to_string(instance->id) + " " + instance->asset->name;
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    selectedEffectInstanceId_ = instance->id;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "age=%.2f  components=%u",
                        instance->age,
                        static_cast<unsigned int>(instance->components.size()));
                }
            }
            ImGui::EndChild();

            ImGui::TableSetColumnIndex(1);
            ImGui::BeginChild("EffectInstanceDetails", ImVec2(0.0f, 240.0f), true);
            EffectInstance* selectedInstance = nullptr;
            for (EffectInstance* instance : instances) {
                if (instance->id == selectedEffectInstanceId_) {
                    selectedInstance = instance;
                    break;
                }
            }

            if (selectedInstance != nullptr) {
                ImGui::PushID(static_cast<int>(selectedInstance->id));
                ImGui::Text("id=%u asset=%s age=%.2f components=%u",
                    selectedInstance->id,
                    selectedInstance->asset->name.c_str(),
                    selectedInstance->age,
                    static_cast<unsigned int>(selectedInstance->components.size()));
                ImGui::DragFloat3("Position", &selectedInstance->transform.translate.x, 0.05f, -100.0f, 100.0f);
                ImGui::DragFloat3("Scale", &selectedInstance->transform.scale.x, 0.02f, 0.01f, 20.0f);
                ImGui::ColorEdit4("Color", &selectedInstance->color.x);
                if (ImGui::Button("Restart")) {
                    effectRuntime.RestartInstance(selectedInstance->id);
                }
                ImGui::SameLine();
                if (ImGui::Button("Stop")) {
                    const uint32_t stoppedId = selectedInstance->id;
                    effectRuntime.StopEffect(stoppedId);
                    selectedEffectInstanceId_ = 0;
                }
                ImGui::PopID();
            } else {
                ImGui::TextDisabled("No effect instance selected.");
            }
            ImGui::EndChild();
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("PostProcess")) {
        for (PostProcessPass& pass : postProcessStack.MutablePasses()) {
            ImGui::PushID(pass.name.c_str());
            ImGui::Checkbox(pass.name.c_str(), &pass.enabled);
            ImGui::SameLine();
            ImGui::SliderFloat("Intensity", &pass.intensity, 0.0f, 4.0f);
            if (pass.pipeline == "BloomExtract") {
                ImGui::SliderFloat("Threshold Min", &pass.parameters.bloomThresholdMin, 0.0f, 2.0f);
                ImGui::SliderFloat("Threshold Max", &pass.parameters.bloomThresholdMax, 0.0f, 4.0f);
                ImGui::SliderFloat("Soft Knee", &pass.parameters.bloomSoftKnee, 0.01f, 1.0f);
            } else if (pass.pipeline == "BloomUpsample") {
                ImGui::SliderFloat("Blend", &pass.parameters.bloomUpsampleBlend, 0.0f, 1.5f);
                ImGui::SliderFloat("Soft Knee", &pass.parameters.bloomUpsampleSoftKnee, 0.01f, 1.0f);
            } else if (pass.pipeline == "BlurHorizontal" || pass.pipeline == "BlurVertical") {
                ImGui::SliderFloat("Blur Radius", &pass.parameters.blurRadius, 1.0f, 8.0f);
            } else if (pass.pipeline == "DistortionComposite") {
                ImGui::SliderFloat("Distortion Scale", &pass.parameters.distortionScale, 0.0f, 0.1f);
            } else if (pass.pipeline == "ToneMapping") {
                ImGui::SliderFloat("Exposure", &pass.parameters.toneExposure, 0.1f, 4.0f);
            } else if (pass.pipeline == "GlowComposite") {
                ImGui::SliderFloat("Glow Weight", &pass.parameters.glowWeight, 0.0f, 4.0f);
                ImGui::ColorEdit3("Glow Tint", &pass.parameters.glowTintR);
            }
            ImGui::Text("  %s -> %s pipeline=%s scale=%.2f",
                pass.inputResource.c_str(),
                pass.outputResource.c_str(),
                pass.pipeline.c_str(),
                pass.resolutionScale);
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void AppImGuiLayer::EndFrame() {
    if (!initialized_) {
        return;
    }

    ImGui::Render();
}

void AppImGuiLayer::Render(ID3D12GraphicsCommandList* cmdList) {
    if (!initialized_ || !cmdList) {
        return;
    }

    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
}

void AppImGuiLayer::Shutdown() {
    if (!initialized_) {
        return;
    }

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    initialized_ = false;
}
