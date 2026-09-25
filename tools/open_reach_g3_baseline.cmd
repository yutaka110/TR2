@echo off
setlocal
set "REACH_REPORT=%~dp0..\artifacts\reach_g3_baseline_v3_20260922\verification\study_01\index.html"
if not exist "%REACH_REPORT%" (
  echo G3-01 comparison report is not available yet.
  pause
  exit /b 1
)
start "" "%REACH_REPORT%"
