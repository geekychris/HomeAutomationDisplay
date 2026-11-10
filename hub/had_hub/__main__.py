"""Run the hub:  python -m had_hub  (then open http://localhost:8390/).

Which house it talks to:

- As a Home Assistant add-on (SUPERVISOR_TOKEN is set): Home Assistant,
  through the Supervisor. Settings come from the add-on's options
  (/data/options.json) and the layouts are kept in /data.
- With HA_URL and HA_TOKEN (or --ha-url / --ha-token / --ha-token-file):
  that Home Assistant.
- Otherwise: the mock house, for trying things out.

--backend forces one or the other.
"""
from __future__ import annotations

import argparse
import asyncio
import ipaddress
import json
import logging
import os
import sys
from pathlib import Path

from aiohttp import web

from . import __version__
from .backends import HomeAssistantBackend, MockBackend
from .registry import Registry
from .server import Hub, advertise, lan_ip, make_app, unadvertise

log = logging.getLogger("had_hub")

ADDON_OPTIONS = Path("/data/options.json")
# Requests through HA's ingress come from the Supervisor at this address.
INGRESS_PROXY = "172.30.32.2"


def addon_options() -> dict:
    try:
        return json.loads(ADDON_OPTIONS.read_text())
    except (OSError, ValueError):
        return {}


def allow_from(networks: list[str]):
    """An admin_allowed check for the given addresses / CIDR ranges."""
    nets = [ipaddress.ip_network(n.strip(), strict=False) for n in networks if n.strip()]

    def allowed(remote: str | None) -> bool:
        try:
            ip = ipaddress.ip_address(remote or "")
        except ValueError:
            return False
        return any(ip in n for n in nets)
    return allowed


def main(argv: list[str] | None = None) -> None:
    p = argparse.ArgumentParser(prog="had_hub", description="Hub for Cheap Yellow Display panels")
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=int(os.environ.get("HUB_PORT", 8390)))
    p.add_argument("--config", default=None,
                   help="where display layouts are kept (default: hub-data/displays.json, "
                        "or /data/displays.json as an add-on)")
    p.add_argument("--backend", choices=["auto", "mock", "homeassistant"], default="auto")
    p.add_argument("--ha-url", default=os.environ.get("HA_URL"),
                   help="Home Assistant, e.g. http://homeassistant.local:8123 (env HA_URL)")
    p.add_argument("--ha-token", default=os.environ.get("HA_TOKEN"),
                   help="a long-lived access token (env HA_TOKEN)")
    p.add_argument("--ha-token-file", default=os.environ.get("HA_TOKEN_FILE"),
                   help="read the token from this file instead")
    p.add_argument("--no-verify-ssl", action="store_true", help="accept HA's self-signed certificate")
    p.add_argument("--admin-from", default=os.environ.get("HUB_ADMIN_FROM"),
                   help="comma-separated addresses/CIDRs allowed to use the admin page (default: anyone)")
    p.add_argument("--tick", type=float, default=3.0, help="mock: seconds between simulation steps")
    p.add_argument("--no-mdns", action="store_true", help="do not advertise the hub on the LAN")
    p.add_argument("--advertise-ip", default=os.environ.get("HUB_ADVERTISE_IP"),
                   help="address to advertise (default: this machine's LAN address)")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("--version", action="version", version=f"had_hub {__version__}")
    args = p.parse_args(argv)

    supervisor_token = os.environ.get("SUPERVISOR_TOKEN")
    opts = addon_options() if supervisor_token else {}
    level = logging.DEBUG if args.verbose else getattr(logging, str(opts.get("log_level", "info")).upper(), logging.INFO)
    logging.basicConfig(level=level, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    if level > logging.DEBUG:
        logging.getLogger("aiohttp.access").setLevel(logging.WARNING)

    token = args.ha_token
    if not token and args.ha_token_file:
        token = Path(args.ha_token_file).read_text().strip()
    kind = args.backend
    if kind == "auto":
        kind = "homeassistant" if (supervisor_token or (args.ha_url and token)) else "mock"

    admin_allowed = allow_from(args.admin_from.split(",")) if args.admin_from else None
    if kind == "mock":
        backend = MockBackend(tick=args.tick)
        config = args.config or "hub-data/displays.json"
    elif supervisor_token and not args.ha_url:
        backend = HomeAssistantBackend.from_supervisor(supervisor_token)
        config = args.config or "/data/displays.json"
        if not opts.get("lan_admin", False):
            admin_allowed = allow_from([INGRESS_PROXY] + (args.admin_from or "").split(","))
        if opts.get("advertise_ip"):
            args.advertise_ip = opts["advertise_ip"]
        if opts.get("mdns") is False:
            args.no_mdns = True
    else:
        if not args.ha_url or not token:
            sys.exit("had_hub: the homeassistant backend needs --ha-url and a token "
                     "(--ha-token, --ha-token-file, or HA_URL / HA_TOKEN)")
        backend = HomeAssistantBackend(args.ha_url, token, verify_ssl=not args.no_verify_ssl)
        config = args.config or "hub-data/displays.json"

    hub = Hub(backend, Registry(config), admin_allowed=admin_allowed)
    app = make_app(hub)

    async def run() -> None:
        runner = web.AppRunner(app)
        await runner.setup()
        await web.TCPSite(runner, args.host, args.port).start()
        handle = None if args.no_mdns else await advertise(args.port, args.advertise_ip)
        log.info("hub %s up (%s backend, layouts in %s): admin page http://localhost:%d/, "
                 "displays connect to ws://%s:%d/ws", __version__, backend.name, config, args.port,
                 args.advertise_ip or lan_ip(), args.port)
        try:
            await asyncio.Event().wait()
        finally:
            await unadvertise(handle)
            await runner.cleanup()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
