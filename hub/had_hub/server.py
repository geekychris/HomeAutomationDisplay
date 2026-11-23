"""The hub: displays connect here, and so does the admin page.

Displays hold a WebSocket to /ws and speak the protocol in
docs/protocol.md: they say hello, get a layout and the live state of every
tile, and send back actions when someone taps. The admin page uses /api/*
to assign entities to displays, and /api/live for live updates.

When the admin side is restricted (as a Home Assistant add-on, where it is
reached through HA's own login), anyone else gets only what a display
needs: /ws, and the virtual display page.
"""
from __future__ import annotations

import asyncio
import json
import logging
import socket
import time
from pathlib import Path
from typing import Callable

from aiohttp import WSMsgType, web

from . import __version__
from . import entities as E
from . import scenes as S
from .backends.base import Backend
from .registry import ConfigError, Registry

log = logging.getLogger(__name__)

PROTO = 1
STATIC = Path(__file__).parent / "static"
# Open to everyone even when the admin side is restricted.
PUBLIC_PATHS = {"/ws", "/virtual"}


class DisplaySession:
    """One connected display. Messages go out through a queue so they leave
    in order, whichever task queued them."""

    def __init__(self, display_id: str, ws: web.WebSocketResponse, hello: dict) -> None:
        self.id = display_id
        self.ws = ws
        self.hello = hello
        self.since = time.time()
        self.active = None              # the scene last reported as on, or None
        self._queue: asyncio.Queue = asyncio.Queue(maxsize=256)
        self._writer = asyncio.create_task(self._write())

    def send(self, msg: dict) -> None:
        try:
            self._queue.put_nowait(msg)
        except asyncio.QueueFull:
            log.warning("%s is not keeping up; dropping a message", self.id)

    async def _write(self) -> None:
        while True:
            msg = await self._queue.get()
            try:
                await self.ws.send_str(json.dumps(msg, separators=(",", ":"), ensure_ascii=False))
            except (ConnectionResetError, RuntimeError):
                return

    def close(self) -> None:
        self._writer.cancel()


