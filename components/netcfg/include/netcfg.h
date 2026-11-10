// WiFi: up to NETCFG_MAX saved networks, tried in turn, kept in NVS. With
// none saved, or none reachable, an open access point (CONFIG_HAD_AP_SSID)
// comes up alongside so the device can always be set up, from the
// touchscreen or from a browser at http://192.168.4.1/.
//
// Once joined: mDNS (http://<netcfg_hostname()>.local/), SNTP for the
// clock, and a watchdog that reconnects when the association looks fine but
// traffic has stopped getting through.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#define NETCFG_MAX 5

typedef struct { char ssid[33]; int rssi; bool open; } netcfg_ap_t;

esp_err_t netcfg_start(void);

// Add a network, or change the password of one already saved. If nothing is
// connected yet, it is tried straight away. ESP_ERR_NO_MEM: the list is full.
esp_err_t netcfg_add(const char *ssid, const char *pass);
esp_err_t netcfg_remove(const char *ssid);       // ESP_ERR_NOT_FOUND if not saved
esp_err_t netcfg_forget_all(void);

// The saved names (never the passwords), in the order they are tried.
int netcfg_saved(char names[][33], int max);

// Scan for networks: blocks for a couple of seconds. One entry per name,
// strongest first. Returns how many, or -1 if the radio is busy.
int netcfg_scan(netcfg_ap_t *out, int max);

bool netcfg_connected(void);
bool netcfg_time_valid(void);                    // SNTP has set the clock

// This device's name on the network: CONFIG_HAD_HOSTNAME, plus the end of
// the MAC unless HAD_HOSTNAME_ADD_MAC is off. Set once netcfg_start() runs.
const char *netcfg_hostname(void);

// The web server, for other components to add pages to. NULL if it did not start.
httpd_handle_t netcfg_httpd(void);

// One line for a status bar: "ssid  name.local  ip", the AP, "joining x" or
// "no WiFi set up".
void netcfg_summary(char *buf, size_t n);
