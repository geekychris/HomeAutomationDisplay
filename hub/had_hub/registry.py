"""Which entities go on which display.

Each display has an id (from its MAC, e.g. "cyd-a1b2c3"), a name you give
it, and pages of tiles. A tile names an entity and may override its label
and, for sensors, the warn/bad thresholds. Displays that connect for the
first time are added with no pages, so they show up in the admin page ready
to be set up.

The registry also keeps the hub's scenes (scenes.py): a library shared by
all displays, and each display's own list of them for its scene strip.

Kept in one JSON file, written whole on every change.
"""
from __future__ import annotations

import json
import os
import re
import secrets
import tempfile
import time
from pathlib import Path

from . import entities as E
from . import scenes as S

DISPLAY_ID = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
ENTITY_ID = re.compile(r"^[a-z0-9_]+\.[a-z0-9_]+$")
SCENE_ID = re.compile(r"^s[0-9a-f]{6}$")


class ConfigError(ValueError):
    pass


def validate_layout(cfg: dict) -> dict:
    """Check a display's config (name and pages) from the admin page and
    return a clean copy. Raises ConfigError with a message for people."""
    if not isinstance(cfg, dict):
        raise ConfigError("config must be an object")
    name = cfg.get("name", "")
    if not isinstance(name, str):
        raise ConfigError("name must be text")
    pages = cfg.get("pages", [])
    if not isinstance(pages, list):
        raise ConfigError("pages must be a list")
    if len(pages) > E.MAX_PAGES:
        raise ConfigError(f"a display holds at most {E.MAX_PAGES} pages")
    out_pages, total = [], 0
    for i, p in enumerate(pages):
        if not isinstance(p, dict) or not isinstance(p.get("tiles", []), list):
            raise ConfigError(f"page {i + 1} is not a page")
        pname = str(p.get("name") or f"Page {i + 1}")
        if len(pname.encode()) > E.MAX_PAGE_NAME:
            raise ConfigError(f"page name '{pname}' is longer than {E.MAX_PAGE_NAME} characters")
        tiles = []
        for t in p.get("tiles", []):
            if isinstance(t, str):
                t = {"entity": t}
            entity = t.get("entity") if isinstance(t, dict) else None
            if not isinstance(entity, str) or not ENTITY_ID.match(entity):
                raise ConfigError(f"'{entity}' is not an entity id")
            if len(entity) > E.MAX_ENTITY_ID:
                raise ConfigError(f"{entity} is longer than {E.MAX_ENTITY_ID} characters")
            if not E.tile_kind(entity):
                raise ConfigError(f"{entity}: displays cannot show {entity.split('.')[0]} entities yet")
            clean = {"entity": entity}
            if t.get("name"):
                clean["name"] = str(t["name"])
            for k in ("warn", "bad"):
                if t.get(k) not in (None, ""):
                    try:
                        clean[k] = float(t[k])
                    except (TypeError, ValueError):
                        raise ConfigError(f"{entity}: {k} must be a number") from None
            tiles.append(clean)
        if len(tiles) > E.MAX_TILES_PER_PAGE:
            raise ConfigError(f"page '{pname}' has {len(tiles)} tiles; the most is {E.MAX_TILES_PER_PAGE}")
        total += len(tiles)
        out_pages.append({"name": pname, "tiles": tiles})
    if total > E.MAX_TILES:
        raise ConfigError(f"{total} tiles in all; a display holds at most {E.MAX_TILES}")
    scenes = cfg.get("scenes", [])
    if not isinstance(scenes, list) or not all(isinstance(x, str) and SCENE_ID.match(x) for x in scenes):
        raise ConfigError("scenes must be a list of scene ids")
    scenes = list(dict.fromkeys(scenes))            # no repeats, order kept
    if len(scenes) > S.MAX_DISPLAY_SCENES:
        raise ConfigError(f"a display's scene strip holds at most {S.MAX_DISPLAY_SCENES} scenes")
    return {"name": name.strip(), "pages": out_pages, "scenes": scenes}


def validate_scene(cfg: dict) -> dict:
    """Check a scene from the admin page and return a clean copy."""
    if not isinstance(cfg, dict):
        raise ConfigError("a scene must be an object")
    name = str(cfg.get("name") or "").strip()
    if not name:
        raise ConfigError("give the scene a name")
    if len(name.encode()) > S.MAX_SCENE_NAME:
        raise ConfigError(f"scene name '{name}' is longer than {S.MAX_SCENE_NAME} characters")
    ents = cfg.get("entities") or {}
    if not isinstance(ents, dict):
        raise ConfigError("entities must be an object")
    if len(ents) > S.MAX_SCENE_ENTITIES:
        raise ConfigError(f"a scene holds at most {S.MAX_SCENE_ENTITIES} entities")
    clean = {}
    for eid, values in ents.items():
        if not isinstance(eid, str) or not ENTITY_ID.match(eid):
            raise ConfigError(f"'{eid}' is not an entity id")
        if not S.can_hold(eid):
            raise ConfigError(f"{eid}: scenes can hold lights, fans, covers, switches, locks and thermostats")
        try:
            clean[eid] = S.clean_values(eid, values)
        except S.SceneError as err:
            raise ConfigError(str(err)) from None
    return {"name": name, "entities": clean, "locked": bool(cfg.get("locked"))}


