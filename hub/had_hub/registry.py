"""Which entities go on which display.

Each display has an id (from its MAC, e.g. "cyd-a1b2c3"), a name you give
it, and pages of tiles. A tile names an entity and may override its label
and, for sensors, the warn/bad thresholds. Displays that connect for the
first time are added with no pages, so they show up in the admin page ready
to be set up.

Kept in one JSON file, written whole on every change.
"""
from __future__ import annotations

import json
import os
import re
import tempfile
import time
from pathlib import Path

from . import entities as E

DISPLAY_ID = re.compile(r"^[A-Za-z0-9_-]{1,32}$")
ENTITY_ID = re.compile(r"^[a-z0-9_]+\.[a-z0-9_]+$")


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
    return {"name": name.strip(), "pages": out_pages}


class Registry:
    def __init__(self, path: str | Path | None) -> None:
        self.path = Path(path) if path else None
        self._displays: dict[str, dict] = {}
        if self.path and self.path.exists():
            self._displays = json.loads(self.path.read_text()).get("displays", {})

    def displays(self) -> dict[str, dict]:
        return self._displays

    def get(self, display_id: str) -> dict | None:
        return self._displays.get(display_id)

    def seen(self, display_id: str, hello: dict) -> dict:
        """A display said hello: add it if new, and note what it told us."""
        if not DISPLAY_ID.match(display_id):
            raise ConfigError(f"bad display id {display_id!r}")
        d = self._displays.setdefault(display_id, {"name": "", "pages": []})
        d["last_seen"] = time.time()
        d["info"] = {k: hello[k] for k in ("name", "fw", "ip", "w", "h") if k in hello}
        self.save()
        return d

    def set_layout(self, display_id: str, cfg: dict) -> dict:
        if not DISPLAY_ID.match(display_id):
            raise ConfigError(f"bad display id {display_id!r}")
        clean = validate_layout(cfg)
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

    def save(self) -> None:
        if not self.path:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=self.path.parent, prefix=".displays-")
        with os.fdopen(fd, "w") as f:
            json.dump({"displays": self._displays}, f, indent=2, ensure_ascii=False)
        os.replace(tmp, self.path)
