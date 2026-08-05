@echo off
setlocal

set "UE_EDITOR=E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe"
set "PROJECT=E:\ueprojrct\multiplayer\multiplayer.uproject"
set "START_MAP=/Game/UI/mainmenu"

if not exist "%UE_EDITOR%" (
    echo UnrealEditor.exe not found: %UE_EDITOR%
    pause
    exit /b 1
)

if not exist "%PROJECT%" (
    echo Project not found: %PROJECT%
    pause
    exit /b 1
)

echo Starting player 1...
start "Co-op Player 1" "%UE_EDITOR%" "%PROJECT%" "%START_MAP%" -game -windowed -ResX=960 -ResY=540 -WinX=0 -WinY=40 -log -NoSplash -DDC=InstalledNoZenLocalFallback

timeout /t 10 /nobreak >nul

echo Starting player 2...
start "Co-op Player 2" "%UE_EDITOR%" "%PROJECT%" "%START_MAP%" -game -windowed -ResX=960 -ResY=540 -WinX=960 -WinY=40 -log -NoSplash -DDC=InstalledNoZenLocalFallback

endlocal
