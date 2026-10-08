@echo off
setlocal
cd /d "%~dp0"
echo === BUILD ===
docker run --rm -v "%cd%:/project" -v pio_cache:/root/.platformio -w /project python:3.12-slim bash -c "pip install -q platformio && pio run"
if errorlevel 1 (
  echo.
  echo BUILD FAILED. Do not merge or flash.
  exit /b 1
)
echo.
echo BUILD OK. Next: merge.cmd
