@echo off
setlocal
python "%~dp0run_reach_g1.py" --stage command_udp --build-name reach_g1_stable_20260921 --duration 60 --name ui_%RANDOM%_%RANDOM% --show
if errorlevel 1 pause
endlocal
