#include "netcfg.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_server.h"
#include "ping/ping_sock.h"
#include "lwip/netdb.h"
#include "nvs.h"
#include "cJSON.h"
#include "mdns.h"

static const char *TAG = "netcfg";

static esp_netif_t *s_sta, *s_ap;
static bool  s_ap_up;
static int   s_fail;
static char  s_ip[16] = "-";
static char  s_ssid[33] = {0};
static int   s_rssi;
static char  s_hostname[33];
static httpd_handle_t s_httpd;

const char *netcfg_hostname(void) { return s_hostname; }
httpd_handle_t netcfg_httpd(void) { return s_httpd; }

// Each display needs its own name on the network, or several of them would
// fight over one mDNS name: "home-display-a1b2c3", from the MAC.
static void make_hostname(void)
{
#ifdef CONFIG_HAD_HOSTNAME_ADD_MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_hostname, sizeof(s_hostname), "%.25s-%02x%02x%02x", CONFIG_HAD_HOSTNAME, mac[3], mac[4], mac[5]);
#else
    snprintf(s_hostname, sizeof(s_hostname), "%s", CONFIG_HAD_HOSTNAME);
#endif
}

// ---------------------------------------------------------------- credentials
//
// Tried in turn: the last one that worked first, a few attempts each, then
// the next. NVS namespace "wifi", keys ssid0..ssidN / pass0..passN, plus
// "nnets" and "last".
#define TRIES_PER_NET  3
typedef struct { char ssid[33]; char pass[65]; } wifi_cred_t;
static wifi_cred_t s_nets[NETCFG_MAX];
static int s_nnets, s_cur, s_tries;
static SemaphoreHandle_t s_net_lock;     // UI and web tasks edit the list; the event task reads it

static void creds_store(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    char k[8];
    for (int i = 0; i < NETCFG_MAX; i++) {
        snprintf(k, sizeof(k), "ssid%d", i);
        if (i < s_nnets) nvs_set_str(h, k, s_nets[i].ssid); else nvs_erase_key(h, k);
        snprintf(k, sizeof(k), "pass%d", i);
        if (i < s_nnets) nvs_set_str(h, k, s_nets[i].pass); else nvs_erase_key(h, k);
    }
    nvs_set_u8(h, "nnets", (uint8_t)s_nnets);
    nvs_set_u8(h, "last", (uint8_t)s_cur);
    nvs_commit(h);
    nvs_close(h);
}

static void creds_load(void)
{
    nvs_handle_t h;
    s_nnets = 0; s_cur = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t n = 0, last = 0;
    char k[8];
    if (nvs_get_u8(h, "nnets", &n) == ESP_OK) {
        for (int i = 0; i < n && i < NETCFG_MAX; i++) {
            size_t a = sizeof(s_nets[0].ssid), b = sizeof(s_nets[0].pass);
            snprintf(k, sizeof(k), "ssid%d", i);
            if (nvs_get_str(h, k, s_nets[s_nnets].ssid, &a) != ESP_OK || a <= 1) continue;
            snprintf(k, sizeof(k), "pass%d", i);
            if (nvs_get_str(h, k, s_nets[s_nnets].pass, &b) != ESP_OK) s_nets[s_nnets].pass[0] = 0;
            s_nnets++;
        }
        if (nvs_get_u8(h, "last", &last) == ESP_OK && last < s_nnets) s_cur = last;
    }
    nvs_close(h);
}

// ---------------------------------------------------------------- wifi

