import asyncio
import json
import tempfile
import unittest
from pathlib import Path

from aiohttp.test_utils import TestClient, TestServer

from had_hub import entities as E
from had_hub.backends.mock import MockBackend
from had_hub.registry import ConfigError, Registry, validate_layout
from had_hub.server import Hub, make_app


class EntityTests(unittest.TestCase):
    def setUp(self):
        self.b = MockBackend(tick=0, seed=1)

    def test_light_tile_and_state(self):
        e = self.b.get("light.living_room")
        self.assertEqual(E.tile_def(e.entity_id, e, {}), {"e": "light.living_room", "k": "light", "n": "Living room", "dim": True})
        self.assertEqual(E.state_msg(e.entity_id, e)["bri"], 71)
        porch = self.b.get("light.porch")
        self.assertFalse(E.tile_def(porch.entity_id, porch, {})["dim"])

    def test_sensor_levels(self):
        self.b.set_state("sensor.living_room_co2", "1200")
        e = self.b.get("sensor.living_room_co2")
        m = E.state_msg(e.entity_id, e)
        self.assertEqual((m["val"], m["unit"], m["lvl"]), ("1200", "ppm", E.LEVEL_WARN))
        self.assertEqual(E.state_msg(e.entity_id, e, {"bad": 1100})["lvl"], E.LEVEL_BAD)
        t = self.b.get("sensor.living_room_temperature")
        self.assertEqual(E.state_msg(t.entity_id, t)["lvl"], E.LEVEL_NONE)

    def test_binary_sensor_words(self):
        e = self.b.get("binary_sensor.front_door")
        self.assertEqual(E.state_msg(e.entity_id, e)["val"], "Closed")

    def test_unavailable(self):
        self.b.set_state("light.kitchen", "unavailable")
        e = self.b.get("light.kitchen")
        self.assertEqual(E.state_msg(e.entity_id, e), {"t": "state", "e": "light.kitchen", "ok": False})
        self.assertEqual(E.state_msg("light.nope", None)["ok"], False)
        with self.assertRaises(E.ActionError):
            E.action_to_service("light.kitchen", e, "toggle")

    def test_actions(self):
        light = self.b.get("light.kitchen")
        self.assertEqual(E.action_to_service(light.entity_id, light, "bri", 40),
                         ("light", "turn_on", {"entity_id": "light.kitchen", "brightness_pct": 40}))
        self.assertEqual(E.action_to_service(light.entity_id, light, "bri", 0)[1], "turn_off")
        heater = self.b.get("climate.house_heater")
        self.assertEqual(E.action_to_service(heater.entity_id, heater, "target", 99)[2]["temperature"], 30)
        with self.assertRaises(E.ActionError):
            E.action_to_service(heater.entity_id, heater, "mode", "cool")
        with self.assertRaises(E.ActionError):
            E.action_to_service(light.entity_id, light, "target", 20)

    def test_clip_keeps_characters_whole(self):
        self.assertEqual(E.clip("ééé", 5), "éé")


class MockTests(unittest.IsolatedAsyncioTestCase):
    async def test_services(self):
        b = MockBackend(tick=0, seed=1)
        seen = []
        b.subscribe(seen.append)
        await b.call_service("light", "turn_on", {"entity_id": "light.kitchen", "brightness_pct": 50})
        self.assertEqual(b.get("light.kitchen").attributes["brightness"], 128)
        await b.call_service("switch", "toggle", {"entity_id": "switch.coffee_maker"})
        self.assertEqual(b.get("switch.coffee_maker").state, "on")
        await b.call_service("climate", "set_temperature", {"entity_id": "climate.house_heater", "temperature": 25})
        self.assertEqual(b.get("climate.house_heater").attributes["hvac_action"], "heating")
        self.assertEqual([e.entity_id for e in seen], ["light.kitchen", "switch.coffee_maker", "climate.house_heater"])

    async def test_heater_warms_the_room(self):
        b = MockBackend(tick=0, seed=1)
        await b.call_service("climate", "set_temperature", {"entity_id": "climate.house_heater", "temperature": 25})
        before = float(b.get("sensor.living_room_temperature").state)
        for _ in range(10):
            b.step()
        self.assertGreater(float(b.get("sensor.living_room_temperature").state), before + 1)


