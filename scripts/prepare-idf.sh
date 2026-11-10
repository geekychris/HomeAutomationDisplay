#!/usr/bin/env bash
# PlatformIO's ESP-IDF 5.4 comes with idf-component-manager 1.x, which cannot
# read the manifests of current LVGL releases ("$CONFIG{...}" rules), and the
# build stops with a pyparsing traceback. Put 2.x in IDF's Python environment.
# Safe to run any number of times.
set -euo pipefail
cd "$(dirname "$0")/.."
PIO=${PIO:-$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")}
penv="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/penv/.espidf-5.4.0"
if [ ! -x "$penv/bin/python" ]; then
    # The first build creates IDF's Python environment (and then fails here).
    "$PIO" run -e cyd >/dev/null 2>&1 || true
fi
if ! "$penv/bin/python" -c 'import importlib.metadata as m, sys; sys.exit(int(m.version("idf-component-manager").split(".")[0]) < 2)' 2>/dev/null; then
    echo "Upgrading idf-component-manager in $penv"
    "$penv/bin/python" -m pip install -q "idf-component-manager>=2.5,<3"
    rm -rf managed_components            # whatever 1.x half-downloaded
fi
