# Changelog

## 0.4.1

- Admin page: a **Room, then device** grouping: each room (HA area) holds
  its devices as groups of their own, and its loose entities (helpers,
  scenes) directly. **Expand all** opens the devices inside rooms too.
- Copyright and MIT License notices on the admin page, the virtual display,
  the add-on documentation, and the display's Setup screen and web pages.

## 0.4.0

- Admin page: the House panel is now where tiles are added. Entities are
  grouped by device (or area, or type) in collapsible groups; **+ Add** puts
  one on the highlighted page and the list stays where it was.
- Search with several words, matched against name, device, area, entity id
  and type, ranked, highlighted, and with `-word` to exclude.
- Type filters (lights, switches, climate, scenes, sensors), and diagnostic
  and settings entities hidden by default.
- Home Assistant backend reads each entity's device and entity category
  from HA's registries. The mock house has devices too.

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
