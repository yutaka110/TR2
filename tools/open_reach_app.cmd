@echo off
setlocal
pushd "%~dp0.."
if errorlevel 1 exit /b 1
set "REACH_VIEWER_EXE=%CD%\artifacts\reach_viewer_20260925\bin\Release\GE3.exe"
if not exist "%REACH_VIEWER_EXE%" (
  echo Build the navigation app first: python tools\build_reach_viewer.py
  pause
  popd
  exit /b 1
)
set "TR2_RESEARCH_MODE=legacy"
start "" "%REACH_VIEWER_EXE%"
popd
