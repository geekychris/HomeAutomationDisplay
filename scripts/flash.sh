#!/usr/bin/env bash
# Flash the display over USB. With no port, PlatformIO picks the board.
#   scripts/flash.sh                         build and flash
#   scripts/flash.sh /dev/cu.usbserial-110   on that port
#   scripts/flash.sh --monitor               then show the logs
# BOARD picks the build (default cyd): cyd, st7796r, sunton35c.
#   BOARD=st7796r scripts/flash.sh
set -euo pipefail
cd "$(dirname "$0")/.."
BOARD=${BOARD:-cyd}
PIO=${PIO:-$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")}
port=() monitor=0
for a in "$@"; do
    case $a in
        --monitor) monitor=1 ;;
        *) port=(--upload-port "$a") ;;
    esac
done
scripts/prepare-idf.sh
"$PIO" run -e "$BOARD" -t upload ${port[@]+"${port[@]}"}
if [ $monitor = 1 ]; then
    "$PIO" device monitor -e "$BOARD" ${port[1]:+--port "${port[1]}"}
fi
