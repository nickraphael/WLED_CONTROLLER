@echo off
setlocal
cd /d "%~dp0"
rem ---- edit these if your setup differs ----
set PYTHON=C:\Temp\venvtest\Scripts\python.exe
set PORT=COM3
rem ------------------------------------------
echo === DEPLOY ===
if not exist merged.bin (
  echo No merged.bin. Run build.cmd then merge.cmd first.
  exit /b 1
)
rem Refuse to flash a stale image: merged.bin must be at least as new as firmware.bin
powershell -NoProfile -Command "if ((Get-Item 'merged.bin').LastWriteTime -lt (Get-Item '.pio\build\esp32s3\firmware.bin').LastWriteTime) { exit 1 }"
if errorlevel 1 (
  echo merged.bin is OLDER than firmware.bin. Run merge.cmd first.
  exit /b 1
)
echo Close any serial monitor first. Flashing %PORT% ...
"%PYTHON%" -m esptool --chip esp32s3 --port %PORT% write-flash 0x0 merged.bin
if errorlevel 1 (
  echo.
  echo FLASH FAILED. If it could not connect: hold BOOT, tap RESET, release BOOT, run again.
  exit /b 1
)
echo.
echo DONE. Press RESET on the board.
