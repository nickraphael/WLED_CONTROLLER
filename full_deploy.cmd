@echo off
setlocal
cd /d "%~dp0"
echo ===== FULL DEPLOY: build, merge, flash =====
echo.
call build.cmd
if errorlevel 1 goto :failed
echo.
call merge.cmd
if errorlevel 1 goto :failed
echo.
call deploy.cmd
if errorlevel 1 goto :failed
echo.
echo ===== FULL DEPLOY COMPLETE =====
exit /b 0

:failed
echo.
echo ===== FULL DEPLOY STOPPED: a step failed (see above) =====
exit /b 1
