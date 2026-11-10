"""The real house: a WebSocket to Home Assistant.

One connection carries everything: get_states once, then state_changed
events as they happen, and call_service for taps. Only entities a display
could show (entities.KIND_BY_DOMAIN) are kept, and of their attributes only
the ones the hub reads (entities.USED_ATTRIBUTES), plus two things from HA's
registries: the area, so the admin page can group by room, and the sensor
display precision set in HA's UI.

Run as a Home Assistant add-on it connects through the Supervisor
(ws://supervisor/core/websocket) with the token the Supervisor hands every
add-on. Anywhere else it needs HA's address and a long-lived access token
(HA: your profile -> Security -> Long-lived access tokens).

If HA goes away (a restart, an update) it reconnects with a back-off and
loads every state afresh; displays keep showing the last known state
meanwhile, and taps fail with a message.
"""
from __future__ import annotations

import asyncio
import itertools
import logging
from urllib.parse import urlsplit, urlunsplit

import aiohttp

from .. import entities as E
from .base import Backend

log = logging.getLogger(__name__)

SUPERVISOR_WS = "ws://supervisor/core/websocket"
REGISTRY_REFRESH = 15 * 60      # seconds: areas and precision change rarely
CALL_TIMEOUT = 15


class AuthError(Exception):
    """HA refused the token. Retrying will not help until it is fixed."""


def websocket_url(url: str) -> str:
    """http://host:8123 -> ws://host:8123/api/websocket; ws:// URLs as given."""
    parts = urlsplit(url.strip().rstrip("/"))
    scheme = {"http": "ws", "https": "wss"}.get(parts.scheme, parts.scheme)
    path = parts.path if parts.path.endswith("/websocket") else parts.path + "/api/websocket"
    return urlunsplit((scheme, parts.netloc, path, "", ""))


def _keep(entity_id: str) -> bool:
    return E.tile_kind(entity_id) is not None


