"""Home Assistant entities in, display tiles out - and display actions back
into Home Assistant service calls.

Everything here works on Home Assistant's own shapes (an entity_id, a state
string and an attributes dict; a service call is domain, service and data),
so the mock backend and the Home Assistant backend share it unchanged.

A tile is one card on a display. The firmware draws five kinds:

    light    on/off and, if `dim`, a 0-100 level slider: lights
             (brightness), fans (speed) and covers (position)
    switch   on/off: switches, input booleans, automations, sirens,
             humidifiers, locks
    sensor   a value, a unit and a line under it, coloured by level:
             sensors, binary sensors, numbers, selects, people, weather...
    climate  current and target temperature, and the mode
    button   tap to run: scenes, scripts, buttons

Many Home Assistant domains share a kind: the hub decides what the on/off,
the level and the text mean for each domain, and the display just draws
them. docs/entities.md has the full table; adding a domain is described in
docs/extending.md.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime

# What the firmware can hold. The hub enforces these so a display is never
# sent more than it has room for.
MAX_PAGES = 6
MAX_TILES_PER_PAGE = 8
MAX_TILES = 24
MAX_ENTITY_ID = 63      # bytes
MAX_NAME = 23           # bytes
MAX_PAGE_NAME = 15      # bytes
MAX_VAL = 11            # bytes: sensor value
MAX_UNIT = 7            # bytes
MAX_TXT = 15            # bytes: the status line

KIND_BY_DOMAIN = {
    "light": "light",
    "fan": "light",
    "cover": "light",
    "switch": "switch",
    "input_boolean": "switch",
    "automation": "switch",
    "siren": "switch",
    "humidifier": "switch",
    "lock": "switch",
    "sensor": "sensor",
    "binary_sensor": "sensor",
    "input_number": "sensor",
    "number": "sensor",
    "counter": "sensor",
    "input_select": "sensor",
    "select": "sensor",
    "input_text": "sensor",
    "text": "sensor",
    "input_datetime": "sensor",
    "person": "sensor",
    "device_tracker": "sensor",
    "weather": "sensor",
    "sun": "sensor",
    "update": "sensor",
    "climate": "climate",
    "scene": "button",
    "script": "button",
    "button": "button",
    "input_button": "button",
}

# Icon hints for the tile's corner. The display falls back to a power symbol
# for anything it does not know.
ICON_BY_DOMAIN = {"fan": "fan", "cover": "cover", "siren": "bell", "lock": "lock",
                  "scene": "play", "script": "play", "button": "play", "input_button": "play"}

# Sensor levels: (warn, bad) by device class. Normally higher is worse; when
# warn > bad, lower is worse (battery). A tile can set its own "warn"/"bad".
DEFAULT_LEVELS = {
    "carbon_dioxide": (1000, 1500),                  # ppm
    "carbon_monoxide": (9, 35),                      # ppm
    "volatile_organic_compounds_parts": (220, 660),  # ppb
    "volatile_organic_compounds": (300, 1000),       # ug/m3
    "pm25": (12, 35),                                # ug/m3
    "pm10": (54, 154),                               # ug/m3
    "aqi": (51, 151),
    "battery": (20, 10),                             # %: low is bad
}
# Level sent to the display: 0 no judgement, 1 good, 2 warn, 3 bad.
LEVEL_NONE, LEVEL_GOOD, LEVEL_WARN, LEVEL_BAD = 0, 1, 2, 3

DEFAULT_PRECISION = {"temperature": 1, "humidity": 0, "carbon_dioxide": 0, "carbon_monoxide": 0,
                     "volatile_organic_compounds_parts": 0, "volatile_organic_compounds": 0,
                     "pm25": 0, "pm10": 0, "aqi": 0, "battery": 0, "power": 0, "energy": 1,
                     "illuminance": 0, "pressure": 0, "signal_strength": 0}

# binary_sensor device class -> (word when on, word when off)
BINARY_WORDS = {
    "door": ("Open", "Closed"), "window": ("Open", "Closed"), "opening": ("Open", "Closed"),
    "garage_door": ("Open", "Closed"), "motion": ("Motion", "Clear"),
    "occupancy": ("Occupied", "Clear"), "presence": ("Home", "Away"),
    "moisture": ("Wet", "Dry"), "smoke": ("Smoke!", "Clear"), "gas": ("Gas!", "Clear"),
    "carbon_monoxide": ("CO!", "Clear"), "lock": ("Unlocked", "Locked"),
    "battery": ("Low", "Normal"), "battery_charging": ("Charging", "Not charging"),
    "connectivity": ("Connected", "Offline"), "plug": ("Plugged in", "Unplugged"),
    "power": ("Power", "No power"), "problem": ("Problem", "OK"), "safety": ("Unsafe", "Safe"),
    "tamper": ("Tampered", "OK"), "heat": ("Hot", "Normal"), "cold": ("Cold", "Normal"),
    "light": ("Light", "Dark"), "running": ("Running", "Stopped"), "sound": ("Sound", "Quiet"),
    "vibration": ("Vibration", "Still"), "update": ("Update", "Up to date"),
}
# binary_sensor classes where "on" is an alarm: coloured bad, "Alert" under it.
BINARY_ALARMS = {"smoke", "gas", "carbon_monoxide", "moisture", "problem", "safety", "tamper",
                 "heat", "cold"}

WEATHER_WORDS = {
    "clear-night": "Clear", "cloudy": "Cloudy", "exceptional": "Exceptional", "fog": "Fog",
    "hail": "Hail", "lightning": "Lightning", "lightning-rainy": "Storms", "partlycloudy": "Partly cloudy",
    "pouring": "Pouring", "rainy": "Rain", "snowy": "Snow", "snowy-rainy": "Sleet",
    "sunny": "Sunny", "windy": "Windy", "windy-variant": "Windy",
}
COVER_WORDS = {"open": "Open", "closed": "Closed", "opening": "Opening", "closing": "Closing"}
LOCK_WORDS = {"locked": "Locked", "unlocked": "Unlocked", "locking": "Locking", "unlocking": "Unlocking",
              "jammed": "Jammed!", "open": "Open", "opening": "Opening"}
CLIMATE_MODES = ("off", "heat", "cool", "auto", "heat_cool", "dry", "fan_only")

# supported_features bits
FAN_SET_SPEED = 1
COVER_SET_POSITION = 4

# The attributes the hub reads. Backends keep only these, which keeps memory
# down and means a change to anything else (media position, say) does not
# wake every display.
USED_ATTRIBUTES = {
    "friendly_name", "device_class", "unit_of_measurement", "suggested_display_precision",
    "supported_color_modes", "color_mode", "brightness", "supported_features", "percentage",
    "current_position", "hvac_modes", "hvac_action", "current_temperature", "temperature",
    "min_temp", "max_temp", "target_temp_step", "temperature_unit",
    # From HA's registries rather than the state (backends/homeassistant.py).
    "area", "device", "device_id", "entity_category",
}


class ActionError(ValueError):
    """A display asked for something the entity cannot do."""


@dataclass
class EntityState:
    entity_id: str
    state: str
    attributes: dict = field(default_factory=dict)

    @property
    def domain(self) -> str:
        return self.entity_id.split(".", 1)[0]

    @property
    def name(self) -> str:
        return self.attributes.get("friendly_name") or self.entity_id

    @property
    def available(self) -> bool:
        return self.state not in ("unavailable", "unknown")

    def as_dict(self) -> dict:
        return {"entity_id": self.entity_id, "state": self.state, "attributes": self.attributes,
                "kind": tile_kind(self.entity_id), "domain": self.domain}


def clip(text: str, limit: int) -> str:
    """Cut to at most `limit` UTF-8 bytes without splitting a character."""
    data = text.encode()
    if len(data) <= limit:
        return text
    return data[:limit].decode(errors="ignore")


def tile_kind(entity_id: str) -> str | None:
    return KIND_BY_DOMAIN.get(entity_id.split(".", 1)[0])


def _features(e: EntityState) -> int:
    try:
        return int(e.attributes.get("supported_features") or 0)
    except (TypeError, ValueError):
        return 0


def has_level(e: EntityState) -> bool:
    """Whether a light-kind tile gets a slider."""
    if e.domain == "light":
        modes = e.attributes.get("supported_color_modes") or []
        return any(m != "onoff" for m in modes) or "brightness" in e.attributes
    if e.domain == "fan":
        return bool(_features(e) & FAN_SET_SPEED)
    if e.domain == "cover":
        return bool(_features(e) & COVER_SET_POSITION)
    return False


def tile_def(entity_id: str, e: EntityState | None, cfg: dict) -> dict | None:
    """The tile as it appears in a layout message. None if no tile kind fits.
    `e` is None when the backend does not (yet) know the entity: the tile is
    still sent, and shows as unavailable."""
    kind = tile_kind(entity_id)
    if not kind:
        return None
    name = cfg.get("name") or (e.name if e else entity_id)
    t = {"e": entity_id, "k": kind, "n": clip(name, MAX_NAME)}
    if kind == "light":
        t["dim"] = bool(e and has_level(e))
    icon = ICON_BY_DOMAIN.get(entity_id.split(".", 1)[0])
    if icon:
        t["ic"] = icon
    return t


def _number(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _format(value: float, precision: int) -> str:
    return f"{value:.{precision}f}" if precision > 0 else str(int(round(value)))


def _level(value: float | None, warn, bad) -> int:
    if value is None or (warn is None and bad is None):
        return LEVEL_NONE
    if warn is not None and bad is not None and warn > bad:      # lower is worse
        if value <= bad:
            return LEVEL_BAD
        return LEVEL_WARN if value <= warn else LEVEL_GOOD
    if bad is not None and value >= bad:
        return LEVEL_BAD
    if warn is not None and value >= warn:
        return LEVEL_WARN
    return LEVEL_GOOD


def sensor_level(e: EntityState, cfg: dict) -> int:
    warn, bad = DEFAULT_LEVELS.get(e.attributes.get("device_class"), (None, None))
    return _level(_number(e.state), cfg.get("warn", warn), cfg.get("bad", bad))


def _when(iso: str) -> str:
    """A timestamp sensor: the time if it is today, otherwise the date."""
    try:
        t = datetime.fromisoformat(iso.replace("Z", "+00:00")).astimezone()
    except ValueError:
        return clip(iso, MAX_VAL)
    return t.strftime("%H:%M") if t.date() == datetime.now().astimezone().date() else t.strftime("%d %b")


def _place(state: str) -> str:
    return {"home": "Home", "not_home": "Away"}.get(state, state.replace("_", " ").title())


def _sensor(e: EntityState, cfg: dict, m: dict) -> None:
    a, d = e.attributes, e.domain
    dc = a.get("device_class")
    m["unit"] = ""
    m["lvl"] = LEVEL_NONE
    if d == "binary_sensor":
        on = e.state == "on"
        words = BINARY_WORDS.get(dc, ("On", "Off"))
        m["val"] = clip(words[0] if on else words[1], MAX_VAL)
        if dc in BINARY_ALARMS:
            m["lvl"], m["txt"] = (LEVEL_BAD, "Alert") if on else (LEVEL_GOOD, "OK")
        elif dc == "battery":
            m["lvl"] = LEVEL_WARN if on else LEVEL_GOOD
    elif d == "update":
        m["val"], m["lvl"] = ("Update", LEVEL_WARN) if e.state == "on" else ("Current", LEVEL_GOOD)
    elif d in ("person", "device_tracker"):
        m["val"] = clip(_place(e.state), MAX_VAL)
    elif d == "sun":
        m["val"] = "Up" if e.state == "above_horizon" else "Down"
    elif d == "weather":
        t = _number(a.get("temperature"))
        m["val"] = _format(t, 1) if t is not None else "--"
        m["unit"] = clip(a.get("temperature_unit") or "", MAX_UNIT)
        m["txt"] = clip(WEATHER_WORDS.get(e.state, e.state.replace("-", " ").capitalize()), MAX_TXT)
    elif dc == "timestamp":
        m["val"] = _when(e.state)
    else:
        value = _number(e.state)
        if value is None:
            m["val"] = clip(e.state, MAX_VAL)
        else:
            prec = a.get("suggested_display_precision")
            if prec is None:
                prec = DEFAULT_PRECISION.get(dc, 0 if value == int(value) else 1)
            m["val"] = clip(_format(value, int(prec)), MAX_VAL)
        m["unit"] = clip(a.get("unit_of_measurement") or "", MAX_UNIT)
        m["lvl"] = sensor_level(e, cfg)


def state_msg(entity_id: str, e: EntityState | None, cfg: dict | None = None) -> dict:
    """The live state of one tile, as sent to a display."""
    cfg = cfg or {}
    kind = tile_kind(entity_id)
    m = {"t": "state", "e": entity_id, "ok": bool(e and e.available)}
    if not m["ok"]:
        return m
    a, d = e.attributes, e.domain
    if kind == "light":
        if d == "cover":
            m["on"] = e.state in ("open", "opening")
            pos = _number(a.get("current_position"))
            m["bri"] = int(pos) if pos is not None else (100 if m["on"] else 0)
            word = COVER_WORDS.get(e.state, e.state.capitalize())
            m["txt"] = f"{word} {int(pos)}%" if pos is not None and e.state == "open" and 0 < pos < 100 else word
        elif d == "fan":
            m["on"] = e.state == "on"
            pct = _number(a.get("percentage"))
            m["bri"] = int(pct) if (m["on"] and pct is not None) else (100 if m["on"] else 0)
        else:
            m["on"] = e.state == "on"
            b = _number(a.get("brightness"))
            m["bri"] = round(b * 100 / 255) if (m["on"] and b is not None) else (100 if m["on"] else 0)
    elif kind == "switch":
        if d == "lock":
            m["on"] = e.state == "locked"
            m["txt"] = LOCK_WORDS.get(e.state, e.state.capitalize())
        else:
            m["on"] = e.state == "on"
            if d == "automation":
                m["txt"] = "Enabled" if m["on"] else "Disabled"
    elif kind == "sensor":
        _sensor(e, cfg, m)
    elif kind == "climate":
        m["mode"] = e.state
        m["modes"] = [x for x in a.get("hvac_modes", []) if x in CLIMATE_MODES][:4]
        m["act"] = a.get("hvac_action") or ""
        m["cur"] = _number(a.get("current_temperature"))
        m["tgt"] = _number(a.get("temperature"))
        m["min"] = _number(a.get("min_temp")) or 7
        m["max"] = _number(a.get("max_temp")) or 35
        m["step"] = _number(a.get("target_temp_step")) or 0.5
        m["unit"] = a.get("temperature_unit") or "°C"
    elif kind == "button":
        m["on"] = d == "script" and e.state == "on"
        m["txt"] = {"scene": "Activate", "script": "Running" if m["on"] else "Run"}.get(d, "Press")
    return m


def action_to_service(entity_id: str, e: EntityState | None, action: str, value=None) -> tuple[str, str, dict]:
    """Map a display action to a Home Assistant service call:
    (domain, service, data).

    Actions: toggle, on, off (light, switch kinds); bri (0-100: light
    brightness, fan speed, cover position); target (a temperature) and mode
    (an hvac mode) for climate; press (button kind).
    """
    if e is None or not e.available:
        raise ActionError(f"{entity_id} is not available")
    kind = tile_kind(entity_id)
    domain = e.domain
    data = {"entity_id": entity_id}

    if action in ("toggle", "on", "off") and kind in ("light", "switch"):
        if domain == "lock":
            want_lock = (e.state != "locked") if action == "toggle" else action == "on"
            return domain, "lock" if want_lock else "unlock", data
        if domain == "cover":
            return domain, {"toggle": "toggle", "on": "open_cover", "off": "close_cover"}[action], data
        return domain, {"toggle": "toggle", "on": "turn_on", "off": "turn_off"}[action], data

    if action == "bri" and kind == "light":
        pct = _number(value)
        if pct is None:
            raise ActionError("the level must be a number")
        pct = max(0, min(100, int(round(pct))))
        if domain == "cover":
            if not has_level(e):
                raise ActionError(f"{entity_id} cannot be set to a position")
            return domain, "set_cover_position", {**data, "position": pct}
        if pct == 0:
            return domain, "turn_off", data
        if domain == "fan":
            return domain, "set_percentage", {**data, "percentage": pct}
        return domain, "turn_on", {**data, "brightness_pct": pct}

    if action == "target" and kind == "climate":
        t = _number(value)
        if t is None:
            raise ActionError("the target must be a number")
        lo = _number(e.attributes.get("min_temp")) or 7
        hi = _number(e.attributes.get("max_temp")) or 35
        return domain, "set_temperature", {**data, "temperature": max(lo, min(hi, t))}

    if action == "mode" and kind == "climate":
        if value not in e.attributes.get("hvac_modes", []):
            raise ActionError(f"{entity_id} has no mode {value!r}")
        return domain, "set_hvac_mode", {**data, "hvac_mode": value}

    if action == "press" and kind == "button":
        if domain in ("scene", "script"):
            return domain, "turn_on", data
        return domain, "press", data

    raise ActionError(f"{entity_id} cannot {action}")
