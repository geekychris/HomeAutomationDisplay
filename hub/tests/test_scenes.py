import json
import tempfile
import unittest
from pathlib import Path

from aiohttp.test_utils import TestClient, TestServer

from had_hub import scenes as S
from had_hub.backends.mock import MockBackend
from had_hub.registry import ConfigError, Registry
from had_hub.server import Hub, make_app

EVENING = {
    "name": "Evening",
    "entities": {
        "light.living_room": {"on": True, "level": 30},
        "light.kitchen": {"on": False},
        "fan.bedroom": {"on": True, "level": 50},
        "cover.living_room_blind": {"on": False},
        "lock.front_door": {"on": True},
        "climate.house_heater": {"mode": "heat", "target": 21.5},
    },
}


class SceneLogicTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.b = MockBackend(tick=0, seed=1)
        self.hub = Hub(self.b, Registry(None))

    def test_capture(self):
        self.assertEqual(S.capture("light.living_room", self.b.get("light.living_room")), {"on": True, "level": 71})
        self.assertEqual(S.capture("light.kitchen", self.b.get("light.kitchen")), {"on": False})
        self.assertEqual(S.capture("light.porch", self.b.get("light.porch")), {"on": False})
        self.assertEqual(S.capture("lock.front_door", self.b.get("lock.front_door")), {"on": True})
        self.assertEqual(S.capture("climate.house_heater", self.b.get("climate.house_heater")),
                         {"mode": "heat", "target": 20.5})
        self.assertIsNone(S.capture("sensor.living_room_co2", self.b.get("sensor.living_room_co2")))
        self.b.set_state("light.kitchen", "unavailable")
        self.assertIsNone(S.capture("light.kitchen", self.b.get("light.kitchen")))

    async def test_apply_then_on_then_off_by_hand(self):
        states = self.b.states()
        self.assertFalse(S.is_on(EVENING, states))
        failed = await S.apply(EVENING, states, self.hub.act)
        self.assertEqual(failed, [])
        self.assertEqual(self.b.get("light.living_room").attributes["brightness"], round(30 * 255 / 100))
        self.assertEqual(self.b.get("cover.living_room_blind").state, "closed")
        self.assertEqual(self.b.get("climate.house_heater").attributes["temperature"], 21.5)
        self.assertTrue(S.is_on(EVENING, self.b.states()))
        # Close enough still counts; someone turning a light up does not.
        await self.hub.act("light.living_room", "bri", 32)
        self.assertTrue(S.is_on(EVENING, self.b.states()))
        await self.hub.act("light.living_room", "bri", 80)
        self.assertFalse(S.is_on(EVENING, self.b.states()))

    async def test_apply_skips_what_is_already_right_and_reports_failures(self):
        calls = []

        async def act(eid, action, value):
            calls.append(eid)
            if eid == "fan.bedroom":
                raise ValueError("fan is stuck")
            await self.hub.act(eid, action, value)

        failed = await S.apply(EVENING, self.b.states(), act)
        self.assertNotIn("light.kitchen", calls)       # already off
        self.assertNotIn("lock.front_door", calls)     # already locked
        self.assertEqual(len(failed), 1)
        self.assertIn("fan is stuck", failed[0])
        self.b.set_state("light.kitchen", "unavailable")
        failed = await S.apply(EVENING, self.b.states(), act)
        self.assertTrue(any("light.kitchen is not available" in f for f in failed))