class Hub:
    def __init__(self, backend: Backend, registry: Registry,
                 admin_allowed: Callable[[str | None], bool] | None = None) -> None:
        self.backend = backend
        self.registry = registry
        # None: anyone may use the admin page. Otherwise called with the
        # client's address.
        self.admin_allowed = admin_allowed
        self.sessions: dict[str, DisplaySession] = {}
        self.admins: set[web.WebSocketResponse] = set()
        self._rev = 0
        self._scenes_on: set[str] = set()   # which scenes are on, as the admin page last heard
        backend.subscribe(self._on_state)

    # ------------------------------------------------------------ messages to displays

    def layout_msg(self, display_id: str) -> dict:
        d = self.registry.get(display_id) or {}
        self._rev += 1
        pages = []
        for p in d.get("pages", []):
            tiles = [t for t in (E.tile_def(c["entity"], self.backend.get(c["entity"]), c)
                                 for c in p["tiles"]) if t]
            pages.append({"n": E.clip(p["name"], E.MAX_PAGE_NAME), "tiles": tiles})
        title = d.get("name") or d.get("info", {}).get("name") or display_id
        msg = {"t": "layout", "rev": self._rev, "title": E.clip(title, E.MAX_NAME), "pages": pages}
        if not any(p["tiles"] for p in pages):
            msg["pages"] = []
            msg["msg"] = f"Not set up yet.\nOpen the hub and add tiles to\n{display_id}"
        strip = self.registry.display_scenes(display_id)
        if strip:
            msg["scenes"] = [{"id": sid, "n": E.clip(sc["name"], S.MAX_SCENE_NAME), "lk": bool(sc.get("locked"))}
                             for sid, sc in strip]
        return msg

    def _active_scene(self, display_id: str) -> str | None:
        """The first scene on the display's strip that is on."""
        states = self.backend.states()
        return next((sid for sid, sc in self.registry.display_scenes(display_id) if S.is_on(sc, states)), None)

    def _send_active(self, s: DisplaySession, force: bool = False) -> None:
        active = self._active_scene(s.id)
        if force or active != s.active:
            s.active = active
            s.send({"t": "active", "s": active or ""})

    def push_layout(self, display_id: str) -> None:
        s = self.sessions.get(display_id)
        if not s:
            return
        s.send(self.layout_msg(display_id))
        for c in self.registry.tiles(display_id):
            s.send(E.state_msg(c["entity"], self.backend.get(c["entity"]), c))
        if self.registry.display_scenes(display_id):
            self._send_active(s, force=True)

    def _on_state(self, e: E.EntityState) -> None:
        for s in list(self.sessions.values()):
            for c in self.registry.tiles(s.id):
                if c["entity"] == e.entity_id:
                    s.send(E.state_msg(e.entity_id, e, c))
            if any(e.entity_id in sc.get("entities", {}) for _, sc in self.registry.display_scenes(s.id)):
                self._send_active(s)
        self._admin_broadcast({"t": "entity", "entity": e.as_dict()})
        if any(e.entity_id in sc.get("entities", {}) for sc in self.registry.scenes().values()):
            self._admin_scenes(only_if_changed=True)

    async def act(self, entity_id: str, action: str, value=None) -> None:
        domain, service, data = E.action_to_service(entity_id, self.backend.get(entity_id), action, value)
        await self.backend.call_service(domain, service, data)

    async def apply_scene(self, scene_id: str) -> list[str]:
        sc = self.registry.scene(scene_id)
        if not sc:
            raise S.SceneError(f"no scene {scene_id}")
        log.info("applying scene %s (%s)", sc["name"], scene_id)
        return await S.apply(sc, self.backend.states(), self.act)

    def capture_scene(self, scene_id: str, entity_ids=None) -> list[str]:
        """Set a scene to how its entities (or `entity_ids`) are now. Entities
        that are unavailable keep their old values. Returns those skipped."""
        sc = self.registry.scene(scene_id)
        if not sc:
            raise S.SceneError(f"no scene {scene_id}")
        ids = list(entity_ids) if entity_ids is not None else list(sc.get("entities", {}))
        values, skipped = {}, []
        for eid in ids:
            v = S.capture(eid, self.backend.get(eid))
            if v is None:
                skipped.append(eid)
                if eid in sc.get("entities", {}):
                    values[eid] = sc["entities"][eid]
            else:
                values[eid] = v
        self.registry.set_scene_values(scene_id, values)
        self.scenes_changed()
        return skipped

    def scenes_changed(self, displays=None) -> None:
        """After scenes were edited: new layouts for displays showing them
        (names, locks, which is on) and a fresh list for the admin page."""
        for display_id in (displays if displays is not None else list(self.sessions)):
            if display_id in self.sessions:
                self.push_layout(display_id)
        self._admin_scenes()
        self._admin_displays()

    def identify(self, display_id: str, seconds: int = 10) -> bool:
        s = self.sessions.get(display_id)
        if s:
            s.send({"t": "identify", "s": seconds})
        return bool(s)

    # ------------------------------------------------------------ the display socket

    async def display_ws(self, request: web.Request) -> web.WebSocketResponse:
        ws = web.WebSocketResponse(heartbeat=30, max_msg_size=4096)
        await ws.prepare(request)
        try:
            first = await ws.receive(timeout=10)
        except asyncio.TimeoutError:
            await ws.close(message=b"no hello")
            return ws
        try:
            hello = json.loads(first.data) if first.type == WSMsgType.TEXT else {}
        except ValueError:
            hello = {}
        display_id = hello.get("id") if hello.get("t") == "hello" else None
        try:
            if not isinstance(display_id, str):
                raise ConfigError("expected hello")
            hello["ip"] = hello.get("ip") or request.remote
            self.registry.seen(display_id, hello)
        except ConfigError as err:
            await ws.send_json({"t": "error", "msg": str(err)})
            await ws.close()
            return ws

        old = self.sessions.pop(display_id, None)
        if old:
            old.close()
            await old.ws.close(message=b"replaced by a newer connection")
        session = DisplaySession(display_id, ws, hello)
        self.sessions[display_id] = session
        log.info("display %s connected from %s (fw %s)", display_id, hello["ip"], hello.get("fw"))
        self.push_layout(display_id)
        self._admin_displays()
        try:
            async for msg in ws:
                if msg.type != WSMsgType.TEXT:
                    continue
                try:
                    m = json.loads(msg.data)
                except ValueError:
                    continue
                await self._from_display(session, m)
        finally:
            session.close()
            if self.sessions.get(display_id) is session:
                del self.sessions[display_id]
                log.info("display %s disconnected", display_id)
                self._admin_displays()
        return ws

    async def _from_display(self, s: DisplaySession, m: dict) -> None:
        t = m.get("t")
        if t == "act":
            entity_id = m.get("e")
            # A display can only drive what is on it.
            if entity_id not in {c["entity"] for c in self.registry.tiles(s.id)}:
                s.send({"t": "error", "msg": f"{entity_id} is not on this display"})
                return
            try:
                await self.act(entity_id, m.get("a"), m.get("v"))
            except Exception as err:      # backend or mapping: tell the display, undo its guess
                log.warning("%s: %s %s failed: %s", s.id, entity_id, m.get("a"), err)
                s.send({"t": "error", "msg": E.clip(str(err), 80)})
                cfg = next(c for c in self.registry.tiles(s.id) if c["entity"] == entity_id)
                s.send(E.state_msg(entity_id, self.backend.get(entity_id), cfg))
        elif t in ("scene", "scene_save"):
            scene_id = m.get("s")
            strip = dict(self.registry.display_scenes(s.id))
            # A display can only use the scenes on its strip.
            if scene_id not in strip:
                s.send({"t": "error", "msg": "that scene is not on this display"})
                return
            name = strip[scene_id]["name"]
            if t == "scene":
                failed = await self.apply_scene(scene_id)
                if failed:
                    s.send({"t": "error", "msg": E.clip(f"{name}: {len(failed)} did not change. {failed[0]}", 80)})
                # The display guessed this scene is now on; correct it if that is wrong.
                self._send_active(s, force=bool(failed))
            elif strip[scene_id].get("locked"):
                s.send({"t": "error", "msg": E.clip(f"{name} is locked; change it in the hub", 80)})
            else:
                skipped = self.capture_scene(scene_id)
                log.info("%s saved scene %s", s.id, name)
                note = f"Saved {name}" + (f" ({len(skipped)} unavailable, kept)" if skipped else "")
                s.send({"t": "note", "msg": E.clip(note, 80)})
        elif t == "ping":
            s.send({"t": "pong"})

    # ------------------------------------------------------------ admin

    def displays_json(self) -> list[dict]:
        out = []
        for display_id, d in sorted(self.registry.displays().items()):
            s = self.sessions.get(display_id)
            out.append({"id": display_id, "name": d.get("name", ""), "pages": d.get("pages", []),
                        "scenes": d.get("scenes", []),
                        "info": d.get("info", {}), "last_seen": d.get("last_seen"),
                        "online": bool(s), "since": s.since if s else None})
        return out

    def _admin_broadcast(self, msg: dict) -> None:
        data = json.dumps(msg, ensure_ascii=False)
        for ws in list(self.admins):
            asyncio.ensure_future(self._admin_send(ws, data))

    async def _admin_send(self, ws: web.WebSocketResponse, data: str) -> None:
        try:
            await ws.send_str(data)
        except (ConnectionResetError, RuntimeError):
            self.admins.discard(ws)

    def scenes_json(self) -> list[dict]:
        states = self.backend.states()
        used: dict[str, list[str]] = {}
        for display_id, d in self.registry.displays().items():
            for sid in d.get("scenes", []):
                used.setdefault(sid, []).append(display_id)
        return sorted(({"id": sid, "name": sc["name"], "entities": sc.get("entities", {}),
                        "locked": bool(sc.get("locked")), "on": S.is_on(sc, states),
                        "displays": used.get(sid, [])}
                       for sid, sc in self.registry.scenes().items()), key=lambda x: x["name"].lower())

    def _admin_scenes(self, only_if_changed: bool = False) -> None:
        scenes = self.scenes_json()
        on = {x["id"] for x in scenes if x["on"]}
        if only_if_changed and on == self._scenes_on:
            return
        self._scenes_on = on
        self._admin_broadcast({"t": "scenes", "scenes": scenes})

    def _admin_displays(self) -> None:
        self._admin_broadcast({"t": "displays", "displays": self.displays_json()})

    async def admin_ws(self, request: web.Request) -> web.WebSocketResponse:
        ws = web.WebSocketResponse(heartbeat=30)
        await ws.prepare(request)
        self.admins.add(ws)
        await ws.send_json({"t": "displays", "displays": self.displays_json()})
        await ws.send_json({"t": "scenes", "scenes": self.scenes_json()})
        try:
            async for _ in ws:
                pass
        finally:
            self.admins.discard(ws)
        return ws

    async def api_info(self, request: web.Request) -> web.Response:
        return web.json_response({
            "backend": self.backend.name, "status": self.backend.status(),
            "version": __version__, "proto": PROTO,
            "limits": {"pages": E.MAX_PAGES, "tiles_per_page": E.MAX_TILES_PER_PAGE,
                       "tiles": E.MAX_TILES, "name": E.MAX_NAME, "page_name": E.MAX_PAGE_NAME,
                       "scenes": S.MAX_SCENES, "display_scenes": S.MAX_DISPLAY_SCENES,
                       "scene_entities": S.MAX_SCENE_ENTITIES, "scene_name": S.MAX_SCENE_NAME},
            "scene_kinds": list(S.SCENE_KINDS),
            "kinds": E.KIND_BY_DOMAIN,
        })

    async def api_entities(self, request: web.Request) -> web.Response:
        states = sorted(self.backend.states().values(), key=lambda e: e.entity_id)
        return web.json_response([e.as_dict() for e in states if E.tile_kind(e.entity_id)])

    async def api_entity_action(self, request: web.Request) -> web.Response:
        body = await request.json()
        try:
            await self.act(request.match_info["entity_id"], body.get("a"), body.get("v"))
        except (E.ActionError, ValueError) as err:
            return web.json_response({"ok": False, "err": str(err)}, status=400)
        return web.json_response({"ok": True})

    async def api_mock_state(self, request: web.Request) -> web.Response:
        """Mock backend only: set an entity's state directly, as if a sensor
        reported it or someone used a wall switch."""
        set_state = getattr(self.backend, "set_state", None)
        if not set_state:
            raise web.HTTPNotFound(text="only the mock backend can set states directly")
        body = await request.json()
        entity_id = request.match_info["entity_id"]
        if not self.backend.get(entity_id) or not isinstance(body.get("state"), str):
            return web.json_response({"ok": False, "err": "unknown entity or no state"}, status=400)
        set_state(entity_id, body["state"], **(body.get("attributes") or {}))
        return web.json_response({"ok": True})

    async def api_displays(self, request: web.Request) -> web.Response:
        return web.json_response(self.displays_json())

    async def api_display_put(self, request: web.Request) -> web.Response:
        display_id = request.match_info["display_id"]
        try:
            self.registry.set_layout(display_id, await request.json())
        except (ConfigError, ValueError) as err:
            return web.json_response({"ok": False, "err": str(err)}, status=400)
        self.push_layout(display_id)
        self._admin_displays()
        return web.json_response({"ok": True})

    async def api_display_delete(self, request: web.Request) -> web.Response:
        display_id = request.match_info["display_id"]
        if not self.registry.delete(display_id):
            raise web.HTTPNotFound()
        self.push_layout(display_id)          # an online display shows "not set up" again
        self._admin_displays()
        return web.json_response({"ok": True})

    # ------------------------------------------------------------ admin: scenes

    async def api_scenes(self, request: web.Request) -> web.Response:
        return web.json_response(self.scenes_json())

    async def api_scene_post(self, request: web.Request) -> web.Response:
        """A new scene: {name, entities, locked}, or {name, capture: [entity ids]}
        to take the entities' current state."""
        body = await request.json()
        capture = body.get("capture") if isinstance(body, dict) else None
        try:
            if capture is not None:
                if not isinstance(capture, list) or not all(isinstance(x, str) for x in capture):
                    raise ConfigError("capture must be a list of entity ids")
                values = {eid: v for eid in capture
                          if (v := S.capture(eid, self.backend.get(eid))) is not None}
                if not values:
                    raise ConfigError("none of those entities can be captured (unavailable, or not a light, switch or thermostat)")
                body = {**body, "entities": values}
            scene_id = self.registry.add_scene(body)
        except (ConfigError, ValueError) as err:
            return web.json_response({"ok": False, "err": str(err)}, status=400)
        self.scenes_changed()
        return web.json_response({"ok": True, "id": scene_id})

    async def api_scene_put(self, request: web.Request) -> web.Response:
        scene_id = request.match_info["scene_id"]
        try:
            self.registry.set_scene(scene_id, await request.json())
        except (ConfigError, ValueError) as err:
            return web.json_response({"ok": False, "err": str(err)}, status=400)
        self.scenes_changed()
        return web.json_response({"ok": True})

    async def api_scene_delete(self, request: web.Request) -> web.Response:
        scene_id = request.match_info["scene_id"]
        if not self.registry.scene(scene_id):
            raise web.HTTPNotFound()
        had = self.registry.delete_scene(scene_id)
        self.scenes_changed(had)
        return web.json_response({"ok": True})

    async def api_scene_apply(self, request: web.Request) -> web.Response:
        try:
            failed = await self.apply_scene(request.match_info["scene_id"])
        except S.SceneError as err:
            return web.json_response({"ok": False, "err": str(err)}, status=404)
        return web.json_response({"ok": not failed, "failed": failed,
                                  "err": "; ".join(failed) if failed else None})

    async def api_scene_capture(self, request: web.Request) -> web.Response:
        """Take the current state of the scene's entities, or of the
        entities listed in the body: {"entities": [ids]}."""
        body = await request.json() if request.can_read_body else {}
        ids = body.get("entities") if isinstance(body, dict) else None
        try:
            skipped = self.capture_scene(request.match_info["scene_id"], ids)
        except (S.SceneError, ConfigError, ValueError) as err:
            return web.json_response({"ok": False, "err": str(err)}, status=400)
        return web.json_response({"ok": True, "skipped": skipped})

    async def api_identify(self, request: web.Request) -> web.Response:
        ok = self.identify(request.match_info["display_id"])
        return web.json_response({"ok": ok, "err": None if ok else "that display is not connected"},
                                 status=200 if ok else 409)


