# Display hub

The hub sits between Home Assistant and the displays. It keeps track of
which entities go on which display, sends each display its layout and live
state, and turns taps into Home Assistant service calls. This directory is
also the Home Assistant add-on (`config.yaml`, `Dockerfile`).

For installing it, see [../docs/install.md](../docs/install.md). For how
it works, see [../docs/architecture.md](../docs/architecture.md).

## Run it

```sh
cd hub
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt

.venv/bin/python -m had_hub                     # the mock house
HA_URL=http://homeassistant.local:8123 HA_TOKEN=... .venv/bin/python -m had_hub   # your Home Assistant
```

Then open:

- **http://localhost:8390/**, the admin page. Displays appear on the left
  as they connect. Pick one, add pages and tiles, then **Save to display**.
  **Identify** makes that display show its id. The right-hand column is
  the house: use the controls there to see the displays follow. In the mock
  house you can also type in a sensor reading to try the CO2 and VOC
  colours.
- **http://localhost:8390/virtual**, a display in a browser tab, drawn like
  the real 320×240 screen. Open several tabs with different `?id=...` to
  try several displays.

Which house it uses: as an add-on (`SUPERVISOR_TOKEN` set), Home Assistant
through the Supervisor; with `HA_URL` and `HA_TOKEN`, that Home Assistant;
otherwise the mock. `--backend` forces one.

| Option | Env | |
|---|---|---|
| `--port` | `HUB_PORT` | default 8390 |
| `--config` | | where layouts are kept (default `hub-data/displays.json`; `/data/displays.json` as an add-on) |
| `--ha-url` | `HA_URL` | e.g. `http://homeassistant.local:8123` |
| `--ha-token`, `--ha-token-file` | `HA_TOKEN`, `HA_TOKEN_FILE` | a long-lived access token |
| `--no-verify-ssl` | | for HA with a self-signed certificate |
| `--admin-from` | `HUB_ADMIN_FROM` | addresses/CIDRs allowed to use the admin page; default anyone |
| `--advertise-ip` | `HUB_ADVERTISE_IP` | the address displays should use |
| `--no-mdns` | | do not advertise `_hadisplay._tcp` |
| `--tick` | | mock: seconds between simulation steps |

`tools/fake_display.py` is a display in a terminal that prints what it is
sent. It helps when debugging the protocol.

## Tests

```sh
.venv/bin/python -m pytest tests
```

`test_hub.py` covers the protocol, registry and mock. `test_kinds.py`
covers every entity domain. `test_homeassistant.py` runs the Home Assistant
backend against a fake of HA's WebSocket API (auth, states, events,
registries, service calls, reconnects), and checks the admin guard.

## Layout of the code

| File | What |
|---|---|
| `had_hub/entities.py` | HA entity → tile and state message; display action → HA service call. All domain knowledge |
| `had_hub/backends/base.py` | What a backend provides: states, change events, service calls, status |
| `had_hub/backends/homeassistant.py` | Home Assistant over its WebSocket API |
| `had_hub/backends/mock.py` | The mock house |
| `had_hub/registry.py` | Displays and their pages of tiles, saved as JSON |
| `had_hub/server.py` | The display WebSocket, the admin API, the admin guard, mDNS |
| `had_hub/__main__.py` | Command line, and add-on options |
| `had_hub/static/` | The admin page and the virtual display |
| `config.yaml`, `build.yaml`, `Dockerfile`, `DOCS.md`, `translations/` | The Home Assistant add-on |
| `docker-compose.yml`, `.env.example` | Running in Docker without the add-on system |

The display protocol is in [../docs/protocol.md](../docs/protocol.md).

## License

© 2026 Chris Collins. MIT License: see
[LICENSE](https://github.com/geekychris/HomeAutomationDisplay/blob/master/LICENSE).
