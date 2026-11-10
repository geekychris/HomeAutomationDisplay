"""The Home Assistant backend against a small fake of HA's WebSocket API."""
import asyncio
import json
import unittest

from aiohttp import WSMsgType, web
from aiohttp.test_utils import TestClient, TestServer

from had_hub.__main__ import allow_from
from had_hub.backends.homeassistant import HomeAssistantBackend, websocket_url
from had_hub.backends.mock import MockBackend
from had_hub.registry import Registry
from had_hub.server import Hub, make_app

TOKEN = "secret"


class FakeHA:
    """Enough of HA's WebSocket API for the backend: auth, get_states,
    subscribe_events, the three registry lists and call_service."""

    def __init__(self):
        self.states = {
            "light.kitchen": {"entity_id": "light.kitchen", "state": "off",
                              "attributes": {"friendly_name": "Kitchen", "supported_color_modes": ["brightness"],
                                             "entity_picture": "/big/thing.png"}},
            "sensor.co2": {"entity_id": "sensor.co2", "state": "812.4",
                           "attributes": {"friendly_name": "CO2", "device_class": "carbon_dioxide",
                                          "unit_of_measurement": "ppm"}},
            "media_player.tv": {"entity_id": "media_player.tv", "state": "playing", "attributes": {}},
        }
        self.calls = []
        self.sockets = []
        self.subscribers = []
        self.app = web.Application()
        self.app.router.add_get("/api/websocket", self.ws)

    async def ws(self, request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        self.sockets.append(ws)
        await ws.send_json({"type": "auth_required", "ha_version": "2026.11.0"})
        auth = await ws.receive_json()
        if auth.get("access_token") != TOKEN:
            await ws.send_json({"type": "auth_invalid", "message": "Invalid access token"})
            await ws.close()
            return ws
        await ws.send_json({"type": "auth_ok", "ha_version": "2026.11.0"})
        async for raw in ws:
            if raw.type != WSMsgType.TEXT:
                continue
            m = json.loads(raw.data)
            t, i = m["type"], m["id"]
            if t == "get_states":
                await ws.send_json({"id": i, "type": "result", "success": True, "result": list(self.states.values())})
            elif t == "subscribe_events":
                self.subscribers.append((ws, i))
                await ws.send_json({"id": i, "type": "result", "success": True, "result": None})
            elif t == "config/area_registry/list":
                await ws.send_json({"id": i, "type": "result", "success": True,
                                    "result": [{"area_id": "kitchen", "name": "Kitchen"}]})
            elif t == "config/device_registry/list":
                await ws.send_json({"id": i, "type": "result", "success": True,
                                    "result": [{"id": "dev1", "area_id": "kitchen"}]})
            elif t == "config/entity_registry/list":
                await ws.send_json({"id": i, "type": "result", "success": True, "result": [
                    {"entity_id": "light.kitchen", "device_id": "dev1", "area_id": None},
                    {"entity_id": "sensor.co2", "area_id": None, "options": {"sensor": {"display_precision": 1}}}]})
            elif t == "call_service":
                self.calls.append((m["domain"], m["service"], m.get("target"), m.get("service_data")))
                entity_id = (m.get("target") or {}).get("entity_id")
                if entity_id not in self.states:
                    await ws.send_json({"id": i, "type": "result", "success": False,
                                        "error": {"code": "not_found", "message": "Entity not found"}})
                    continue
                await ws.send_json({"id": i, "type": "result", "success": True, "result": {}})
                await self.change(entity_id, "on" if m["service"] in ("turn_on", "toggle") else "off")
        return ws

    async def change(self, entity_id, state, **attrs):
        new = {**self.states[entity_id], "state": state}
        new["attributes"] = {**new["attributes"], **attrs}
        self.states[entity_id] = new
        for ws, sub in self.subscribers:
            if not ws.closed:
                await ws.send_json({"id": sub, "type": "event", "event": {
                    "event_type": "state_changed", "data": {"entity_id": entity_id, "new_state": new}}})


async def until(cond, timeout=3):
    for _ in range(int(timeout / 0.02)):
        if cond():
            return
        await asyncio.sleep(0.02)
    raise AssertionError("timed out")


class HomeAssistantTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.ha = FakeHA()
        self.server = TestServer(self.ha.app)
        await self.server.start_server()
        self.url = f"http://{self.server.host}:{self.server.port}"

    async def asyncTearDown(self):
        await self.server.close()

    async def test_url(self):
        self.assertEqual(websocket_url("http://ha.local:8123/"), "ws://ha.local:8123/api/websocket")
        self.assertEqual(websocket_url("https://ha.example"), "wss://ha.example/api/websocket")
        self.assertEqual(websocket_url("ws://supervisor/core/websocket"), "ws://supervisor/core/websocket")

    async def test_loads_follows_and_calls(self):
        b = HomeAssistantBackend(self.url, TOKEN)
        seen = []
        b.subscribe(seen.append)
        await b.start()
        try:
            await until(lambda: b.status()["connected"])
            self.assertEqual(sorted(b.states()), ["light.kitchen", "sensor.co2"])   # no media_player
            light = b.get("light.kitchen")
            self.assertEqual(light.attributes["area"], "Kitchen")                    # from the device
            self.assertNotIn("entity_picture", light.attributes)                     # trimmed
            self.assertEqual(b.get("sensor.co2").attributes["suggested_display_precision"], 1)
            self.assertIn("2026.11.0", b.status()["detail"])

            await b.call_service("light", "turn_on", {"entity_id": "light.kitchen", "brightness_pct": 40})
            self.assertEqual(self.ha.calls[-1], ("light", "turn_on", {"entity_id": "light.kitchen"},
                                                 {"brightness_pct": 40}))
            await until(lambda: b.get("light.kitchen").state == "on")
            self.assertEqual(seen[-1].entity_id, "light.kitchen")

            with self.assertRaisesRegex(ValueError, "Entity not found"):
                await b.call_service("light", "turn_on", {"entity_id": "light.nope"})
        finally:
            await b.stop()

    async def test_reconnects_and_resyncs(self):
        b = HomeAssistantBackend(self.url, TOKEN)
        await b.start()
        try:
            await until(lambda: b.status()["connected"])
            del self.ha.states["sensor.co2"]
            self.ha.states["light.kitchen"]["state"] = "on"
            for ws in list(self.ha.sockets):
                await ws.close()                     # HA restarts
            await until(lambda: not b.status()["connected"])
            with self.assertRaises(ValueError):
                await b.call_service("light", "toggle", {"entity_id": "light.kitchen"})
            await until(lambda: b.status()["connected"], timeout=5)
            self.assertEqual(b.get("light.kitchen").state, "on")
            self.assertEqual(b.get("sensor.co2").state, "unavailable")
        finally:
            await b.stop()

    async def test_bad_token(self):
        b = HomeAssistantBackend(self.url, "wrong")
        await b.start()
        try:
            await until(lambda: "token refused" in b.status()["detail"])
            self.assertFalse(b.status()["connected"])
        finally:
            await b.stop()


class AdminGuardTests(unittest.IsolatedAsyncioTestCase):
    async def test_only_displays_from_outside(self):
        hub = Hub(MockBackend(tick=0), Registry(None), admin_allowed=allow_from(["172.30.32.2"]))
        async with TestClient(TestServer(make_app(hub))) as client:
            self.assertEqual((await client.get("/")).status, 403)
            self.assertEqual((await client.get("/api/displays")).status, 403)
            self.assertEqual((await client.get("/virtual")).status, 200)
            ws = await client.ws_connect("/ws")
            await ws.send_json({"t": "hello", "id": "cyd-x", "proto": 1})
            self.assertEqual(json.loads((await ws.receive(timeout=2)).data)["t"], "layout")
            await ws.close()

    def test_allow_from(self):
        allowed = allow_from(["192.168.1.0/24", "10.0.0.5"])
        self.assertTrue(allowed("192.168.1.77"))
        self.assertTrue(allowed("10.0.0.5"))
        self.assertFalse(allowed("10.0.0.6"))
        self.assertFalse(allowed(None))


if __name__ == "__main__":
    unittest.main()
