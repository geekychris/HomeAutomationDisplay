"""Every Home Assistant domain the hub maps: its tile, its state message and
its service calls."""
import unittest

from had_hub import entities as E
from had_hub.backends.mock import MockBackend
from had_hub.entities import EntityState as S


def msg(entity_id, state, **attrs):
    return E.state_msg(entity_id, S(entity_id, state, attrs))


def call(entity_id, state, action, value=None, **attrs):
    return E.action_to_service(entity_id, S(entity_id, state, attrs), action, value)


class KindTests(unittest.TestCase):
    def test_every_domain_has_a_kind_the_firmware_knows(self):
        self.assertEqual(set(E.KIND_BY_DOMAIN.values()), {"light", "switch", "sensor", "climate", "button"})

    def test_fan(self):
        t = E.tile_def("fan.f", S("fan.f", "on", {"supported_features": 1}), {})
        self.assertEqual((t["k"], t["dim"], t["ic"]), ("light", True, "fan"))
        self.assertEqual(msg("fan.f", "on", percentage=33)["bri"], 33)
        self.assertEqual(call("fan.f", "on", "bri", 50), ("fan", "set_percentage", {"entity_id": "fan.f", "percentage": 50}))
        self.assertEqual(call("fan.f", "on", "bri", 0)[1], "turn_off")
        self.assertFalse(E.tile_def("fan.g", S("fan.g", "on", {"supported_features": 0}), {})["dim"])

    def test_cover(self):
        m = msg("cover.c", "open", current_position=40, supported_features=15)
        self.assertEqual((m["on"], m["bri"], m["txt"]), (True, 40, "Open 40%"))
        self.assertEqual(msg("cover.c", "closing")["txt"], "Closing")
        self.assertEqual(call("cover.c", "open", "toggle")[1], "toggle")
        self.assertEqual(call("cover.c", "open", "bri", 70, supported_features=4)[1:],
                         ("set_cover_position", {"entity_id": "cover.c", "position": 70}))
        with self.assertRaises(E.ActionError):
            call("cover.c", "open", "bri", 70, supported_features=3)

    def test_lock(self):
        m = msg("lock.l", "locked")
        self.assertEqual((m["on"], m["txt"]), (True, "Locked"))
        self.assertEqual(call("lock.l", "locked", "toggle")[1], "unlock")
        self.assertEqual(call("lock.l", "unlocked", "toggle")[1], "lock")
        self.assertEqual(call("lock.l", "unlocked", "on")[1], "lock")

    def test_switch_likes(self):
        for domain in ("switch", "input_boolean", "automation", "siren", "humidifier"):
            self.assertEqual(call(f"{domain}.x", "off", "toggle"), (domain, "toggle", {"entity_id": f"{domain}.x"}))
        self.assertEqual(msg("automation.a", "off")["txt"], "Disabled")

    def test_buttons(self):
        self.assertEqual(call("scene.s", "2026-01-01T00:00:00", "press")[:2], ("scene", "turn_on"))
        self.assertEqual(call("script.s", "off", "press")[:2], ("script", "turn_on"))
        self.assertEqual(call("button.b", "2026-01-01T00:00:00", "press")[:2], ("button", "press"))
        self.assertEqual(call("input_button.b", "2026-01-01T00:00:00", "press")[:2], ("input_button", "press"))
        self.assertEqual(msg("script.s", "on")["txt"], "Running")
        with self.assertRaises(E.ActionError):
            call("scene.s", "x", "toggle")

    def test_sensors(self):
        self.assertEqual(msg("binary_sensor.s", "on", device_class="smoke")["lvl"], E.LEVEL_BAD)
        self.assertEqual(msg("binary_sensor.s", "off", device_class="smoke")["txt"], "OK")
        self.assertEqual(msg("sensor.b", "15", device_class="battery", unit_of_measurement="%")["lvl"], E.LEVEL_WARN)
        self.assertEqual(msg("sensor.b", "5", device_class="battery")["lvl"], E.LEVEL_BAD)
        self.assertEqual(msg("sensor.b", "80", device_class="battery")["lvl"], E.LEVEL_GOOD)
        self.assertEqual(msg("person.p", "not_home")["val"], "Away")
        self.assertEqual(msg("device_tracker.p", "work")["val"], "Work")
        w = msg("weather.w", "partlycloudy", temperature=12.34, temperature_unit="°C")
        self.assertEqual((w["val"], w["unit"], w["txt"]), ("12.3", "°C", "Partly cloudy"))
        self.assertEqual(msg("input_select.mode", "A very long option name")["val"], "A very long")
        self.assertEqual(msg("counter.c", "3")["val"], "3")
        self.assertEqual(msg("sensor.p", "1.23456", suggested_display_precision=2)["val"], "1.23")
        self.assertEqual(msg("update.u", "on")["lvl"], E.LEVEL_WARN)
        self.assertEqual(msg("sun.sun", "above_horizon")["val"], "Up")
        with self.assertRaises(E.ActionError):
            call("sensor.t", "20", "toggle")

    def test_climate_modes_fit_the_display(self):
        m = msg("climate.c", "cool", hvac_modes=["off", "heat", "cool", "auto", "dry", "fan_only"])
        self.assertEqual(m["modes"], ["off", "heat", "cool", "auto"])


class MockHouseTests(unittest.IsolatedAsyncioTestCase):
    async def test_new_domains(self):
        b = MockBackend(tick=0, seed=1)
        for entity_id in ("cover.living_room_blind", "lock.front_door", "fan.bedroom"):
            e = b.get(entity_id)
            await b.call_service(*E.action_to_service(entity_id, e, "toggle"))
        self.assertEqual(b.get("cover.living_room_blind").state, "closed")
        self.assertEqual(b.get("lock.front_door").state, "unlocked")
        self.assertEqual(b.get("fan.bedroom").state, "on")
        await b.call_service(*E.action_to_service("fan.bedroom", b.get("fan.bedroom"), "bri", 40))
        self.assertEqual(b.get("fan.bedroom").attributes["percentage"], 40)
        await b.call_service(*E.action_to_service("scene.movie_night", b.get("scene.movie_night"), "press"))

    def test_every_mock_entity_renders(self):
        b = MockBackend(tick=0, seed=1)
        for e in b.states().values():
            self.assertIsNotNone(E.tile_def(e.entity_id, e, {}), e.entity_id)
            self.assertTrue(E.state_msg(e.entity_id, e)["ok"], e.entity_id)


if __name__ == "__main__":
    unittest.main()