class HomeAssistantBackend(Backend):
    name = "homeassistant"

    def __init__(self, url: str, token: str, verify_ssl: bool = True) -> None:
        super().__init__()
        self.url = websocket_url(url)
        self._token = token
        self._verify_ssl = verify_ssl
        self._session: aiohttp.ClientSession | None = None
        self._ws: aiohttp.ClientWebSocketResponse | None = None
        self._task: asyncio.Task | None = None
        self._ids = itertools.count(1)
        self._pending: dict[int, asyncio.Future] = {}
        self._connected = False
        self._detail = "connecting"
        self._ha_version = ""
        # Registry extras, merged into attributes: entity_id -> {"area": .., "suggested_display_precision": ..}
        self._extra: dict[str, dict] = {}
        self._raw: dict[str, tuple[str, dict]] = {}     # last (state, trimmed attrs) from HA

    @classmethod
    def from_supervisor(cls, token: str) -> "HomeAssistantBackend":
        return cls(SUPERVISOR_WS, token)

    # ------------------------------------------------------------ lifecycle

    async def start(self) -> None:
        self._session = aiohttp.ClientSession()
        self._task = asyncio.create_task(self._run())

    async def stop(self) -> None:
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except (asyncio.CancelledError, Exception):
                pass
        if self._ws:
            await self._ws.close()
        if self._session:
            await self._session.close()

    def status(self) -> dict:
        detail = self._detail
        if self._connected:
            detail = f"Home Assistant {self._ha_version}".strip()
        return {"connected": self._connected, "detail": detail}

    async def _run(self) -> None:
        delay = 1
        while True:
            try:
                await self._session_once()
                delay = 1
            except AuthError as err:
                self._detail = f"token refused: {err}"
                log.error("Home Assistant refused the token (%s); retrying in 60 s", err)
                delay = 60
            except asyncio.CancelledError:
                raise
            except Exception as err:          # network, HA restarting, protocol surprises
                self._detail = f"cannot reach Home Assistant: {err or type(err).__name__}"
                log.warning("%s; retrying in %d s", self._detail, delay)
            finally:
                self._connected = False
                self._ws = None
                for fut in self._pending.values():
                    if not fut.done():
                        fut.set_exception(ConnectionError("lost the connection to Home Assistant"))
                self._pending.clear()
            await asyncio.sleep(delay)
            delay = min(delay * 2, 30)

    async def _session_once(self) -> None:
        log.info("connecting to %s", self.url)
        async with self._session.ws_connect(self.url, heartbeat=30, max_msg_size=0,
                                            ssl=True if self._verify_ssl else False) as ws:
            msg = await ws.receive_json(timeout=10)
            if msg.get("type") != "auth_required":
                raise ConnectionError(f"unexpected greeting {msg.get('type')!r}")
            self._ha_version = msg.get("ha_version", "")
            await ws.send_json({"type": "auth", "access_token": self._token})
            msg = await ws.receive_json(timeout=10)
            if msg.get("type") != "auth_ok":
                raise AuthError(msg.get("message", msg.get("type")))
            self._ha_version = msg.get("ha_version", self._ha_version)
            self._ws = ws
            reader = asyncio.create_task(self._read(ws))
            try:
                await self._command({"type": "subscribe_events", "event_type": "state_changed"})
                await self._load_registries()
                await self._load_states()
                self._connected = True
                self._detail = ""
                log.info("connected to Home Assistant %s: %d entities a display can show",
                         self._ha_version, len(self._states))
                refresher = asyncio.create_task(self._refresh_registries())
                try:
                    await reader
                finally:
                    refresher.cancel()
            finally:
                reader.cancel()
        raise ConnectionError("Home Assistant closed the connection")

    # ------------------------------------------------------------ requests

    async def _command(self, msg: dict, timeout: float = CALL_TIMEOUT):
        ws = self._ws
        if ws is None or ws.closed:
            raise ConnectionError("not connected to Home Assistant")
        msg_id = next(self._ids)
        fut = asyncio.get_running_loop().create_future()
        self._pending[msg_id] = fut
        try:
            await ws.send_json({"id": msg_id, **msg})
            return await asyncio.wait_for(fut, timeout)
        finally:
            self._pending.pop(msg_id, None)

    async def _read(self, ws: aiohttp.ClientWebSocketResponse) -> None:
        async for raw in ws:
            if raw.type != aiohttp.WSMsgType.TEXT:
                continue
            data = raw.json()
            # HA may batch several messages into one JSON array.
            for msg in data if isinstance(data, list) else [data]:
                self._dispatch(msg)

    def _dispatch(self, msg: dict) -> None:
        t = msg.get("type")
        if t == "event":
            ev = msg.get("event", {})
            if ev.get("event_type") == "state_changed":
                d = ev.get("data", {})
                self._apply(d.get("entity_id"), d.get("new_state"))
            return
        fut = self._pending.get(msg.get("id"))
        if not fut or fut.done():
            return
        if t == "result" and not msg.get("success", False):
            err = msg.get("error") or {}
            fut.set_exception(E.ActionError(err.get("message") or err.get("code") or "Home Assistant said no"))
        else:
            fut.set_result(msg.get("result"))

    # ------------------------------------------------------------ states

    def _apply(self, entity_id: str | None, new_state: dict | None) -> None:
        if not entity_id or not _keep(entity_id):
            return
        if new_state is None:                    # removed from HA
            self._raw.pop(entity_id, None)
            self._set(entity_id, "unavailable", {})
            return
        attrs = {k: v for k, v in (new_state.get("attributes") or {}).items() if k in E.USED_ATTRIBUTES}
        self._raw[entity_id] = (new_state.get("state", "unknown"), attrs)
        self._publish(entity_id)

    def _publish(self, entity_id: str) -> None:
        state, attrs = self._raw[entity_id]
        extra = self._extra.get(entity_id)
        self._set(entity_id, state, {**attrs, **extra} if extra else attrs)

    async def _load_states(self) -> None:
        states = await self._command({"type": "get_states"}, timeout=60) or []
        seen = set()
        for s in states:
            entity_id = s.get("entity_id")
            if entity_id and _keep(entity_id):
                seen.add(entity_id)
                self._apply(entity_id, s)
        # Anything that was there before a reconnect and is gone now.
        for entity_id in [e for e in self._states if e not in seen]:
            self._raw.pop(entity_id, None)
            self._set(entity_id, "unavailable", {})

    async def _load_registries(self) -> None:
        """Areas, and the display precision people set in HA's UI. Optional:
        without them the hub works the same, with less to sort by."""
        try:
            areas = {a["area_id"]: a["name"] for a in await self._command({"type": "config/area_registry/list"})}
            devices = {d["id"]: d.get("area_id") for d in await self._command({"type": "config/device_registry/list"})}
            entries = await self._command({"type": "config/entity_registry/list"}, timeout=60)
        except Exception as err:
            log.info("could not read HA's registries (%s); no areas", err)
            return
        extra: dict[str, dict] = {}
        for r in entries:
            entity_id = r.get("entity_id", "")
            if not _keep(entity_id):
                continue
            x = {}
            area = areas.get(r.get("area_id") or devices.get(r.get("device_id")) or "")
            if area:
                x["area"] = area
            sensor_opts = (r.get("options") or {}).get("sensor") or {}
            prec = sensor_opts.get("display_precision", sensor_opts.get("suggested_display_precision"))
            if prec is not None:
                x["suggested_display_precision"] = prec
            if x:
                extra[entity_id] = x
        self._extra = extra

    async def _refresh_registries(self) -> None:
        while True:
            await asyncio.sleep(REGISTRY_REFRESH)
            await self._load_registries()
            for entity_id in list(self._raw):
                self._publish(entity_id)

    # ------------------------------------------------------------ services

    async def call_service(self, domain: str, service: str, data: dict) -> None:
        if not self._connected:
            raise E.ActionError("Home Assistant is not connected")
        data = dict(data)
        target = {"entity_id": data.pop("entity_id")} if "entity_id" in data else None
        msg = {"type": "call_service", "domain": domain, "service": service, "service_data": data}
        if target:
            msg["target"] = target
        log.info("service %s.%s %s %s", domain, service, target or "", data or "")
        await self._command(msg)
