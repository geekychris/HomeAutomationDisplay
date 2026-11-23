"""Scenes kept by the hub.

A scene is a name and, for each entity in it, how that entity should be:

    {"on": true, "level": 40}          a light at 40%, a fan at 40% speed,
                                       a cover 40% open
    {"on": false}                      off, closed, unlocked
    {"mode": "heat", "target": 21.0}   a thermostat

`on` means what a tile's on means for that domain (open for a cover, locked
for a lock). Applying a scene turns each value into the same actions a
display sends when someone taps (entities.action_to_service), so any entity a
tile can switch, dim or set, a scene can too.

A scene "is on" when every entity in it is as the scene says, near enough:
levels within a few percent, temperatures within a quarter of a degree. The
displays show that, rather than the last scene someone tapped, so a scene
stops showing as active when someone changes a light by hand.

These scenes live in the hub only (registry.py saves them); Home Assistant
does not know about them.
"""
from __future__ import annotations

import asyncio
import logging

from . import entities as E

log = logging.getLogger(__name__)

MAX_SCENE_ENTITIES = 32
MAX_SCENES = 64             # in the hub
MAX_DISPLAY_SCENES = 6      # on one display's strip
MAX_SCENE_NAME = 15         # bytes, like a page name
LEVEL_SLACK = 3             # percent
TARGET_SLACK = 0.25         # degrees

# Kinds a scene can hold. Sensors and buttons have no state to set.
SCENE_KINDS = ("light", "switch", "climate")


class SceneError(ValueError):
    pass


def can_hold(entity_id: str) -> bool:
    return E.tile_kind(entity_id) in SCENE_KINDS


def capture(entity_id: str, e: E.EntityState | None) -> dict | None:
    """How the entity is now, as scene values. None if it is unavailable or
    a scene cannot hold it."""
    if not can_hold(entity_id) or e is None or not e.available:
        return None
    m = E.state_msg(entity_id, e)
    kind = E.tile_kind(entity_id)
    if kind == "climate":
        v = {"mode": m["mode"]}
        if m.get("tgt") is not None and m["mode"] != "off":
            v["target"] = m["tgt"]
        return v
    v = {"on": bool(m.get("on"))}
    if kind == "light" and E.has_level(e) and v["on"]:
        v["level"] = int(m.get("bri", 100))
    return v


def clean_values(entity_id: str, values) -> dict:
    """Check one entity's values from the admin page."""
    if not isinstance(values, dict):
        raise SceneError(f"{entity_id}: values must be an object")
    kind = E.tile_kind(entity_id)
    out = {}
    if kind == "climate":
        mode = values.get("mode")
        if not isinstance(mode, str) or mode not in E.CLIMATE_MODES:
            raise SceneError(f"{entity_id}: pick a mode")
        out["mode"] = mode
        if values.get("target") not in (None, "") and mode != "off":
            try:
                out["target"] = round(float(values["target"]) * 2) / 2
            except (TypeError, ValueError):
                raise SceneError(f"{entity_id}: the target must be a number") from None
        return out
    out["on"] = bool(values.get("on"))
    if kind == "light" and out["on"] and values.get("level") not in (None, ""):
        try:
            out["level"] = max(1, min(100, int(round(float(values["level"])))))
        except (TypeError, ValueError):
            raise SceneError(f"{entity_id}: the level must be a number") from None
    return out


def matches(entity_id: str, e: E.EntityState | None, values: dict) -> bool:
    """Whether the entity is as the scene says."""
    if e is None or not e.available:
        return False
    m = E.state_msg(entity_id, e)
    if E.tile_kind(entity_id) == "climate":
        if m["mode"] != values.get("mode"):
            return False
        t = values.get("target")
        return t is None or (m.get("tgt") is not None and abs(m["tgt"] - t) <= TARGET_SLACK)
    if bool(m.get("on")) != bool(values.get("on")):
        return False
    level = values.get("level")
    if level is None or not values.get("on") or not E.has_level(e):
        return True
    return abs(int(m.get("bri", 0)) - level) <= LEVEL_SLACK


def actions(entity_id: str, e: E.EntityState, values: dict) -> list[tuple[str, object]]:
    """The display actions that bring the entity to the scene's values."""
    if E.tile_kind(entity_id) == "climate":
        out = [("mode", values["mode"])] if e.state != values["mode"] else []
        if values.get("target") is not None and values["mode"] != "off":
            out.append(("target", values["target"]))
        return out
    if not values.get("on"):
        return [("off", None)]
    if values.get("level") is not None and E.has_level(e):
        return [("bri", values["level"])]
    return [("on", None)]


def is_on(scene: dict, states: dict[str, E.EntityState]) -> bool:
    ents = scene.get("entities") or {}
    return bool(ents) and all(matches(eid, states.get(eid), v) for eid, v in ents.items())


async def apply(scene: dict, states: dict[str, E.EntityState], act) -> list[str]:
    """Bring every entity in the scene to its values, skipping those already
    there. `act(entity_id, action, value)` makes one call (Hub.act). Entities
    go in parallel; one entity's actions in order (a thermostat's mode before
    its target). Returns a message per entity that failed."""
    async def one(eid: str, values: dict) -> str | None:
        e = states.get(eid)
        if e is None or not e.available:
            return f"{eid} is not available"
        if matches(eid, e, values):
            return None
        try:
            for action, value in actions(eid, e, values):
                await act(eid, action, value)
        except Exception as err:        # backend or mapping: report it, carry on with the rest
            log.warning("scene %s: %s failed: %s", scene.get("name"), eid, err)
            return f"{eid}: {err}"
        return None

    results = await asyncio.gather(*(one(eid, v) for eid, v in (scene.get("entities") or {}).items()))
    return [r for r in results if r]
