# Scenes

A scene sets several things at once: the living room light to 30%, the
blind closed, the heating to 21.5°. You make scenes in the hub's admin page
(in Home Assistant's sidebar, under **Displays**), give each display its own
set of them, and they appear as a strip of buttons along the bottom of
every page on that display.

```
 ┌───────────────────────────────────┐
 │ <  Living room  >       18:45  o  │
 │ ┌─────────────┐ ┌─────────────┐   │
 │ │ Living room │ │ Kitchen     │   │   tiles shrink a little
 │ └─────────────┘ └─────────────┘   │   so two rows still fit
 │ ┌───────────────────────────────┐ │
 │ │ Heating  20.5°     − 21.5° +  │ │
 │ └───────────────────────────────┘ │
 │ (Evening) ( Bright ) ( Away )     │   ← the scene strip
 └───────────────────────────────────┘
```

These scenes belong to the hub, not to Home Assistant: they are kept in the
hub's data file (in HA backups when the hub runs as an add-on) and do not
appear under Settings → Scenes. Home Assistant's own scenes still work as
tiles (tap to activate), as before.

## On the display

- **Tap** a scene to apply it.
- **Hold** a scene (about half a second) to save the room into it: after you
  confirm, everything in that scene takes the state it is in now. "Saved
  Evening" confirms it. A scene that is **locked** in the hub cannot be
  saved over from a display; holding it says so.
- The scene that is **on** is highlighted. "On" means everything in the
  scene is as the scene says, near enough (levels within 3%, temperatures
  within a quarter of a degree). So if someone turns a light up by hand, the
  highlight goes out. If several scenes on the strip are on at once, the
  first one is highlighted.

A display can apply and save only the scenes on its own strip. A scene can
include entities that are not tiles on that display.

## In the admin page

**Scenes** on the left lists every scene, with a green dot when it is on.

- **+ New scene** makes an empty one and opens it.
- With a scene open, **+ Add** in the house list puts an entity in the
  scene *as it is right now*. A scene can hold lights, fans, covers,
  switches, input booleans, automations, sirens, humidifiers, locks and
  thermostats, up to 32 of them.
- Each entity's row sets what the scene does to it: on or off (open or
  closed, locked or unlocked), the level for dimmable lights, fan speed or
  cover position, and a thermostat's mode and target.
- **Capture current state** sets every entity in the scene to how it is now.
  Set the room up the way you like it, then capture.
- **Apply now** runs it. **Locked** stops displays saving over it.
- **Save scene** keeps your edits; displays showing the scene get its new
  name straight away.

To put scenes on a display, open the display: its layout has a **Scene
strip** section below the pages. Add up to 6 scenes, in the order you want
them, and **Save to display**. Several displays can share a scene.

## How applying works

The hub turns each entity's values into the same actions a tap would send
(see [entities.md](entities.md)): `bri` for a level, `on`/`off` otherwise,
and `mode` then `target` for a thermostat. Entities already in the right
state are skipped, and the rest go in parallel. If some fail (an entity is
unavailable, say), the others still change, and the display shows how many
did not.

## Limits

| | |
|---|---|
| Scenes in the hub | 64 |
| Entities in a scene | 32 |
| Scenes on one display's strip | 6 |
| Scene name | 15 bytes (about 15 letters) |