def make_app(hub: Hub) -> web.Application:
    @web.middleware
    async def guard(request: web.Request, handler):
        if (hub.admin_allowed and request.path not in PUBLIC_PATHS
                and not request.path.startswith("/static/") and not hub.admin_allowed(request.remote)):
            raise web.HTTPForbidden(text="The hub's admin page is only open through Home Assistant "
                                         "(Settings > Add-ons > Display Hub > Open web UI).")
        return await handler(request)

    app = web.Application(middlewares=[guard])

    def page(name):
        async def handler(request):
            return web.FileResponse(STATIC / name)
        return handler

    app.add_routes([
        web.get("/", page("index.html")),
        web.get("/virtual", page("virtual.html")),
        web.get("/ws", hub.display_ws),
        web.get("/api/live", hub.admin_ws),
        web.get("/api/info", hub.api_info),
        web.get("/api/entities", hub.api_entities),
        web.post("/api/entities/{entity_id}/action", hub.api_entity_action),
        web.post("/api/mock/{entity_id}", hub.api_mock_state),
        web.get("/api/displays", hub.api_displays),
        web.put("/api/displays/{display_id}", hub.api_display_put),
        web.delete("/api/displays/{display_id}", hub.api_display_delete),
        web.post("/api/displays/{display_id}/identify", hub.api_identify),
        web.get("/api/scenes", hub.api_scenes),
        web.post("/api/scenes", hub.api_scene_post),
        web.put("/api/scenes/{scene_id}", hub.api_scene_put),
        web.delete("/api/scenes/{scene_id}", hub.api_scene_delete),
        web.post("/api/scenes/{scene_id}/apply", hub.api_scene_apply),
        web.post("/api/scenes/{scene_id}/capture", hub.api_scene_capture),
    ])
    app.router.add_static("/static/", STATIC)

    async def lifecycle(app):
        await hub.backend.start()
        yield
        await hub.backend.stop()
        for s in list(hub.sessions.values()):
            s.close()
            await s.ws.close()
        for ws in list(hub.admins):
            await ws.close()
    app.cleanup_ctx.append(lifecycle)
    return app


