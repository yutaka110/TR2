@echo off
setlocal
python "%~dp0run_reach_g1.py" --stage command_udp --build-name reach_g2_feedback_20260922 --duration 60 --trace-bundle "%~dp0..\config\reach_rt_g2_trace" --budget-config "%~dp0..\config\reach_rt_g2_budget.json" --state-feedback --name g2_feedback_ui_%RANDOM%_%RANDOM% --show
if errorlevel 1 pause
endlocal
