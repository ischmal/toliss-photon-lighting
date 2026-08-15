@echo off
REM Build + deploy EVERYTHING -- OBJs, overlays and the native .xpl -- into
REM X-Plane 12 (double-click me). Close X-Plane first.
REM Passes any args through to deploy.ps1 (e.g. -Dev, -Test, -NoObjs,
REM -XPlaneRoot "D:\...").
REM
REM PREFERS pwsh (PowerShell 7) and it matters for one step: Windows PowerShell
REM 5.1 cannot create a symlink without elevation even with Developer Mode on --
REM it never passes SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE -- so panelfx.txt
REM silently falls back to a COPY and in-sim FX edits stop reaching the repo.
where pwsh >nul 2>&1
if %ERRORLEVEL%==0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1" %*
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1" %*
)
echo.
pause
