@echo off
setlocal

rem Two-player local network smoke test.
rem Player 1 starts a listen server; Player 2 connects automatically.

set "UE_EDITOR=E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe"
set "PROJECT=E:\ueprojrct\multiplayer\multiplayer.uproject"
set "TEST_MAP=/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo"
set "PORT=17777"

rem Change these values if the windows do not fit your monitor.
set "WINDOW_WIDTH=900"
set "WINDOW_HEIGHT=650"
set "WINDOW_Y=40"
set "HOST_X=0"
set "CLIENT_X=900"

if not exist "%UE_EDITOR%" (
    echo [ERROR] UnrealEditor.exe not found:
    echo %UE_EDITOR%
    pause
    exit /b 1
)

if not exist "%PROJECT%" (
    echo [ERROR] Project not found:
    echo %PROJECT%
    pause
    exit /b 1
)

echo Starting Player 1 - Listen Server...
start "Co-op Test - Player 1 Host" "%UE_EDITOR%" "%PROJECT%" "%TEST_MAP%?listen" ^
    -game -windowed -ResX=%WINDOW_WIDTH% -ResY=%WINDOW_HEIGHT% ^
    -WinX=%HOST_X% -WinY=%WINDOW_Y% -port=%PORT% -NoSplash ^
    -DDC=InstalledNoZenLocalFallback

echo Waiting for the listen server...
timeout /t 8 /nobreak >nul

echo Starting Player 2 - Client...
start "Co-op Test - Player 2 Client" "%UE_EDITOR%" "%PROJECT%" "127.0.0.1:%PORT%" ^
    -game -windowed -ResX=%WINDOW_WIDTH% -ResY=%WINDOW_HEIGHT% ^
    -WinX=%CLIENT_X% -WinY=%WINDOW_Y% -NoSplash ^
    -DDC=InstalledNoZenLocalFallback

echo.
echo Two-player test launched.
echo Host:   %WINDOW_WIDTH%x%WINDOW_HEIGHT% at X=%HOST_X% Y=%WINDOW_Y%
echo Client: %WINDOW_WIDTH%x%WINDOW_HEIGHT% at X=%CLIENT_X% Y=%WINDOW_Y%
echo Press any key to close this launcher. The game windows will remain open.
pause >nul

endlocal
