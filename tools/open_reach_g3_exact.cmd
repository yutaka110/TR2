@echo off
setlocal
set "REACH_REPORT=%~dp0..\artifacts\reach_g3_exact_20260924\study_03\index.html"
if not exist "%REACH_REPORT%" (
  echo G3-03 report is not available yet.
  exit /b 1
)
start "" "%REACH_REPORT%"