// The AP config has to be in place BEFORE esp_wifi_start(). Starting in APSTA
// with an unset AP config brings the interface up with a zero-length SSID,
// which does not appear in any scan - and esp_wifi_start() may reject it
// outright, which under ESP_ERROR_CHECK is a boot loop.
static void ap_configure(void)
{
    wifi_config_t wc = { .ap = { .ssid_len = strlen(CONFIG_HAD_AP_SSID),
                                 .channel = 1, .max_connection = 4,
                                 .authmode = WIFI_AUTH_OPEN,
                                 .beacon_interval = 100 } };
    strncpy((char *)wc.ap.ssid, CONFIG_HAD_AP_SSID, sizeof(wc.ap.ssid) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
}

static void ap_up(void)
{
    if (s_ap_up) return;
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    s_ap_up = (e == ESP_OK);
    ESP_LOGW(TAG, "AP '%s' %s - join it and open http://192.168.4.1/",
             CONFIG_HAD_AP_SSID, s_ap_up ? "up" : esp_err_to_name(e));
}

// Point the station at network s_cur and start connecting.
static void try_current(void)
{
    wifi_config_t wc = {0};
    xSemaphoreTake(s_net_lock, portMAX_DELAY);
    if (s_nnets == 0) { xSemaphoreGive(s_net_lock); s_ssid[0] = 0; return; }
    if (s_cur >= s_nnets) s_cur = 0;
    strncpy((char *)wc.sta.ssid, s_nets[s_cur].ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_nets[s_cur].pass, sizeof(wc.sta.password) - 1);
    strcpy(s_ssid, s_nets[s_cur].ssid);
    xSemaphoreGive(s_net_lock);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_nnets) { ESP_LOGI(TAG, "joining '%s'", s_nets[s_cur].ssid); try_current(); }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        strcpy(s_ip, "-");
        if (!s_nnets) return;
        // A few attempts at this network, then the next one in the list.
        if (++s_tries >= TRIES_PER_NET && s_nnets > 1) {
            s_tries = 0;
            s_cur = (s_cur + 1) % s_nnets;
            ESP_LOGW(TAG, "giving up on '%s' (reason %d) - trying '%s'", s_ssid, d->reason, s_nets[s_cur].ssid);
        }
        // Never get locked out by wrong or absent networks: once every network
        // has had its turn, bring the AP up alongside so setup stays
        // reachable and the list can be corrected. The STA keeps trying.
        int limit = s_nnets * TRIES_PER_NET;
        if (limit < CONFIG_HAD_WIFI_MAX_RETRY) limit = CONFIG_HAD_WIFI_MAX_RETRY;
        if (++s_fail >= limit) ap_up();
        try_current();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_fail = 0; s_tries = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
        ESP_LOGI(TAG, "joined %s (%d dBm) - http://%s/", s_ssid, s_rssi, s_ip);
        // Remember which one worked, to try it first next boot.
        nvs_handle_t h;
        uint8_t last = 0xff;
        if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_get_u8(h, "last", &last) != ESP_OK || last != s_cur) { nvs_set_u8(h, "last", (uint8_t)s_cur); nvs_commit(h); }
            nvs_close(h);
        }
        // The AP was only there to get us here.
        if (s_ap_up && esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) s_ap_up = false;
        // Started once; SNTP keeps resyncing by itself and survives reconnects.
        static bool sntp_up;
        if (!sntp_up) {
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_HAD_NTP_SERVER);
            sntp_up = esp_netif_sntp_init(&sc) == ESP_OK;
        }
    }
}

// ---------------------------------------------------------------- the list

esp_err_t netcfg_forget_all(void)
{
    xSemaphoreTake(s_net_lock, portMAX_DELAY);
    s_nnets = 0; s_cur = 0;
    creds_store();
    xSemaphoreGive(s_net_lock);
    return ESP_OK;
}

esp_err_t netcfg_add(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) return ESP_ERR_INVALID_ARG;
    esp_err_t e = ESP_OK;
    xSemaphoreTake(s_net_lock, portMAX_DELAY);
    int i = 0;
    while (i < s_nnets && strcmp(s_nets[i].ssid, ssid)) i++;
    if (i == s_nnets) {
        if (s_nnets == NETCFG_MAX) e = ESP_ERR_NO_MEM; else s_nnets++;
    }
    if (e == ESP_OK) {
        strcpy(s_nets[i].ssid, ssid);
        strcpy(s_nets[i].pass, pass ? pass : "");
        creds_store();
    }
    const bool now = e == ESP_OK && !netcfg_connected();
    if (now) { s_cur = i; s_tries = 0; }
    xSemaphoreGive(s_net_lock);
    ESP_LOGI(TAG, "network '%s' %s", ssid, e == ESP_OK ? "saved" : "not saved: list full");
    // Not on any network: go for this one now rather than waiting for the
    // turn of whatever was failing.
    if (now) { esp_wifi_disconnect(); try_current(); }
    return e;
}

