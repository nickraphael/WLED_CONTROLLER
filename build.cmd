@echo off
setlocal
cd /d "%~dp0"
echo === BUILD ===
docker run --rm -v "%cd%:/project" -v pio_cache:/root/.platformio -v pio_work:/pio_work -e PLATFORMIO_WORKSPACE_DIR=/pio_work -e PIP_CACHE_DIR=/root/.platformio/pipcache -w /project python:3.12-slim bash -c "pip install -q platformio && pio run && mkdir -p .pio/build/esp32s3 && cp /pio_work/build/esp32s3/firmware.bin /pio_work/build/esp32s3/bootloader.bin /pio_work/build/esp32s3/partitions.bin .pio/build/esp32s3/"
if errorlevel 1 (
  echo.
  echo BUILD FAILED. Do not merge or flash.
  exit /b 1
)
echo.
echo BUILD OK. Next: merge.cmd