class Registry:
    def __init__(self, path: str | Path | None) -> None:
        self.path = Path(path) if path else None
        self._displays: dict[str, dict] = {}
        self._scenes: dict[str, dict] = {}
        if self.path and self.path.exists():
            data = json.loads(self.path.read_text())
            self._displays = data.get("displays", {})
            self._scenes = data.get("scenes", {})

    def displays(self) -> dict[str, dict]:
        return self._displays

    def get(self, display_id: str) -> dict | None:
        return self._displays.get(display_id)

    def seen(self, display_id: str, hello: dict) -> dict:
        """A display said hello: add it if new, and note what it told us."""
        if not DISPLAY_ID.match(display_id):
            raise ConfigError(f"bad display id {display_id!r}")
        d = self._displays.setdefault(display_id, {"name": "", "pages": [], "scenes": []})
        d["last_seen"] = time.time()
        d["info"] = {k: hello[k] for k in ("name", "fw", "ip", "w", "h") if k in hello}
        self.save()
        return d

    def set_layout(self, display_id: str, cfg: dict) -> dict:
        if not DISPLAY_ID.match(display_id):
            raise ConfigError(f"bad display id {display_id!r}")
        clean = validate_layout(cfg)
        if "scenes" not in cfg:             # leave the strip alone if the caller did not mention it
            del clean["scenes"]
        missing = [x for x in clean.get("scenes", []) if x not in self._scenes]
        if missing:
            raise ConfigError(f"no scene {missing[0]}")
        d = self._displays.setdefault(display_id, {})
        d.update(clean)
        self.save()
        return d

    def delete(self, display_id: str) -> bool:
        if self._displays.pop(display_id, None) is None:
            return False
        self.save()
        return True

    def tiles(self, display_id: str) -> list[dict]:
        d = self._displays.get(display_id) or {}
        return [t for p in d.get("pages", []) for t in p.get("tiles", [])]

    # ------------------------------------------------------------ scenes

    def scenes(self) -> dict[str, dict]:
        return self._scenes

    def scene(self, scene_id: str) -> dict | None:
        return self._scenes.get(scene_id)

    def display_scenes(self, display_id: str) -> list[tuple[str, dict]]:
        """The display's strip, in order: (id, scene)."""
        d = self._displays.get(display_id) or {}
        return [(x, self._scenes[x]) for x in d.get("scenes", []) if x in self._scenes]

    def add_scene(self, cfg: dict) -> str:
        if len(self._scenes) >= S.MAX_SCENES:
            raise ConfigError(f"the hub holds at most {S.MAX_SCENES} scenes")
        clean = validate_scene(cfg)
        scene_id = "s" + secrets.token_hex(3)
        while scene_id in self._scenes:
            scene_id = "s" + secrets.token_hex(3)
        self._scenes[scene_id] = clean
        self.save()
        return scene_id

    def set_scene(self, scene_id: str, cfg: dict) -> dict:
        if scene_id not in self._scenes:
            raise ConfigError(f"no scene {scene_id}")
        self._scenes[scene_id] = validate_scene(cfg)
        self.save()
        return self._scenes[scene_id]

    def set_scene_values(self, scene_id: str, entities: dict) -> None:
        """New values for a scene's entities (a capture), name and lock kept."""
        sc = self._scenes[scene_id]
        self.set_scene(scene_id, {**sc, "entities": entities})

    def delete_scene(self, scene_id: str) -> list[str]:
        """Remove a scene, and from every display's strip. Returns the
        displays that had it."""
        if self._scenes.pop(scene_id, None) is None:
            return []
        had = []
        for display_id, d in self._displays.items():
            if scene_id in d.get("scenes", []):
                d["scenes"] = [x for x in d["scenes"] if x != scene_id]
                had.append(display_id)
        self.save()
        return had

    def save(self) -> None:
        if not self.path:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=self.path.parent, prefix=".displays-")
        with os.fdopen(fd, "w") as f:
            json.dump({"displays": self._displays, "scenes": self._scenes}, f, indent=2, ensure_ascii=False)
        os.replace(tmp, self.path)
