"""A pretend house, for building and testing without Home Assistant.

It has at least one entity of every tile kind: lights, a fan, covers, a
lock, switches, an automation, a scene and a script, sensors of several
sorts, a person, the weather and a heater.

The entities look exactly as Home Assistant reports them (same domains,
attributes and services), so whatever works against the mock works against
the real thing. Sensors drift, and the heater warms the living room when
it runs and the room cools towards the outdoor temperature when it does not.
"""
from __future__ import annotations

import asyncio
import copy
import logging
import random

from .base import Backend

log = logging.getLogger(__name__)

DIMMABLE = {"supported_color_modes": ["brightness"], "color_mode": "brightness"}
ONOFF = {"supported_color_modes": ["onoff"], "color_mode": "onoff"}

HOUSE = {
    "light.living_room": ("on", {"friendly_name": "Living room", "brightness": 180, **DIMMABLE}),
    "light.kitchen": ("off", {"friendly_name": "Kitchen", **DIMMABLE}),
    "light.bedroom": ("off", {"friendly_name": "Bedroom", **DIMMABLE}),
    "light.porch": ("off", {"friendly_name": "Porch", **ONOFF}),
    "switch.coffee_maker": ("off", {"friendly_name": "Coffee maker"}),
    "switch.garden_pump": ("off", {"friendly_name": "Garden pump"}),
    "input_boolean.guest_mode": ("off", {"friendly_name": "Guest mode"}),
    "sensor.living_room_temperature": ("19.4", {
        "friendly_name": "Living room temp", "device_class": "temperature",
        "unit_of_measurement": "°C", "state_class": "measurement"}),
    "sensor.living_room_humidity": ("46", {
        "friendly_name": "Living room humidity", "device_class": "humidity",
        "unit_of_measurement": "%", "state_class": "measurement"}),
    "sensor.living_room_co2": ("780", {
        "friendly_name": "Living room CO2", "device_class": "carbon_dioxide",
        "unit_of_measurement": "ppm", "state_class": "measurement"}),
    "sensor.living_room_voc": ("140", {
        "friendly_name": "Living room VOC", "device_class": "volatile_organic_compounds_parts",
        "unit_of_measurement": "ppb", "state_class": "measurement"}),
    "sensor.outdoor_temperature": ("8.2", {
        "friendly_name": "Outside", "device_class": "temperature",
        "unit_of_measurement": "°C", "state_class": "measurement"}),
    "binary_sensor.front_door": ("off", {"friendly_name": "Front door", "device_class": "door"}),
    "fan.bedroom": ("off", {"friendly_name": "Bedroom fan", "supported_features": 1, "percentage": 0}),
    "cover.living_room_blind": ("open", {
        "friendly_name": "Living room blind", "device_class": "blind",
        "supported_features": 15, "current_position": 60}),
    "cover.garage_door": ("closed", {"friendly_name": "Garage door", "device_class": "garage", "supported_features": 3}),
    "lock.front_door": ("locked", {"friendly_name": "Front door lock", "supported_features": 0}),
    "automation.porch_light_at_dusk": ("on", {"friendly_name": "Porch light at dusk"}),
    "scene.movie_night": ("2026-01-01T18:00:00+00:00", {"friendly_name": "Movie night"}),
    "script.good_night": ("off", {"friendly_name": "Good night"}),
    "binary_sensor.kitchen_smoke": ("off", {"friendly_name": "Kitchen smoke", "device_class": "smoke"}),
    "sensor.door_lock_battery": ("64", {
        "friendly_name": "Lock battery", "device_class": "battery", "unit_of_measurement": "%"}),
    "person.alex": ("home", {"friendly_name": "Alex"}),
    "weather.home": ("partlycloudy", {"friendly_name": "Weather", "temperature": 8.2, "temperature_unit": "°C"}),
    "climate.house_heater": ("heat", {
        "friendly_name": "Heating", "hvac_modes": ["off", "heat"], "hvac_action": "idle",
        "current_temperature": 19.4, "temperature": 20.5, "min_temp": 7, "max_temp": 30,
        "target_temp_step": 0.5, "temperature_unit": "°C", "supported_features": 1}),
}

HEATER = "climate.house_heater"
ROOM_TEMP = "sensor.living_room_temperature"
OUTSIDE = "sensor.outdoor_temperature"


