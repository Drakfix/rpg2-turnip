@echo off
rem Double-click while the game is running on the G2 (Wireless debugging on, screen awake).
rem Records 60 seconds of FPS / GPU data, then shows every saved run side by side.
cd /d "%~dp0"
echo.
echo  Game must already be running on the G2, at the spot you want to measure.
echo.
set "LABEL="
set /p "LABEL=Name for this run (example: fc2-tuned-v4): "
if "%LABEL%"=="" (
  echo No name given, nothing recorded.
  pause
  exit /b 1
)
python scripts\gamemon.py --label "%LABEL%" --seconds 60
echo.
echo ================= all saved runs =================
python scripts\gamemon.py --compare
echo.
pause
