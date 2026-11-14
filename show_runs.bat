@echo off
rem Double-click to list every recorded run, grouped by game.
cd /d "%~dp0"
python scripts\gamemon.py --compare
echo.
pause
