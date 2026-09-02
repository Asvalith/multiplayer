@echo off
setlocal

set "NETWORK_TEST=%~dp0Scripts\RunMultiplayerNetworkTests.ps1"
set "DEFAULT_EDITOR=C:\Program Files\Epic Games\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe"
if defined UE_EDITOR set "DEFAULT_EDITOR=%UE_EDITOR%"

if not exist "%NETWORK_TEST%" (
    echo [ERROR] Network test script not found:
    echo %NETWORK_TEST%
    exit /b 1
)

if not exist "%DEFAULT_EDITOR%" (
    echo [ERROR] UnrealEditor.exe not found:
    echo %DEFAULT_EDITOR%
    echo Set UE_EDITOR to the full path of UnrealEditor.exe and run again.
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%NETWORK_TEST%" ^
    -EditorPath "%DEFAULT_EDITOR%" -TestReconnect %*
set "TEST_EXIT_CODE=%ERRORLEVEL%"

if not "%TEST_EXIT_CODE%"=="0" (
    echo.
    echo Network test failed. See Saved\TestReports for details.
)

endlocal & exit /b %TEST_EXIT_CODE%