esp_err_t netcfg_remove(const char *ssid)
{
    esp_err_t e = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_net_lock, portMAX_DELAY);
    for (int i = 0; i < s_nnets; i++) {
        if (strcmp(s_nets[i].ssid, ssid)) continue;
        memmove(&s_nets[i], &s_nets[i + 1], (s_nnets - i - 1) * sizeof(s_nets[0]));
        s_nnets--;
        if (s_cur > i || s_cur >= s_nnets) s_cur = s_cur > 0 ? s_cur - 1 : 0;
        creds_store();
        e = ESP_OK;
        break;
    }
    xSemaphoreGive(s_net_lock);
    return e;
}

int netcfg_saved(char names[][33], int max)
{
    xSemaphoreTake(s_net_lock, portMAX_DELAY);
    int n = 0;
    for (; n < s_nnets && n < max; n++) strcpy(names[n], s_nets[n].ssid);
    xSemaphoreGive(s_net_lock);
    return n;
}

int netcfg_scan(netcfg_ap_t *out, int max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;
    uint16_t n = 24;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) { esp_wifi_clear_ap_list(); return -1; }
    esp_wifi_scan_get_ap_records(&n, recs);
    // Records come strongest first; keep one row per name, not per access point.
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        const char *s = (const char *)recs[i].ssid;
        if (!s[0]) continue;
        bool dup = false;
        for (int j = 0; j < k; j++) if (!strcmp(out[j].ssid, s)) dup = true;
        if (dup) continue;
        snprintf(out[k].ssid, sizeof(out[k].ssid), "%s", s);
        out[k].rssi = recs[i].rssi;
        out[k].open = recs[i].authmode == WIFI_AUTH_OPEN;
        k++;
    }
    free(recs);
    return k;
}

bool netcfg_connected(void) { return strcmp(s_ip, "-") != 0; }

bool netcfg_time_valid(void)
{
    return time(NULL) > 1700000000;     // anything before late 2023 is the epoch plus uptime
}

void netcfg_summary(char *buf, size_t n)
{
    if (netcfg_connected()) snprintf(buf, n, "%s  %s.local  %s", s_ssid, s_hostname, s_ip);
    else if (s_ap_up)       snprintf(buf, n, "AP \"%s\"  192.168.4.1", CONFIG_HAD_AP_SSID);
    else if (s_nnets)       snprintf(buf, n, "joining %s", s_ssid[0] ? s_ssid : s_nets[s_cur].ssid);
    else                    snprintf(buf, n, "no WiFi set up");
}

// ---------------------------------------------------------------- watchdog
//
// A station can stay "associated" while the router has quietly stopped
// passing its traffic - a weak signal, or the router restarting - and then
// nothing ever reconnects it. Every 30 s, check traffic gets through: a ping
// to the gateway, or failing that a DNS lookup (some routers ignore pings).
// After 4 failed checks in a row, drop the association; the disconnect
// handler then reconnects from scratch.
typedef struct { SemaphoreHandle_t done; uint32_t rx; } ping_ctx_t;
static void ping_end(esp_ping_handle_t h, void *arg)
{
    ping_ctx_t *c = arg;
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &c->rx, sizeof(c->rx));
    xSemaphoreGive(c->done);
}

