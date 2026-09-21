@echo off
setlocal
python "%~dp0run_reach_g1.py" --stage command_udp --build-name reach_g2_link_20260922 --duration 60 --link-config "%~dp0..\config\reach_rt_g2_link.json" --name g2_ui_%RANDOM%_%RANDOM% --show
if errorlevel 1 pause
endlocal
