#!/usr/bin/env bash
#
# ============================================================
#        APPLE II Emulator for ESP32-TTGO-VGA
#   (C) 2026 Reinaldo Torres / CoCo Byte Club
#   https://github.com/reyco2000/ESP32-TTGO-VGA_AppleII_Emulator
#   MIT License
# ============================================================
#  File   : build-firmware.sh
#  Module : Builds release firmware with arduino-cli, merges the
#           bootloader / partition table / boot_app0 / app images
#           into one flashable file at offset 0x0, and writes
#           SHA256 checksums. Output lands in build/.
# ============================================================
#
# Usage:  tools/build-firmware.sh [output-dir]
#
set -euo pipefail

FQBN="esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=huge_app"
SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-$SKETCH_DIR/build}"
NAME="ESP32-VGA_AppleII_Emulator"

command -v arduino-cli >/dev/null || { echo "error: arduino-cli not found in PATH"; exit 1; }

echo "==> Cleaning $OUT_DIR"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

echo "==> Compiling ($FQBN)"
arduino-cli compile --fqbn "$FQBN" --output-dir "$OUT_DIR" "$SKETCH_DIR"

# esptool and boot_app0 ship with the installed esp32 core; resolve whatever
# version is present rather than hard-coding one.
ESPTOOL="$(ls -1 "$HOME"/.arduino15/packages/esp32/tools/esptool_py/*/esptool.py 2>/dev/null | sort -V | tail -1)"
BOOT_APP0="$(ls -1 "$HOME"/.arduino15/packages/esp32/hardware/esp32/*/tools/partitions/boot_app0.bin 2>/dev/null | sort -V | tail -1)"
[ -n "$ESPTOOL" ]   || { echo "error: esptool.py not found under ~/.arduino15"; exit 1; }
[ -n "$BOOT_APP0" ] || { echo "error: boot_app0.bin not found under ~/.arduino15"; exit 1; }

cp "$BOOT_APP0" "$OUT_DIR/boot_app0.bin"

echo "==> Merging into single-file image"
python3 "$ESPTOOL" --chip esp32 merge_bin \
  -o "$OUT_DIR/$NAME.merged.bin" \
  --flash_mode dio --flash_freq keep --flash_size 4MB \
  0x1000  "$OUT_DIR/$NAME.ino.bootloader.bin" \
  0x8000  "$OUT_DIR/$NAME.ino.partitions.bin" \
  0xe000  "$OUT_DIR/boot_app0.bin" \
  0x10000 "$OUT_DIR/$NAME.ino.bin"

# The .elf and .map are large build intermediates, not release artifacts.
rm -f "$OUT_DIR/$NAME.ino.elf" "$OUT_DIR/$NAME.ino.map"

echo "==> Checksums"
( cd "$OUT_DIR" && sha256sum ./*.bin > SHA256SUMS && cat SHA256SUMS )

echo
echo "Firmware ready in $OUT_DIR"
ls -la "$OUT_DIR"
