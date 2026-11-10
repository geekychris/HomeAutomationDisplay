#!/usr/bin/env bash
# Everything CI checks: the hub's tests, and the firmware builds.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -x hub/.venv/bin/python ]; then
    python3 -m venv hub/.venv
    hub/.venv/bin/pip install -q -r hub/requirements-dev.txt
fi
(cd hub && .venv/bin/python -m pytest -q tests)
scripts/build-firmware.sh
