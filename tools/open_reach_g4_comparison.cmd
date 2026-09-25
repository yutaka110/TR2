@echo off
setlocal
set "REACH_COMPARISON_REPORT=%~dp0..\artifacts\reach_g4_comparison_20260925\verification\final_01\index.html"
if not exist "%REACH_COMPARISON_REPORT%" (
  echo G4-04 comparison report is not available yet.
  pause
  exit /b 1
)
start "" "%REACH_COMPARISON_REPORT%"
