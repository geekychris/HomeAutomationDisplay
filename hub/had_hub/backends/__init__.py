from .base import Backend
from .homeassistant import HomeAssistantBackend
from .mock import MockBackend

BACKENDS = {"mock": MockBackend, "homeassistant": HomeAssistantBackend}

__all__ = ["Backend", "HomeAssistantBackend", "MockBackend", "BACKENDS"]
