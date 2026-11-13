#!/usr/bin/env bash
# Build the display firmware, and for each board a single image you can flash
# at 0x0 with esptool or a web flasher:
#   dist/home-automation-display-<version>-<board>.bin
#   scripts/build-firmware.sh              every board
#   scripts/build-firmware.sh st7796r      one (cyd, st7796r, sunton35c)
set -euo pipefail
cd "$(dirname "$0")/.."

BOARDS=("$@")
[ ${#BOARDS[@]} -gt 0 ] || BOARDS=(cyd st7796r sunton35c)
PIO=${PIO:-$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")}
scripts/prepare-idf.sh

version=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' CMakeLists.txt)
mkdir -p dist
# esptool from PlatformIO if there is one, else whatever is on the PATH.
ESPTOOL=(python3 -m esptool)
pio_esptool="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
pio_python="$HOME/.platformio/penv/bin/python"
if [ -f "$pio_esptool" ] && [ -x "$pio_python" ]; then ESPTOOL=("$pio_python" "$pio_esptool"); fi

for board in "${BOARDS[@]}"; do
    "$PIO" run -e "$board"
    b=.pio/build/$board
    out=dist/home-automation-display-${version}-${board}.bin
    "${ESPTOOL[@]}" --chip esp32 merge_bin -o "$out" --flash_mode dio --flash_size 4MB \
        0x1000 "$b/bootloader.bin" 0x8000 "$b/partitions.bin" 0x10000 "$b/firmware.bin"
    echo "built $out"
done
