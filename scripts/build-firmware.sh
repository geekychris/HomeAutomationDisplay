#!/usr/bin/env bash
# Build the display firmware, and a single image you can flash at 0x0
# (dist/home-automation-display-<version>.bin) with esptool or a web flasher.
set -euo pipefail
cd "$(dirname "$0")/.."

PIO=${PIO:-$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")}
scripts/prepare-idf.sh
"$PIO" run -e cyd

version=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' CMakeLists.txt)
out=dist/home-automation-display-${version}.bin
mkdir -p dist
b=.pio/build/cyd
# esptool from PlatformIO if there is one, else whatever is on the PATH.
ESPTOOL=(python3 -m esptool)
pio_esptool="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
pio_python="$HOME/.platformio/penv/bin/python"
if [ -f "$pio_esptool" ] && [ -x "$pio_python" ]; then ESPTOOL=("$pio_python" "$pio_esptool"); fi
"${ESPTOOL[@]}" --chip esp32 merge_bin -o "$out" --flash_mode dio --flash_size 4MB \
    0x1000 "$b/bootloader.bin" 0x8000 "$b/partitions.bin" 0x10000 "$b/firmware.bin"
echo "built $out"
