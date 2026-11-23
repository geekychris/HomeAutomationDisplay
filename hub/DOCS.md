# Display Hub

Puts your Home Assistant entities on ESP32 "Cheap Yellow Display"
touchscreens. Displays find this add-on by themselves. You choose what each
one shows from **Displays** in the sidebar.

## Getting started

1. Start the add-on and turn on **Show in sidebar**.
2. Flash a display with the firmware from
   <https://github.com/geekychris/HomeAutomationDisplay>, and join it to
   your WiFi from its screen (gear → WiFi).
3. The display appears in **Displays**. Press **Identify** to see which
   panel it is, give it a name, add tiles, and press **Save to display**.

## Scenes

Scenes set several lights, switches, covers, locks and thermostats at once.
Make them under **Scenes** in **Displays**: add entities as they are now,
adjust them, or set the room up and press **Capture current state**. Then
add scenes to a display's **Scene strip**. On the display, tap a scene to
apply it, or hold it to save the room into it (unless the scene is locked).
These scenes are kept by this add-on, not in Home Assistant's scene list.
[More about scenes](https://github.com/geekychris/HomeAutomationDisplay/blob/master/docs/scenes.md).

## Options

- **lan_admin**: also serve the admin page at `http://<this host>:8390/`
  to everyone on your network, without a login. Off by default: the admin
  page is then only reachable through Home Assistant's sidebar.
- **mdns**: advertise the hub (`_hadisplay._tcp`) so displays find it.
  Turn it off only if you give each display the hub's address yourself
  (`http://<display>.local/hub`).
- **advertise_ip**: the address displays should connect to, if this host
  has several.
- **log_level**: how much the add-on logs.

## Network

The add-on uses the host's network and listens on port **8390**. Displays
connect there (`ws://<this host>:8390/ws`). The display connection needs no
login, but a display can only control the entities you have put on it, and
the scenes on its strip.

## Data

Layouts and scenes are kept in the add-on's `/data/displays.json` and
included in Home Assistant backups.

## Supported entities

Lights, fans, covers, switches, input booleans, automations, sirens,
humidifiers, locks, sensors, binary sensors, numbers, counters, selects,
texts, people, device trackers, weather, sun, updates, thermostats, scenes,
scripts and buttons. See the
[entity reference](https://github.com/geekychris/HomeAutomationDisplay/blob/master/docs/entities.md).

## License

© 2026 Chris Collins. MIT License: see
[LICENSE](https://github.com/geekychris/HomeAutomationDisplay/blob/master/LICENSE).