class RegistryTests(unittest.TestCase):
    def test_validation(self):
        ok = validate_layout({"name": "Kitchen", "pages": [{"name": "Main", "tiles": ["light.kitchen", {"entity": "sensor.living_room_co2", "warn": "900"}]}]})
        self.assertEqual(ok["pages"][0]["tiles"][1], {"entity": "sensor.living_room_co2", "warn": 900.0})
        for bad in ({"pages": [{"tiles": ["nonsense"]}]},
                    {"pages": [{"tiles": ["camera.door"]}]},
                    {"pages": [{"tiles": ["light.a"] * 9}]},
                    {"pages": [{"name": "x" * 20, "tiles": []}]},
                    {"pages": [{"tiles": []}] * 7}):
            with self.assertRaises(ConfigError):
                validate_layout(bad)

    def test_persists(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "displays.json"
            r = Registry(path)
            r.seen("cyd-1", {"name": "hall", "fw": "0.1"})
            r.set_layout("cyd-1", {"name": "Hall", "pages": [{"name": "A", "tiles": ["light.kitchen"]}]})
            r2 = Registry(path)
            self.assertEqual(r2.tiles("cyd-1"), [{"entity": "light.kitchen"}])
            self.assertEqual(r2.get("cyd-1")["info"]["fw"], "0.1")


class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.backend = MockBackend(tick=0, seed=1)
        self.hub = Hub(self.backend, Registry(None))
        self.client = TestClient(TestServer(make_app(self.hub)))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close()

    async def connect(self, display_id="cyd-test"):
        ws = await self.client.ws_connect("/ws")
        await ws.send_json({"t": "hello", "id": display_id, "name": "test", "fw": "0", "proto": 1})
        return ws

    async def recv(self, ws):
        return json.loads((await ws.receive(timeout=2)).data)

    async def test_new_display_is_not_set_up(self):
        ws = await self.connect()
        m = await self.recv(ws)
        self.assertEqual((m["t"], m["pages"]), ("layout", []))
        self.assertIn("cyd-test", m["msg"])
        displays = await (await self.client.get("/api/displays")).json()
        self.assertEqual([(d["id"], d["online"]) for d in displays], [("cyd-test", True)])

    async def test_assign_then_control(self):
        ws = await self.connect()
        await self.recv(ws)
        r = await self.client.put("/api/displays/cyd-test", json={
            "name": "Kitchen", "pages": [{"name": "Main", "tiles": ["light.kitchen", "climate.house_heater"]}]})
        self.assertEqual(r.status, 200)
        layout = await self.recv(ws)
        self.assertEqual(layout["title"], "Kitchen")
        self.assertEqual([t["k"] for t in layout["pages"][0]["tiles"]], ["light", "climate"])
        states = {m["e"]: m for m in [await self.recv(ws), await self.recv(ws)]}
        self.assertFalse(states["light.kitchen"]["on"])
        self.assertEqual(states["climate.house_heater"]["tgt"], 20.5)

        await ws.send_json({"t": "act", "e": "light.kitchen", "a": "bri", "v": 30})
        m = await self.recv(ws)
        self.assertEqual((m["e"], m["on"], m["bri"]), ("light.kitchen", True, 30))

        # A change made elsewhere reaches the display too.
        self.backend.set_state("light.kitchen", "off")
        self.assertFalse((await self.recv(ws))["on"])

    async def test_display_cannot_drive_other_entities(self):
        self.hub.registry.set_layout("cyd-test", {"pages": [{"tiles": ["light.kitchen"]}]})
        ws = await self.connect()
        await self.recv(ws)
        await self.recv(ws)
        await ws.send_json({"t": "act", "e": "switch.garden_pump", "a": "toggle"})
        self.assertEqual((await self.recv(ws))["t"], "error")
        self.assertEqual(self.backend.get("switch.garden_pump").state, "off")

    async def test_bad_action_restores_state(self):
        self.hub.registry.set_layout("cyd-test", {"pages": [{"tiles": ["climate.house_heater"]}]})
        ws = await self.connect()
        await self.recv(ws)
        await self.recv(ws)
        await ws.send_json({"t": "act", "e": "climate.house_heater", "a": "mode", "v": "cool"})
        self.assertEqual((await self.recv(ws))["t"], "error")
        self.assertEqual((await self.recv(ws))["mode"], "heat")

    async def test_two_displays_share_an_entity(self):
        for d in ("cyd-a", "cyd-b"):
            self.hub.registry.set_layout(d, {"pages": [{"tiles": ["switch.coffee_maker"]}]})
        a, b = await self.connect("cyd-a"), await self.connect("cyd-b")
        for ws in (a, b):
            await self.recv(ws)
            await self.recv(ws)
        await a.send_json({"t": "act", "e": "switch.coffee_maker", "a": "toggle"})
        self.assertTrue((await self.recv(a))["on"])
        self.assertTrue((await self.recv(b))["on"])

    async def test_identify_and_reconnect(self):
        first = await self.connect()
        await self.recv(first)
        second = await self.connect()          # the same display again, e.g. after a reboot
        await self.recv(second)
        await asyncio.sleep(0.05)
        self.assertTrue(first.closed or (await first.receive(timeout=1)).type.name in ("CLOSE", "CLOSED", "CLOSING"))
        r = await self.client.post("/api/displays/cyd-test/identify")
        self.assertEqual(r.status, 200)
        self.assertEqual(await self.recv(second), {"t": "identify", "s": 10})

    async def test_admin_live_feed(self):
        live = await self.client.ws_connect("/api/live")
        self.assertEqual((await self.recv(live))["t"], "displays")
        r = await self.client.post("/api/entities/switch.garden_pump/action", json={"a": "on"})
        self.assertEqual(r.status, 200)
        m = await self.recv(live)
        self.assertEqual((m["t"], m["entity"]["state"]), ("entity", "on"))


if __name__ == "__main__":
    unittest.main()
