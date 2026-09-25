@echo off
setlocal
set "REACH_SCHEDULER_REPORT=%~dp0..\artifacts\reach_g4_scheduler_20260925\verification\final_01\index.html"
if not exist "%REACH_SCHEDULER_REPORT%" (
  echo G4-03 verification report is not available yet.
  pause
  exit /b 1
)
start "" "%REACH_SCHEDULER_REPORT%"
