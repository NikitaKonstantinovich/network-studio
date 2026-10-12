@echo off
set "STUDIO_EXE=%~dp0build\Release\network_studio.exe"
if not exist "%STUDIO_EXE%" (
    echo Build the application first: powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Test
    pause
    exit /b 1
)
start "" "%STUDIO_EXE%" %*
