@echo off
rem One-click OpenVST update for REAPER on Windows.
rem If REAPER is open it must be closed: the VST3 file cannot be replaced while
rem the host holds it. This window waits for REAPER to exit, installs, then
rem relaunches REAPER.
setlocal
set "ROOT=%~dp0"
echo OpenVST update
echo If REAPER is open, close it. The update applies when REAPER exits.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%openvst_update.ps1" -Config "%ROOT%update.json" -WaitForReaper 86400
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
    echo OpenVST update stopped with code %RC%.
    pause
    exit /b %RC%
)
echo OpenVST update finished.
ping -n 4 127.0.0.1 >nul
exit /b 0
