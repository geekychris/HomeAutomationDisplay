# Display protocol (version 1)

How a display and the hub talk. The firmware (`components/hub/hub.c`), the
virtual display (`hub/had_hub/static/virtual.html`) and the hub
(`hub/had_hub/server.py`) all follow this page.

## Connecting

The display holds a WebSocket to the hub, at the first of:

1. an address saved on the display (`http://<name>.local/hub`),
2. `CONFIG_HAD_HUB_URL` from menuconfig,
3. whatever answers an mDNS query for `_hadisplay._tcp` (TXT `path` gives
   the WebSocket path, normally `/ws`).

Every message is one JSON object in a text frame, with its type in `t`.
Keys are short because the display parses them on a chip with ~50 KB free.

## Display → hub

**hello**: the first message, always.

```json
{"t":"hello","id":"cyd-a1b2c3","name":"home-display-a1b2c3","fw":"0.2.0","proto":1,"w":320,"h":240}
```

`id` is fixed per device (from its MAC) and is how the hub knows which
layout it gets. A display the hub has not seen before is added with no
pages. If a display with the same id is already connected, the older
connection is closed.

**act**: someone tapped something.

```json
{"t":"act","e":"light.kitchen","a":"toggle"}
{"t":"act","e":"light.kitchen","a":"bri","v":40}
{"t":"act","e":"climate.house_heater","a":"target","v":21.5}
{"t":"act","e":"climate.house_heater","a":"mode","v":"off"}
```

| `a` | For | `v` |
|---|---|---|
| `toggle`, `on`, `off` | light, switch | none |
| `bri` | light | the level, 0-100: brightness, fan speed or cover position (0 turns a light or fan off) |
| `target` | climate | temperature, clamped to min/max |
| `mode` | climate | one of the tile's `modes` |
| `press` | button | none |

The hub turns each action into the Home Assistant service call that fits
the entity's domain; [entities.md](entities.md) lists them all.

The hub only accepts actions on entities that are on that display. The
display shows its guess straight away; the hub's next `state` confirms or
corrects it.

**ping**: optional; the hub answers `pong`. WebSocket pings also work.

## Hub → display

**layout**: what to show. Sent after hello and whenever the layout is
changed in the admin page.

```json
{"t":"layout","rev":7,"title":"Kitchen","pages":[
  {"n":"Lights","tiles":[
    {"e":"light.kitchen","k":"light","n":"Kitchen","dim":true},
    {"e":"switch.coffee_maker","k":"switch","n":"Coffee maker"}]},
  {"n":"Climate","tiles":[
    {"e":"climate.house_heater","k":"climate","n":"Heating"},
    {"e":"sensor.living_room_co2","k":"sensor","n":"CO2"}]}]}
```

With no tiles, `pages` is empty and `msg` says what to do
("Not set up yet..."). Limits, enforced by the hub: 6 pages, 8 tiles a page,
24 tiles in all, names 23 bytes, page names 15, entity ids 63.

Tile kinds (`k`), and what each Home Assistant domain becomes, are in
[entities.md](entities.md):

| `k` | Draws | Tile fields |
|---|---|---|
| `light` | power symbol, on/off words, and a 0-100 slider if `dim` | `dim` |
| `switch` | power symbol, on/off words and a switch | |
| `sensor` | big value and unit, a line under it | |
| `climate` | a full-width thermostat | |
| `button` | a symbol and a line of text; tap runs it | |

`ic` (optional, any kind) is a hint for the corner symbol: `fan`, `cover`,
`bell`, `lock`, `play`. Anything else, or none, is a power symbol.

A display skips kinds it does not know, and ignores fields it does not
know, so new kinds and fields can be added without breaking older firmware.

**state**: one entity's live state. Sent for every tile after a layout, and
then whenever the entity changes. `ok` false means unavailable, and nothing
else is sent.

```json
{"t":"state","e":"light.kitchen","ok":true,"on":true,"bri":40}
{"t":"state","e":"cover.living_room_blind","ok":true,"on":true,"bri":60,"txt":"Open 60%"}
{"t":"state","e":"switch.coffee_maker","ok":true,"on":false}
{"t":"state","e":"lock.front_door","ok":true,"on":true,"txt":"Locked"}
{"t":"state","e":"sensor.living_room_co2","ok":true,"val":"812","unit":"ppm","lvl":1}
{"t":"state","e":"weather.home","ok":true,"val":"8.2","unit":"°C","lvl":0,"txt":"Partly cloudy"}
{"t":"state","e":"climate.house_heater","ok":true,"mode":"heat","modes":["off","heat"],
 "act":"heating","cur":19.4,"tgt":20.5,"min":7,"max":30,"step":0.5,"unit":"°C"}
{"t":"state","e":"script.good_night","ok":true,"on":false,"txt":"Run"}
```

| Field | Kinds | Meaning |
|---|---|---|
| `ok` | all | false: unavailable; nothing else is sent |
| `on` | light, switch, button | on / open / locked; for a button, running |
| `bri` | light | the level, 0-100 |
| `txt` | light, switch, sensor, button | words to show instead of the display's own ("Locked", "Open 60%", "Partly cloudy"); max 15 bytes |
| `val`, `unit` | sensor | already formatted text; max 11 and 7 bytes |
| `lvl` | sensor | the hub's judgement: 0 none, 1 good, 2 warn, 3 bad |
| `mode`, `modes`, `act`, `cur`, `tgt`, `min`, `max`, `step`, `unit` | climate | hvac mode, up to 4 modes, hvac action, temperatures |

The hub works `lvl` out from the sensor's device class (CO2 warns at 1000
ppm and is bad at 1500, battery warns at 20% and is bad at 10%...) or from
per-tile thresholds set in the admin page.

**identify**: show your id on screen for `s` seconds, so you can tell which
physical display is which. `{"t":"identify","s":10}`

**error**: something went wrong, for a short message on screen.
`{"t":"error","msg":"climate.house_heater has no mode 'cool'"}`
