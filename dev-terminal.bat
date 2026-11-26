@echo off
cd /d "%~dp0"
where wt >nul 2>&1
if %errorlevel%==0 (
    start "" wt -d "%~dp0"
) else (
    start "" powershell -NoExit -Command "Set-Location '%~dp0'"
)