class SceneRegistryTests(unittest.TestCase):
    def test_validation(self):
        r = Registry(None)
        sid = r.add_scene(EVENING)
        self.assertRegex(sid, r"^s[0-9a-f]{6}$")
        for bad in ({"name": ""}, {"name": "x" * 20},
                    {"name": "A", "entities": {"sensor.co2": {"on": True}}},
                    {"name": "A", "entities": {"climate.house_heater": {"mode": "warp"}}},
                    {"name": "A", "entities": {"light.kitchen": {"on": True, "level": "lots"}}}):
            with self.assertRaises(ConfigError):
                r.add_scene(bad)
        self.assertEqual(r.scene(sid)["entities"]["climate.house_heater"], {"mode": "heat", "target": 21.5})
        # A level on a light that is off means nothing, so it is dropped.
        sid2 = r.add_scene({"name": "B", "entities": {"light.kitchen": {"on": False, "level": 50}}})
        self.assertEqual(r.scene(sid2)["entities"]["light.kitchen"], {"on": False})

    def test_strip_and_delete(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "displays.json"
            r = Registry(path)
            a, b = r.add_scene(EVENING), r.add_scene({"name": "Off", "entities": {"light.kitchen": {"on": False}}})
            r.set_layout("cyd-1", {"pages": [], "scenes": [a, b, a]})
            self.assertEqual([x for x, _ in r.display_scenes("cyd-1")], [a, b])
            with self.assertRaises(ConfigError):
                r.set_layout("cyd-1", {"pages": [], "scenes": ["s000000"]})
            with self.assertRaises(ConfigError):
                r.set_layout("cyd-1", {"pages": [], "scenes": ["nonsense"]})
            r.set_layout("cyd-1", {"name": "Hall", "pages": []})     # no "scenes": strip kept
            self.assertEqual(len(r.display_scenes("cyd-1")), 2)
            self.assertEqual(r.delete_scene(a), ["cyd-1"])
            r2 = Registry(path)
            self.assertEqual([x for x, _ in r2.display_scenes("cyd-1")], [b])
            self.assertEqual(r2.scene(b)["name"], "Off")


class SceneProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.backend = MockBackend(tick=0, seed=1)
        self.hub = Hub(self.backend, Registry(None))
        self.client = TestClient(TestServer(make_app(self.hub)))
        await self.client.start_server()
        self.evening = self.hub.registry.add_scene(EVENING)
        self.locked = self.hub.registry.add_scene({"name": "Away", "locked": True,
                                                   "entities": {"light.porch": {"on": True}}})
        self.other = self.hub.registry.add_scene({"name": "Other", "entities": {"switch.garden_pump": {"on": True}}})
        self.hub.registry.set_layout("cyd-test", {"pages": [{"tiles": ["light.living_room"]}],
                                                  "scenes": [self.evening, self.locked]})

    async def asyncTearDown(self):
        await self.client.close()

    async def recv(self, ws):
        return json.loads((await ws.receive(timeout=2)).data)

    async def recv_until(self, ws, t, **want):
        for _ in range(30):
            m = await self.recv(ws)
            if m["t"] == t and all(m.get(k) == v for k, v in want.items()):
                return m
        self.fail(f"no {t} message like {want}")

    async def connect(self):
        ws = await self.client.ws_connect("/ws")
        await ws.send_json({"t": "hello", "id": "cyd-test", "fw": "0", "proto": 1})
        layout = await self.recv(ws)
        await self.recv(ws)                                  # the tile's state
        self.assertEqual(await self.recv(ws), {"t": "active", "s": ""})
        return ws, layout

    async def test_layout_has_the_strip(self):
        _, layout = await self.connect()
        self.assertEqual(layout["scenes"], [{"id": self.evening, "n": "Evening", "lk": False},
                                            {"id": self.locked, "n": "Away", "lk": True}])

    async def test_apply_from_display_and_active_follows(self):
        ws, _ = await self.connect()
        await ws.send_json({"t": "scene", "s": self.evening})
        self.assertEqual((await self.recv_until(ws, "active"))["s"], self.evening)
        self.assertEqual(self.backend.get("climate.house_heater").attributes["temperature"], 21.5)
        # Someone turns a light up by hand: the scene is no longer on.
        self.backend.set_state("light.living_room", "on", brightness=255)
        self.assertEqual((await self.recv_until(ws, "active"))["s"], "")

    async def test_save_from_display(self):
        ws, _ = await self.connect()
        await self.hub.act("light.living_room", "bri", 55)
        await ws.send_json({"t": "scene_save", "s": self.evening})
        note = await self.recv_until(ws, "note")
        self.assertEqual(note["msg"], "Saved Evening")
        sc = self.hub.registry.scene(self.evening)
        self.assertEqual(sc["entities"]["light.living_room"], {"on": True, "level": 55})
        self.assertEqual(set(sc["entities"]), set(EVENING["entities"]))   # same entities, new values

    async def test_locked_and_foreign_scenes_are_refused(self):
        ws, _ = await self.connect()
        await ws.send_json({"t": "scene_save", "s": self.locked})
        self.assertIn("locked", (await self.recv_until(ws, "error"))["msg"])
        await ws.send_json({"t": "scene", "s": self.other})
        self.assertEqual((await self.recv_until(ws, "error"))["msg"], "that scene is not on this display")
        self.assertEqual(self.backend.get("switch.garden_pump").state, "off")

    async def test_admin_create_capture_apply_delete(self):
        ws, _ = await self.connect()
        r = await self.client.post("/api/scenes", json={"name": "Now", "capture": ["light.living_room", "sensor.living_room_co2", "lock.front_door"]})
        body = await r.json()
        self.assertEqual(r.status, 200, body)
        sid = body["id"]
        self.assertEqual(set(self.hub.registry.scene(sid)["entities"]), {"light.living_room", "lock.front_door"})
        scenes = {s["id"]: s for s in await (await self.client.get("/api/scenes")).json()}
        self.assertTrue(scenes[sid]["on"])                 # captured from now, so it matches now
        self.assertEqual(scenes[self.evening]["displays"], ["cyd-test"])

        r = await self.client.post(f"/api/scenes/{self.evening}/apply")
        self.assertEqual((await r.json())["ok"], True)
        await self.recv_until(ws, "active", s=self.evening)

        r = await self.client.put(f"/api/scenes/{self.evening}", json={"name": "Late", "entities": EVENING["entities"]})
        self.assertEqual(r.status, 200)
        layout = await self.recv_until(ws, "layout")
        self.assertEqual(layout["scenes"][0]["n"], "Late")

        r = await self.client.delete(f"/api/scenes/{self.evening}")
        self.assertEqual(r.status, 200)
        layout = await self.recv_until(ws, "layout")
        self.assertEqual([x["id"] for x in layout["scenes"]], [self.locked])

        r = await self.client.post("/api/scenes", json={"name": "Nothing", "capture": ["sensor.living_room_co2"]})
        self.assertEqual(r.status, 400)


if __name__ == "__main__":
    unittest.main()