class MockBackend(Backend):
    name = "mock"

    def __init__(self, tick: float = 3.0, seed: int | None = None) -> None:
        super().__init__()
        self._tick = tick
        self._rand = random.Random(seed)
        self._task: asyncio.Task | None = None
        for entity_id, (state, attrs) in copy.deepcopy(HOUSE).items():
            self._set(entity_id, state, attrs)

    async def start(self) -> None:
        if self._tick > 0:
            self._task = asyncio.create_task(self._run())

    async def stop(self) -> None:
        if self._task:
            self._task.cancel()

    # ------------------------------------------------------------ services

    async def call_service(self, domain: str, service: str, data: dict) -> None:
        entity_id = data.get("entity_id")
        e = self.get(entity_id)
        if not e or e.domain != domain:
            raise ValueError(f"no entity {entity_id} in domain {domain}")
        log.info("service %s.%s %s", domain, service, data)
        attrs = dict(e.attributes)
        state = e.state
        if service == "toggle" and domain != "cover":
            service = "turn_off" if state == "on" else "turn_on"
        if domain == "fan" and service == "set_percentage":
            state, service = "on", "turn_on"
            attrs["percentage"] = int(data["percentage"])
        if domain == "cover":
            pos = {"open_cover": 100, "close_cover": 0,
                   "toggle": 0 if state in ("open", "opening") else 100}.get(service)
            if service == "set_cover_position":
                pos = int(data["position"])
            if pos is None:
                raise ValueError(f"cover has no service {service}")
            state = "open" if pos > 0 else "closed"
            if "current_position" in attrs:
                attrs["current_position"] = pos
        elif domain == "lock":
            if service not in ("lock", "unlock"):
                raise ValueError(f"lock has no service {service}")
            state = service + "ed"
        elif domain in ("scene", "script", "button", "input_button"):
            if service not in ("turn_on", "press"):
                raise ValueError(f"{domain} has no service {service}")
            log.info("mock: ran %s", entity_id)
            if domain != "script":
                return
            # A script runs for a moment, then stops.
            self._set(entity_id, "on", attrs)
            asyncio.get_running_loop().call_later(2, lambda: self._set(entity_id, "off", attrs))
            return
        elif domain in ("light", "switch", "input_boolean", "fan", "automation"):
            if service == "turn_off":
                state = "off"
                attrs.pop("brightness", None)
                if domain == "fan":
                    attrs["percentage"] = 0
            elif service == "turn_on":
                state = "on"
                if domain == "fan" and not attrs.get("percentage"):
                    attrs["percentage"] = 100
                if domain == "light" and "brightness" in attrs.get("supported_color_modes", []):
                    if "brightness_pct" in data:
                        attrs["brightness"] = round(data["brightness_pct"] * 255 / 100)
                    elif "brightness" in data:
                        attrs["brightness"] = int(data["brightness"])
                    else:
                        attrs.setdefault("brightness", 255)
            else:
                raise ValueError(f"{domain} has no service {service}")
        elif domain == "climate":
            if service == "set_temperature":
                attrs["temperature"] = float(data["temperature"])
            elif service == "set_hvac_mode":
                if data["hvac_mode"] not in attrs["hvac_modes"]:
                    raise ValueError(f"unsupported mode {data['hvac_mode']}")
                state = data["hvac_mode"]
            else:
                raise ValueError(f"climate has no service {service}")
            attrs["hvac_action"] = self._heater_action(state, attrs)
        else:
            raise ValueError(f"cannot call {domain}.{service}")
        self._set(entity_id, state, attrs)

    def set_state(self, entity_id: str, state: str, **attrs) -> None:
        """Change an entity directly, as if something outside the hub did it
        (a wall switch, a sensor reading). For tests and the admin page."""
        e = self.get(entity_id)
        self._set(entity_id, state, {**(e.attributes if e else {}), **attrs})

    # ------------------------------------------------------------ simulation

    @staticmethod
    def _heater_action(mode: str, attrs: dict) -> str:
        if mode == "off":
            return "off"
        cur, tgt = attrs.get("current_temperature"), attrs.get("temperature")
        if cur is None or tgt is None:
            return "idle"
        # Hysteresis, like a real thermostat: on below target - 0.3, off at target.
        if attrs.get("hvac_action") == "heating":
            return "heating" if cur < tgt else "idle"
        return "heating" if cur < tgt - 0.3 else "idle"

    def _drift(self, entity_id: str, step: float, lo: float, hi: float, digits: int) -> None:
        e = self.get(entity_id)
        v = float(e.state) + self._rand.uniform(-step, step)
        v = max(lo, min(hi, v))
        self._set(entity_id, f"{v:.{digits}f}" if digits else str(int(round(v))), e.attributes)

    def step(self) -> None:
        """One tick of the simulated house."""
        heater = self.get(HEATER)
        outside = float(self.get(OUTSIDE).state)
        room = float(self.get(ROOM_TEMP).state)
        if heater.attributes.get("hvac_action") == "heating":
            room += 0.15
        else:
            room += (outside - room) * 0.01
        room += self._rand.uniform(-0.03, 0.03)
        self._set(ROOM_TEMP, f"{room:.1f}", self.get(ROOM_TEMP).attributes)
        attrs = dict(heater.attributes, current_temperature=round(room, 1))
        attrs["hvac_action"] = self._heater_action(heater.state, attrs)
        self._set(HEATER, heater.state, attrs)

        self._drift(OUTSIDE, 0.1, -5, 25, 1)
        self._drift("sensor.living_room_humidity", 1, 30, 70, 0)
        # CO2 creeps up while people are in, as it does in a closed room.
        co2 = self.get("sensor.living_room_co2")
        v = float(co2.state) + self._rand.uniform(-25, 35)
        if v > 1700:
            v = 650           # someone opened a window
        self._set("sensor.living_room_co2", str(int(max(400, v))), co2.attributes)
        self._drift("sensor.living_room_voc", 25, 20, 900, 0)

    async def _run(self) -> None:
        while True:
            await asyncio.sleep(self._tick)
            try:
                self.step()
            except Exception:
                log.exception("mock simulation step failed")
