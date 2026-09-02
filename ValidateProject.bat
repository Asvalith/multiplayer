@echo off
setlocal

set "NETWORK_TEST=%~dp0Scripts\RunMultiplayerNetworkTests.ps1"
set "DEFAULT_EDITOR=C:\Program Files\Epic Games\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe"
if defined UE_EDITOR set "DEFAULT_EDITOR=%UE_EDITOR%"

if not exist "%NETWORK_TEST%" (
    echo [ERROR] Validation script not found: %NETWORK_TEST%
    exit /b 1
)
if not exist "%DEFAULT_EDITOR%" (
    echo [ERROR] UnrealEditor.exe not found: %DEFAULT_EDITOR%
    echo Set UE_EDITOR to the full path of UnrealEditor.exe and run again.
    exit /b 1
)

echo [1/2] Running direct-connect weak-network and reconnect regression...
powershell -NoProfile -ExecutionPolicy Bypass -File "%NETWORK_TEST%" ^
    -EditorPath "%DEFAULT_EDITOR%" -Profile All -TestReconnect ^
    -PublishEvidence -EvidenceName "weak-network-reconnect"
if errorlevel 1 exit /b %ERRORLEVEL%

echo [2/2] Running Session, gameplay, UI, restart/leave and bandwidth A/B validation...
powershell -NoProfile -ExecutionPolicy Bypass -File "%NETWORK_TEST%" ^
    -EditorPath "%DEFAULT_EDITOR%" -Profile Normal -Port 28777 ^
    -TestSession -TestGameplayFlow -TestRestartAndLeave -CompareBandwidth ^
    -PublishEvidence -EvidenceName "full-project-validation"
if errorlevel 1 exit /b %ERRORLEVEL%

echo Project validation passed. Public evidence is in Tests\Evidence.
exit /b 0