static bool link_works(void)
{
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta, &ip) != ESP_OK || !ip.gw.addr) return false;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr.u_addr.ip4.addr = ip.gw.addr;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.count = 3;
    cfg.interval_ms = 300;
    cfg.timeout_ms = 1000;
    ping_ctx_t c = { .done = xSemaphoreCreateBinary(), .rx = 0 };
    esp_ping_callbacks_t cbs = { .on_ping_end = ping_end, .cb_args = &c };
    esp_ping_handle_t h = NULL;
    if (c.done && esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK) {
        esp_ping_start(h);
        xSemaphoreTake(c.done, pdMS_TO_TICKS(6000));
        esp_ping_stop(h);
        esp_ping_delete_session(h);
    }
    if (c.done) vSemaphoreDelete(c.done);
    if (c.rx > 0) return true;
    struct addrinfo *res = NULL;
    const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    const bool dns = getaddrinfo(CONFIG_HAD_NTP_SERVER, "123", &hints, &res) == 0 && res;
    if (res) freeaddrinfo(res);
    return dns;
}

static void netwatch_task(void *arg)
{
    int bad = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (!netcfg_connected()) { bad = 0; continue; }     // not associated: the event handler is on it
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
        if (link_works()) { bad = 0; continue; }
        ESP_LOGW(TAG, "connected to '%s' (%d dBm) but traffic is not getting through (%d)", s_ssid, s_rssi, bad + 1);
        if (++bad >= 4) {
            bad = 0;
            ESP_LOGW(TAG, "dropping the WiFi association to reconnect");
            esp_wifi_disconnect();          // STA_DISCONNECTED then reconnects via try_current()
        }
    }
}

// ---------------------------------------------------------------- web setup
//
// Just enough to set up WiFi from a phone on the fallback AP: the saved
// list, a scan, and a form. Everything else is the touchscreen's job.
static const char PAGE[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
"<title>WiFi setup</title><style>body{font:16px system-ui;margin:16px;max-width:30em}"
"li{margin:.3em 0}button{margin-left:.5em}input{font:inherit;width:100%;margin:.3em 0}</style>"
"<h2>WiFi setup</h2><p id=now></p><p><a href=/hub>Hub settings</a></p><h3>Saved</h3><ul id=saved></ul>"
"<h3>Add</h3><form id=f><input id=s placeholder='Network name' list=found>"
"<datalist id=found></datalist><input id=p type=password placeholder=Password>"
"<button>Save</button> <button type=button id=scan>Scan</button></form><p id=msg></p>"
"<script>const $=i=>document.getElementById(i);"
"async function load(){const j=await(await fetch('/api/net')).json();$('now').textContent=j.summary;"
"$('saved').innerHTML='';for(const n of j.saved){const li=document.createElement('li');li.textContent=n;"
"const b=document.createElement('button');b.textContent='Remove';b.onclick=()=>post({remove:n});"
"li.append(b);$('saved').append(li)}}"
"async function post(o){const j=await(await fetch('/api/wifi',{method:'POST',body:JSON.stringify(o)})).json();"
"$('msg').textContent=j.ok?'Saved.':j.err;load()}"
"$('f').onsubmit=e=>{e.preventDefault();post({ssid:$('s').value,pass:$('p').value})};"
"$('scan').onclick=async()=>{$('msg').textContent='Scanning...';"
"const j=await(await fetch('/api/scan')).json();$('found').innerHTML='';"
"for(const n of j.nets){const o=document.createElement('option');o.value=n.ssid;$('found').append(o)}"
"$('msg').textContent=j.nets.length+' found - pick one in the name box'};load()</script>";

static esp_err_t send_json(httpd_req_t *r, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, s);
    free(s);
    return e;
}

static esp_err_t h_root(httpd_req_t *r) { return httpd_resp_send(r, PAGE, sizeof(PAGE) - 1); }

static esp_err_t h_net(httpd_req_t *r)
{
    char line[96], names[NETCFG_MAX][33];
    netcfg_summary(line, sizeof(line));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "summary", line);
    cJSON_AddBoolToObject(j, "connected", netcfg_connected());
    cJSON_AddNumberToObject(j, "rssi", s_rssi);
    cJSON *a = cJSON_AddArrayToObject(j, "saved");
    const int n = netcfg_saved(names, NETCFG_MAX);
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateString(names[i]));
    return send_json(r, j);
}

