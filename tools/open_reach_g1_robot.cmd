@echo off
cd /d "%~dp0.."
python tools\run_reach_g1.py --stage robot_video --name robot_ui_%RANDOM%_%RANDOM% --show
if errorlevel 1 pause
