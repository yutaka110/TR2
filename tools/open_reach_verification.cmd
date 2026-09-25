@echo off
setlocal
set "REACH_VERIFICATION_PAGE=%~dp0..\docs\Reach_RT_Verification.html"
if not exist "%REACH_VERIFICATION_PAGE%" (
  echo Reach-RT verification page was not found.
  pause
  exit /b 1
)
start "" "%REACH_VERIFICATION_PAGE%"
