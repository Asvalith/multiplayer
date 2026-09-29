@echo off
setlocal
rem Default topology: one dedicated server and two remote clients.
if defined UE_EDITOR (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\RunLocalDedicatedServer.ps1" -Play -Port 7777 -EditorPath "%UE_EDITOR%"
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\RunLocalDedicatedServer.ps1" -Play -Port 7777
)
if errorlevel 1 pause
endlocal
