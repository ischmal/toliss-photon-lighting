@echo off
REM Build + deploy the native ToLiss Photon .xpl into X-Plane 12 (double-click me).
REM Passes any args through to deploy.ps1 (e.g. -NoBuild, -XPlaneRoot "D:\...").
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1" %*
echo.
pause
