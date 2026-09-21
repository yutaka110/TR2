@echo off
setlocal
python "%~dp0run_reach_g1.py" --stage command_udp --name command_ui_%RANDOM%_%RANDOM% --show
if errorlevel 1 pause
endlocal
