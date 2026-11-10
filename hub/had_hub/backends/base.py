"""Where entity states come from and where service calls go.

A backend mirrors Home Assistant: it knows a set of entities (entity_id,
state, attributes), tells listeners when one changes, and carries out
service calls (domain, service, data). The mock backend fakes a house; the
Home Assistant backend holds a WebSocket to HA and forwards both ways.
"""
from __future__ import annotations

import logging
from typing import Callable

from ..entities import EntityState

log = logging.getLogger(__name__)

Listener = Callable[[EntityState], None]


class Backend:
    name = "base"

    def __init__(self) -> None:
        self._states: dict[str, EntityState] = {}
        self._listeners: list[Listener] = []

    async def start(self) -> None:
        pass

    async def stop(self) -> None:
        pass

    def states(self) -> dict[str, EntityState]:
        return self._states

    def get(self, entity_id: str) -> EntityState | None:
        return self._states.get(entity_id)

    def status(self) -> dict:
        """For the admin page: whether the backend can reach its house."""
        return {"connected": True, "detail": ""}

    def subscribe(self, listener: Listener) -> None:
        self._listeners.append(listener)

    async def call_service(self, domain: str, service: str, data: dict) -> None:
        raise NotImplementedError

    # For subclasses: record a state and tell listeners if it changed.
    def _set(self, entity_id: str, state: str, attributes: dict) -> None:
        old = self._states.get(entity_id)
        if old and old.state == state and old.attributes == attributes:
            return
        new = EntityState(entity_id, state, dict(attributes))
        self._states[entity_id] = new
        for listener in self._listeners:
            try:
                listener(new)
            except Exception:
                log.exception("state listener failed")
