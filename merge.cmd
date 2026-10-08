@echo off
setlocal
cd /d "%~dp0"
echo === MERGE ===
if not exist ".pio\build\esp32s3\firmware.bin" (
  echo No firmware.bin. Run build.cmd first.
  exit /b 1
)
if exist merged.bin del merged.bin
docker run --rm -v "%cd%:/project" -v pio_cache:/root/.platformio -w /project python:3.12-slim bash -c "pip install -q esptool && python -m esptool --chip esp32s3 merge-bin -o merged.bin --flash-mode dio --flash-size 16MB 0x0 .pio/build/esp32s3/bootloader.bin 0x8000 .pio/build/esp32s3/partitions.bin 0xe000 /root/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin 0x10000 .pio/build/esp32s3/firmware.bin"
if errorlevel 1 (
  echo.
  echo MERGE FAILED. Do not flash.
  exit /b 1
)
if not exist merged.bin (
  echo.
  echo MERGE FAILED: merged.bin was not created.
  exit /b 1
)
echo.
echo MERGE OK. Next: deploy.cmd
