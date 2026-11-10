# Changelog

## 0.3.0

- Home Assistant backend: one WebSocket to HA (through the Supervisor as an
  add-on, or with a long-lived token anywhere else), reconnecting when HA
  restarts.
- Runs as a Home Assistant add-on, with the admin page in HA's sidebar.
- New entity types: fans, covers, locks, automations, sirens, humidifiers,
  scenes, scripts, buttons, numbers, selects, counters, people, device
  trackers, weather, sun and updates, plus more sensor and binary sensor
  classes (battery, CO, PM10, AQI, smoke, gas, problem...).
- Admin page: areas from HA, a filter for big houses, a connection banner.

## 0.2.0

- First hub: displays, layouts, the mock house and the virtual display.
