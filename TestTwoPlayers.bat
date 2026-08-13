@echo off
setlocal

rem Compatibility wrapper for the structured GAS two-player launcher.
rem Usage: TestTwoPlayers.bat [PktLagMs] [PktLossPercent] [Stage] [NoPause] [Auto]
rem Example: TestTwoPlayers.bat 150 0 M5 NoPause Auto

set "PKT_LAG=%~1"
if not defined PKT_LAG set "PKT_LAG=0"
set "PKT_LOSS=%~2"
if not defined PKT_LOSS set "PKT_LOSS=0"
set "STAGE=%~3"
if not defined STAGE set "STAGE=M5"
set "AUTO_SWITCH="
if /I "%~5"=="Auto" set "AUTO_SWITCH=-AutoSequence"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\StartGASM5TwoPlayers.ps1" ^
    -PktLagMs %PKT_LAG% -PktLossPercent %PKT_LOSS% -Stage %STAGE% %AUTO_SWITCH%
set "RESULT=%ERRORLEVEL%"

if not "%RESULT%"=="0" (
    echo [ERROR] Two-player launch failed with exit code %RESULT%.
)

if /I not "%~4"=="NoPause" pause
exit /b %RESULT%
