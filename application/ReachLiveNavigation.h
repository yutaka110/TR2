#pragma once
#include <string>

namespace reachui {
// Main/UI thread only. A request ends the legacy loop through its normal cleanup.
void RequestResearchLive();
bool ResearchLiveRequested();
void ResetResearchLiveRequest();
// Returns true only when the user requests the normal screen (or launch fails).
bool RunResearchLive(int& exitCode, const std::wstring& diagnostic = {});
std::wstring NavigationDiagnostic();
void NavigationTrace(const char* event, int result = 0);
}
