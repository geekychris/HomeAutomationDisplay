// The link to the hub: finds it (a saved address, the Kconfig default, or
// mDNS _hadisplay._tcp), holds a WebSocket to it, and keeps the layout and
// the live state of every tile it sends. Taps go back as actions.
//
// The UI reads the model under hub_lock(), from an LVGL timer: compare
// hub_layout_t.gen to notice a new layout, and each tile's seq to notice new
// state. Nothing here calls LVGL, so the two never wait on each other.
//
// Protocol: docs/protocol.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Must match the hub (hub/had_hub/entities.py).
#define HUB_MAX_PAGES      6
#define HUB_MAX_TILES      24
#define HUB_ENTITY_LEN     64
#define HUB_NAME_LEN       24
#define HUB_PAGE_NAME_LEN  16
#define HUB_MAX_MODES      4

// What a tile draws. The hub maps every Home Assistant domain onto one of
// these (hub/had_hub/entities.py): LIGHT is on/off with an optional 0-100
// level (lights, fans, covers), SWITCH is on/off (switches, locks, ...),
// BUTTON is tap-to-run (scenes, scripts, buttons).
typedef enum { HUB_LIGHT, HUB_SWITCH, HUB_SENSOR, HUB_CLIMATE, HUB_BUTTON } hub_kind_t;

// The tile's corner symbol, from the layout's "ic".
typedef enum { HUB_IC_POWER, HUB_IC_FAN, HUB_IC_COVER, HUB_IC_BELL, HUB_IC_LOCK, HUB_IC_PLAY } hub_icon_t;

typedef struct {
    char       entity[HUB_ENTITY_LEN];
    char       name[HUB_NAME_LEN];
    hub_kind_t kind;
    hub_icon_t icon;
    uint8_t    page;
    bool       dimmable;            // light: has a level slider
    // Live state. seq goes up with every change.
    uint32_t   seq;
    bool       ok;                  // available
    bool       on;                  // light, switch; button: running
    uint8_t    bri;                 // light: level 0-100
    char       txt[16];             // the hub's status words ("Open 40%", "Locked"); "" to use the default
    char       val[12], unit[8];    // sensor
    uint8_t    lvl;                 // sensor: 0 none, 1 good, 2 warn, 3 bad
    float      cur, tgt, tmin, tmax, step;   // climate; cur/tgt NAN if unknown
    char       mode[10], act[10];   // climate: hvac mode, hvac action
    char       modes[HUB_MAX_MODES][10];
    uint8_t    nmodes;
} hub_tile_t;

typedef struct {
    uint32_t   gen;                 // goes up with every layout received
    char       title[HUB_NAME_LEN];
    char       msg[112];            // shown when there are no tiles
    uint8_t    npages;
    char       page[HUB_MAX_PAGES][HUB_PAGE_NAME_LEN];
    uint8_t    ntiles;
    hub_tile_t tiles[HUB_MAX_TILES];
} hub_layout_t;

typedef enum {
    HUB_NO_WIFI,        // waiting for the network
    HUB_SEARCHING,      // asking mDNS for a hub
    HUB_CONNECTING,
    HUB_ONLINE,         // connected and has a layout
} hub_status_t;

esp_err_t hub_start(void);

// This display's id ("cyd-a1b2c3", from the MAC) as the hub knows it.
const char *hub_device_id(void);

hub_status_t hub_status(void);
// One line for people: "hub 192.168.1.20:8390", "looking for the hub", ...
void hub_status_text(char *buf, size_t n);

// The model. Hold the lock while reading it; do not keep pointers after.
void hub_lock(void);
void hub_unlock(void);
const hub_layout_t *hub_layout(void);

// Actions. They update the model straight away (so the screen answers the
// tap at once); the hub's reply then confirms or corrects it. Safe from the
// LVGL task. Returns false if the hub is not connected.
bool hub_toggle(const char *entity);
bool hub_brightness(const char *entity, int pct);
bool hub_target(const char *entity, float temp);
bool hub_mode(const char *entity, const char *mode);
bool hub_press(const char *entity);                 // button tiles

// Set by the hub's "identify" (show this display's id for a few seconds) and
// "error" messages. Each call returns the latest and clears it.
int  hub_take_identify(void);                       // seconds, or 0
bool hub_take_error(char *buf, size_t n);

// The hub address, saved in NVS: "ws://host:port/ws", or "" to go back to
// the Kconfig default and then mDNS. Reconnects.
esp_err_t hub_set_url(const char *url);
void hub_get_url(char *buf, size_t n);              // the saved one, or ""
