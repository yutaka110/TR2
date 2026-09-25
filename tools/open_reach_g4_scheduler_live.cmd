@echo off
setlocal
pushd "%~dp0.."
if errorlevel 1 exit /b 1
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss_fff"') do set "REACH_VIEW_NAME=g4_scheduler_view_%%i"
if not defined REACH_VIEW_NAME goto failed
python tools\run_reach_g1.py --name "%REACH_VIEW_NAME%" --build-name reach_g4_scheduler_20260925 --stage command_udp --task T2 --duration 20 --baseline G4-03 --state-feedback --packet-trace --process-priority above_normal --precise-wait --budget-config config\reach_rt_g2_budget.json --link-config artifacts\reach_g4_scheduler_20260925\verification\matrix_01\normal_T2_link.json --prediction-model artifacts\reach_g4_prediction_20260924\verification\study_02\paths.csv --show
if errorlevel 1 goto failed
popd
exit /b 0
:failed
echo Reach-RT could not start or the run was interrupted. See the message above.
pause
popd
exit /b 1
