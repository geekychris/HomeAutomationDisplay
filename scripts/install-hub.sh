#!/usr/bin/env bash
# Install the hub on a Linux machine (or a Mac) without Home Assistant's
# add-on system - for HA Container / Core installs, or a separate box.
#
#   scripts/install-hub.sh --ha-url http://homeassistant.local:8123 --token-file ~/ha-token
#
# Installs into ~/.local/share/had-hub, keeps layouts in ~/.local/share/had-hub/data,
# and on Linux with systemd adds a user service (had-hub) that starts at boot.
# Without --ha-url it runs the mock house.
set -euo pipefail
src="$(cd "$(dirname "$0")/../hub" && pwd)"
dest="${HAD_HUB_HOME:-$HOME/.local/share/had-hub}"
ha_url="" token_file="" port=8390

while [ $# -gt 0 ]; do
    case $1 in
        --ha-url) ha_url=$2; shift ;;
        --token-file) token_file=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); shift ;;
        --port) port=$2; shift ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
if [ -n "$ha_url" ] && [ -z "$token_file" ]; then
    echo "A long-lived access token is needed with --ha-url." >&2
    echo "Make one in Home Assistant (your profile > Security), save it to a file, and pass --token-file." >&2
    exit 2
fi

echo "Installing the hub into $dest"
mkdir -p "$dest/data"
python3 -m venv "$dest/venv"
"$dest/venv/bin/pip" install -q --upgrade pip
"$dest/venv/bin/pip" install -q -r "$src/requirements.txt"
rm -rf "$dest/had_hub"
cp -R "$src/had_hub" "$dest/had_hub"

args=(--port "$port" --config "$dest/data/displays.json")
[ -n "$ha_url" ] && args+=(--ha-url "$ha_url" --ha-token-file "$token_file")
cat > "$dest/run.sh" <<RUN
#!/bin/sh
cd "$dest" && exec "$dest/venv/bin/python" -m had_hub $(printf '%q ' "${args[@]}")"\$@"
RUN
chmod +x "$dest/run.sh"

if command -v systemctl >/dev/null && [ "$(uname)" = Linux ]; then
    mkdir -p "$HOME/.config/systemd/user"
    cat > "$HOME/.config/systemd/user/had-hub.service" <<UNIT
[Unit]
Description=Home Automation Display hub
After=network-online.target

[Service]
ExecStart=$dest/run.sh
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
UNIT
    systemctl --user daemon-reload
    systemctl --user enable --now had-hub
    # Keep it running when you are logged out.
    loginctl enable-linger "$USER" 2>/dev/null || true
    echo "Running as the systemd user service had-hub (journalctl --user -u had-hub -f)."
else
    echo "Start it with: $dest/run.sh"
fi
echo "Admin page: http://$(hostname):$port/"