# ---------------------------------------------------------------- mDNS

def lan_ip() -> str:
    """The address other machines on the LAN reach this one at."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("10.255.255.255", 1))      # no packet is sent
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


async def advertise(port: int, ip: str | None = None):
    """Announce the hub as _hadisplay._tcp so displays find it without being
    told an address. Returns something to pass to unadvertise(), or None if
    the zeroconf package is not installed."""
    try:
        from zeroconf import ServiceInfo
        from zeroconf.asyncio import AsyncZeroconf
    except ImportError:
        log.warning("zeroconf is not installed: displays will not find the hub by themselves")
        return None
    ip = ip or lan_ip()
    host = socket.gethostname().split(".")[0]
    info = ServiceInfo(
        "_hadisplay._tcp.local.", f"HA display hub on {host}._hadisplay._tcp.local.",
        addresses=[socket.inet_aton(ip)], port=port,
        properties={"path": "/ws", "proto": str(PROTO)}, server=f"{host}.local.")
    zc = AsyncZeroconf()
    await zc.async_register_service(info)
    log.info("advertising _hadisplay._tcp at %s:%d", ip, port)
    return zc, info


async def unadvertise(handle) -> None:
    if handle:
        zc, info = handle
        await zc.async_unregister_service(info)
        await zc.async_close()