static esp_err_t h_scan(httpd_req_t *r)
{
    netcfg_ap_t *aps = calloc(16, sizeof(*aps));
    const int n = aps ? netcfg_scan(aps, 16) : -1;
    cJSON *j = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(j, "nets");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "open", aps[i].open);
        cJSON_AddItemToArray(a, o);
    }
    free(aps);
    return send_json(r, j);
}

// {"ssid":"x","pass":"y"} adds or updates, {"remove":"x"} removes.
static esp_err_t h_wifi(httpd_req_t *r)
{
    char body[256];
    const int len = r->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body");
    for (int got = 0; got < len;) {
        const int k = httpd_req_recv(r, body + got, len - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[len] = 0;
    cJSON *in = cJSON_Parse(body);
    const cJSON *s = cJSON_GetObjectItem(in, "ssid"), *p = cJSON_GetObjectItem(in, "pass");
    const cJSON *rm = cJSON_GetObjectItem(in, "remove");
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(rm)) e = netcfg_remove(rm->valuestring);
    else if (cJSON_IsString(s)) e = netcfg_add(s->valuestring, cJSON_IsString(p) ? p->valuestring : "");
    cJSON_Delete(in);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", e == ESP_OK);
    if (e != ESP_OK)
        cJSON_AddStringToObject(j, "err", e == ESP_ERR_NO_MEM ? "the list is full - remove one first"
                                        : e == ESP_ERR_NOT_FOUND ? "no such network" : "bad request");
    return send_json(r, j);
}

static void start_http(void)
{
    httpd_handle_t srv = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    // Recycle the least recently used socket rather than refusing new ones
    // when a phone leaves keep-alive connections dangling.
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;
    cfg.max_uri_handlers = 12;          // room for other components' pages (netcfg_httpd)
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd failed"); return; }
    s_httpd = srv;
    const httpd_uri_t u[] = {
        { .uri = "/",          .method = HTTP_GET,  .handler = h_root },
        { .uri = "/api/net",   .method = HTTP_GET,  .handler = h_net  },
        { .uri = "/api/scan",  .method = HTTP_GET,  .handler = h_scan },
        { .uri = "/api/wifi",  .method = HTTP_POST, .handler = h_wifi },
    };
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) httpd_register_uri_handler(srv, &u[i]);
}

// ---------------------------------------------------------------- start

esp_err_t netcfg_start(void)
{
    s_net_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // Both interfaces always exist: the STA side is needed to scan for networks
    // even when we are only serving the setup AP.
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap  = esp_netif_create_default_wifi_ap();
    // The same name for DHCP (so it shows in the router's device list) and
    // mDNS (so http://<name>.local/ finds it without knowing the address).
    make_hostname();
    esp_netif_set_hostname(s_sta, s_hostname);
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));

    creds_load();

    // Configure both interfaces first, then choose the mode, then start. The
    // STA config itself is set by try_current() when the STA starts.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ap_configure();
    if (s_nnets) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_LOGI(TAG, "%d saved network%s", s_nnets, s_nnets == 1 ? "" : "s");
    } else {
        s_ap_up = true;                 // already in APSTA with a valid AP config
        ESP_LOGW(TAG, "no saved networks - AP '%s', open http://192.168.4.1/", CONFIG_HAD_AP_SSID);
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    setenv("TZ", CONFIG_HAD_TZ, 1);
    tzset();
    start_http();
    // Not fatal: without it the device is still reachable by address.
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(s_hostname);
        mdns_instance_name_set("Home Automation Display");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS: http://%s.local/", s_hostname);
    } else {
        ESP_LOGW(TAG, "mDNS did not start");
    }
    xTaskCreate(netwatch_task, "netwatch", 4096, NULL, 3, NULL);
    return ESP_OK;
}
