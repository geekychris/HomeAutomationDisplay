# Shortcuts. Everything here is also a script in scripts/ or a plain command.
.PHONY: firmware flash monitor hub hub-ha test addon icons clean

firmware:            ## build the firmware and dist/home-automation-display-<ver>.bin
	scripts/build-firmware.sh

flash:               ## build and flash over USB (PORT=/dev/cu.usbserial-110 to pick one)
	scripts/flash.sh $(PORT)

monitor:             ## the display's serial log
	pio device monitor -e cyd

hub/.venv:
	python3 -m venv hub/.venv && hub/.venv/bin/pip install -q -r hub/requirements-dev.txt

hub: hub/.venv       ## run the hub with the mock house: http://localhost:8390/
	cd hub && .venv/bin/python -m had_hub

hub-ha: hub/.venv    ## run the hub against Home Assistant (HA_URL and HA_TOKEN set)
	cd hub && .venv/bin/python -m had_hub --backend homeassistant

test: hub/.venv      ## hub tests and a firmware build
	scripts/test.sh

addon:               ## build the add-on image locally, as the Supervisor would on a 64-bit Pi
	docker build --build-arg BUILD_FROM=ghcr.io/home-assistant/aarch64-base-python:3.13-alpine3.22 -t had-hub-addon hub

icons:               ## redraw the add-on's icon.png and logo.png
	python3 scripts/make_icons.py

clean:
	rm -rf .pio dist hub/.pytest_cache

help:
	@grep -E '^[a-z-]+:.*##' $(MAKEFILE_LIST) | sed 's/:.*##/\t/'
