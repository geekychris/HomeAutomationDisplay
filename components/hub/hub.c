// The link to the hub. See hub.h; the messages are in docs/protocol.md.
#include "hub.h"
#include "board.h"
#include "sdkconfig.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "mdns.h"
#include "nvs.h"
#include "netcfg.h"

static const char *TAG = "hub";

void hub_web_start(void);         // hub_web.c

#define PROTO        1
#define MAX_MSG      8192           // a full layout is ~3 KB
#define REDISCOVER_S 30             // offline this long: ask mDNS again (the hub may have moved)

// Static, so it exists before hub_start() and cannot fail to allocate.
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static SemaphoreHandle_t lock_handle(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);   // first use is from app_main
    return s_lock;
}
static hub_layout_t s_layout;
static char s_id[16];
static char s_url[96];             // what we are connected (or connecting) to
static volatile hub_status_t s_status = HUB_NO_WIFI;
static volatile bool s_url_changed;
static volatile bool s_has_layout;
static volatile int s_identify;
static char s_error[96];
static esp_websocket_client_handle_t s_ws;

void hub_lock(void)   { xSemaphoreTake(lock_handle(), portMAX_DELAY); }
void hub_unlock(void) { xSemaphoreGive(lock_handle()); }
const hub_layout_t *hub_layout(void) { return &s_layout; }
const char *hub_device_id(void) { return s_id; }
hub_status_t hub_status(void) { return s_status; }

// ---------------------------------------------------------------- settings

void hub_get_url(char *buf, size_t n)
{
    buf[0] = 0;
    nvs_handle_t h;
    if (nvs_open("hub", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = n;
    if (nvs_get_str(h, "url", buf, &len) != ESP_OK) buf[0] = 0;
    nvs_close(h);
}

esp_err_t hub_set_url(const char *url)
{
    if (strlen(url) >= sizeof(s_url)) return ESP_ERR_INVALID_SIZE;
    if (url[0] && strncmp(url, "ws://", 5) && strncmp(url, "wss://", 6)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open("hub", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = url[0] ? nvs_set_str(h, "url", url) : nvs_erase_key(h, "url");
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) s_url_changed = true;
    return e;
}

void hub_status_text(char *buf, size_t n)
{
    // "ws://192.168.1.20:8390/ws" -> "192.168.1.20:8390"
    char host[64] = "";
    const char *p = strstr(s_url, "://");
    if (p) {
        snprintf(host, sizeof(host), "%s", p + 3);
        char *slash = strchr(host, '/');
        if (slash) *slash = 0;
    }
    switch (s_status) {
    case HUB_NO_WIFI:    snprintf(buf, n, "hub: waiting for WiFi"); break;
    case HUB_SEARCHING:  snprintf(buf, n, "looking for the hub on the network"); break;
    case HUB_CONNECTING: snprintf(buf, n, "connecting to the hub at %s", host); break;
    case HUB_ONLINE:     snprintf(buf, n, "hub %s", host); break;
    }
}

int hub_take_identify(void) { int s = s_identify; s_identify = 0; return s; }

bool hub_take_error(char *buf, size_t n)
{
    hub_lock();
    const bool any = s_error[0] != 0;
    if (any) snprintf(buf, n, "%s", s_error);
    s_error[0] = 0;
    hub_unlock();
    return any;
}

// ---------------------------------------------------------------- from the hub

static void copy_str(char *dst, size_t n, const cJSON *j)
{
    snprintf(dst, n, "%s", cJSON_IsString(j) ? j->valuestring : "");
}

static float num_or(const cJSON *j, float dflt)
{
    return cJSON_IsNumber(j) ? (float)j->valuedouble : dflt;
}

static void on_layout(const cJSON *m)
{
    hub_lock();
    hub_layout_t *L = &s_layout;
    const uint32_t gen = L->gen + 1;
    memset(L, 0, sizeof(*L));
    L->gen = gen;
    copy_str(L->title, sizeof(L->title), cJSON_GetObjectItem(m, "title"));
    copy_str(L->msg, sizeof(L->msg), cJSON_GetObjectItem(m, "msg"));
    const cJSON *page;
    cJSON_ArrayForEach(page, cJSON_GetObjectItem(m, "pages")) {
        if (L->npages >= HUB_MAX_PAGES) break;
        copy_str(L->page[L->npages], HUB_PAGE_NAME_LEN, cJSON_GetObjectItem(page, "n"));
        const cJSON *t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItem(page, "tiles")) {
            if (L->ntiles >= HUB_MAX_TILES) break;
            const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(t, "k"));
            hub_tile_t *T = &L->tiles[L->ntiles];
            if (!k) continue;
            if (!strcmp(k, "light"))        T->kind = HUB_LIGHT;
            else if (!strcmp(k, "switch"))  T->kind = HUB_SWITCH;
            else if (!strcmp(k, "sensor"))  T->kind = HUB_SENSOR;
            else if (!strcmp(k, "climate")) T->kind = HUB_CLIMATE;
            else if (!strcmp(k, "button"))  T->kind = HUB_BUTTON;
            else continue;              // a kind this firmware does not know: skip it
            const char *ic = cJSON_GetStringValue(cJSON_GetObjectItem(t, "ic"));
            T->icon = !ic ? HUB_IC_POWER : !strcmp(ic, "fan") ? HUB_IC_FAN : !strcmp(ic, "cover") ? HUB_IC_COVER
                    : !strcmp(ic, "bell") ? HUB_IC_BELL : !strcmp(ic, "lock") ? HUB_IC_LOCK
                    : !strcmp(ic, "play") ? HUB_IC_PLAY : HUB_IC_POWER;
            copy_str(T->entity, sizeof(T->entity), cJSON_GetObjectItem(t, "e"));
            copy_str(T->name, sizeof(T->name), cJSON_GetObjectItem(t, "n"));
            T->dimmable = cJSON_IsTrue(cJSON_GetObjectItem(t, "dim"));
            T->page = L->npages;
            T->cur = T->tgt = NAN;
            L->ntiles++;
        }
        L->npages++;
    }
    hub_unlock();
    s_has_layout = true;
    if (s_status == HUB_CONNECTING) s_status = HUB_ONLINE;
    ESP_LOGI(TAG, "layout \"%s\": %d pages, %d tiles", L->title, L->npages, L->ntiles);
}

static void on_state(const cJSON *m)
{
    const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(m, "e"));
    if (!e) return;
    hub_lock();
    for (int i = 0; i < s_layout.ntiles; i++) {
        hub_tile_t *T = &s_layout.tiles[i];
        if (strcmp(T->entity, e)) continue;
        T->ok = cJSON_IsTrue(cJSON_GetObjectItem(m, "ok"));
        T->on = cJSON_IsTrue(cJSON_GetObjectItem(m, "on"));
        T->bri = (uint8_t)num_or(cJSON_GetObjectItem(m, "bri"), T->on ? 100 : 0);
        copy_str(T->txt, sizeof(T->txt), cJSON_GetObjectItem(m, "txt"));
        copy_str(T->val, sizeof(T->val), cJSON_GetObjectItem(m, "val"));
        copy_str(T->unit, sizeof(T->unit), cJSON_GetObjectItem(m, "unit"));
        T->lvl = (uint8_t)num_or(cJSON_GetObjectItem(m, "lvl"), 0);
        T->cur = num_or(cJSON_GetObjectItem(m, "cur"), NAN);
        T->tgt = num_or(cJSON_GetObjectItem(m, "tgt"), NAN);
        T->tmin = num_or(cJSON_GetObjectItem(m, "min"), 7);
        T->tmax = num_or(cJSON_GetObjectItem(m, "max"), 35);
        T->step = num_or(cJSON_GetObjectItem(m, "step"), 0.5f);
        copy_str(T->mode, sizeof(T->mode), cJSON_GetObjectItem(m, "mode"));
        copy_str(T->act, sizeof(T->act), cJSON_GetObjectItem(m, "act"));
        T->nmodes = 0;
        const cJSON *x;
        cJSON_ArrayForEach(x, cJSON_GetObjectItem(m, "modes")) {
            if (T->nmodes >= HUB_MAX_MODES) break;
            copy_str(T->modes[T->nmodes++], sizeof(T->modes[0]), x);
        }
        T->seq++;
    }
    hub_unlock();
}

static void on_message(const char *text)
{
    cJSON *m = cJSON_Parse(text);
    if (!m) { ESP_LOGW(TAG, "not JSON: %.40s", text); return; }
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(m, "t"));
    if (!t) {
    } else if (!strcmp(t, "state")) {
        on_state(m);
    } else if (!strcmp(t, "layout")) {
        on_layout(m);
    } else if (!strcmp(t, "identify")) {
        s_identify = (int)num_or(cJSON_GetObjectItem(m, "s"), 10);
    } else if (!strcmp(t, "error")) {
        hub_lock();
        copy_str(s_error, sizeof(s_error), cJSON_GetObjectItem(m, "msg"));
        hub_unlock();
        ESP_LOGW(TAG, "hub says: %s", s_error);
    }
    cJSON_Delete(m);
}

static void send_hello(void)
{
    char buf[240];
    const board_info_t *bd = board();
    const int n = snprintf(buf, sizeof(buf),
        "{\"t\":\"hello\",\"id\":\"%s\",\"name\":\"%s\",\"fw\":\"%s\",\"proto\":%d,"
        "\"w\":%d,\"h\":%d,\"board\":\"%s\"}",
        s_id, netcfg_hostname(), esp_app_get_description()->version, PROTO, bd->w, bd->h, bd->id);
    esp_websocket_client_send_text(s_ws, buf, n, pdMS_TO_TICKS(1000));
}

// Text messages can arrive in pieces (payload_offset > 0); put them back
// together before parsing.
static char  *s_rx;
static size_t s_rx_len;

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to %s", s_url);
        s_status = HUB_CONNECTING;      // ONLINE once the layout arrives
        send_hello();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_status == HUB_ONLINE || s_status == HUB_CONNECTING) ESP_LOGW(TAG, "hub connection lost");
        s_status = HUB_CONNECTING;
        s_has_layout = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code != 0x1) break;   // text only; pings and closes are handled for us
        if (d->payload_offset == 0) {
            free(s_rx);
            s_rx = NULL;
            s_rx_len = d->payload_len;
            if (d->payload_len > MAX_MSG) { ESP_LOGW(TAG, "message too big (%d)", d->payload_len); break; }
            s_rx = malloc(d->payload_len + 1);
            if (!s_rx) { ESP_LOGE(TAG, "no memory for a %d byte message", d->payload_len); break; }
        }
        if (!s_rx || d->payload_offset + d->data_len > (int)s_rx_len) break;
        memcpy(s_rx + d->payload_offset, d->data_ptr, d->data_len);
        if (d->payload_offset + d->data_len == (int)s_rx_len) {
            s_rx[s_rx_len] = 0;
            on_message(s_rx);
            free(s_rx);
            s_rx = NULL;
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- to the hub

static bool send_act(const char *entity, const char *action, const char *value_json)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return false;
    char buf[160];
    const int n = value_json
        ? snprintf(buf, sizeof(buf), "{\"t\":\"act\",\"e\":\"%s\",\"a\":\"%s\",\"v\":%s}", entity, action, value_json)
        : snprintf(buf, sizeof(buf), "{\"t\":\"act\",\"e\":\"%s\",\"a\":\"%s\"}", entity, action);
    if (n >= (int)sizeof(buf)) return false;
    return esp_websocket_client_send_text(s_ws, buf, n, pdMS_TO_TICKS(300)) >= 0;
}

// Apply a guess to every tile showing `entity`, so the screen answers now.
typedef void (*guess_fn)(hub_tile_t *t, const void *arg);
static void guess(const char *entity, guess_fn fn, const void *arg)
{
    hub_lock();
    for (int i = 0; i < s_layout.ntiles; i++) {
        hub_tile_t *T = &s_layout.tiles[i];
        if (!strcmp(T->entity, entity)) { fn(T, arg); T->seq++; }
    }
    hub_unlock();
}

static void g_toggle(hub_tile_t *t, const void *a)
{
    t->on = !t->on;
    if (t->on && t->bri == 0) t->bri = 100;
    t->txt[0] = 0;                      // the hub's words were about the old state
}
static void g_bri(hub_tile_t *t, const void *a) { t->bri = *(const int *)a; t->on = t->bri > 0; t->txt[0] = 0; }
static void g_tgt(hub_tile_t *t, const void *a) { t->tgt = *(const float *)a; }
static void g_mode(hub_tile_t *t, const void *a) { snprintf(t->mode, sizeof(t->mode), "%s", (const char *)a); }

bool hub_toggle(const char *entity)
{
    if (!send_act(entity, "toggle", NULL)) return false;
    guess(entity, g_toggle, NULL);
    return true;
}

bool hub_brightness(const char *entity, int pct)
{
    char v[8];
    snprintf(v, sizeof(v), "%d", pct);
    if (!send_act(entity, "bri", v)) return false;
    guess(entity, g_bri, &pct);
    return true;
}

bool hub_target(const char *entity, float temp)
{
    char v[16];
    snprintf(v, sizeof(v), "%.1f", temp);
    if (!send_act(entity, "target", v)) return false;
    guess(entity, g_tgt, &temp);
    return true;
}

bool hub_mode(const char *entity, const char *mode)
{
    char v[16];
    if (snprintf(v, sizeof(v), "\"%s\"", mode) >= (int)sizeof(v)) return false;
    if (!send_act(entity, "mode", v)) return false;
    guess(entity, g_mode, mode);
    return true;
}

bool hub_press(const char *entity)
{
    return send_act(entity, "press", NULL);         // nothing to guess: the hub's state says if it runs
}

// ---------------------------------------------------------------- finding the hub

// Ask the LAN for _hadisplay._tcp. Fills s_url; false if nothing answered.
static bool discover(void)
{
    mdns_result_t *res = NULL;
    if (mdns_query_ptr("_hadisplay", "_tcp", 3000, 4, &res) != ESP_OK || !res) return false;
    bool found = false;
    for (const mdns_result_t *r = res; r && !found; r = r->next) {
        esp_ip4_addr_t ip = { 0 };
        for (const mdns_ip_addr_t *a = r->addr; a; a = a->next)
            if (a->addr.type == ESP_IPADDR_TYPE_V4) { ip = a->addr.u_addr.ip4; break; }
        if (!ip.addr && r->hostname) mdns_query_a(r->hostname, 2000, &ip);
        if (!ip.addr || !r->port) continue;
        const char *path = "/ws";
        for (size_t i = 0; i < r->txt_count; i++)
            if (!strcmp(r->txt[i].key, "path") && r->txt[i].value) path = r->txt[i].value;
        snprintf(s_url, sizeof(s_url), "ws://" IPSTR ":%u%s", IP2STR(&ip), r->port, path);
        ESP_LOGI(TAG, "found \"%s\" at %s", r->instance_name ? r->instance_name : "hub", s_url);
        found = true;
    }
    mdns_query_results_free(res);
    return found;
}

static void stop_client(void)
{
    if (!s_ws) return;
    esp_websocket_client_destroy(s_ws);     // stops it too
    s_ws = NULL;
    free(s_rx);
    s_rx = NULL;
    s_has_layout = false;
}

static bool start_client(void)
{
    const esp_websocket_client_config_t cfg = {
        .uri = s_url,
        .task_stack = 5120,
        .task_prio = 4,
        .buffer_size = 1024,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 45,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) { ESP_LOGE(TAG, "no memory for the WebSocket client"); return false; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) { stop_client(); return false; }
    s_status = HUB_CONNECTING;
    return true;
}

static void hub_task(void *arg)
{
    for (;;) {
        if (!netcfg_connected()) {
            if (s_ws && s_status != HUB_NO_WIFI) ESP_LOGI(TAG, "WiFi down");
            s_status = HUB_NO_WIFI;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        // Where to: a saved address, then the build-time default, then mDNS.
        s_url_changed = false;
        hub_get_url(s_url, sizeof(s_url));
        bool from_mdns = false;
        if (!s_url[0]) snprintf(s_url, sizeof(s_url), "%s", CONFIG_HAD_HUB_URL);
        if (!s_url[0]) {
            s_status = HUB_SEARCHING;
            if (!discover()) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
            from_mdns = true;
        }
        if (!start_client()) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }

        // The client reconnects by itself; step in only when the address
        // changed, or when a discovered hub has been gone a while.
        int offline_s = 0;
        while (!s_url_changed) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (!netcfg_connected()) { s_status = HUB_NO_WIFI; offline_s = 0; continue; }
            if (esp_websocket_client_is_connected(s_ws)) { offline_s = 0; continue; }
            if (s_status == HUB_NO_WIFI) s_status = HUB_CONNECTING;
            if (from_mdns && ++offline_s >= REDISCOVER_S) { ESP_LOGI(TAG, "looking for the hub again"); break; }
        }
        stop_client();
        hub_lock();
        s_layout.npages = s_layout.ntiles = 0;
        snprintf(s_layout.msg, sizeof(s_layout.msg), "%s", "");
        s_layout.gen++;
        hub_unlock();
    }
}

// ---------------------------------------------------------------- start

esp_err_t hub_start(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof(s_id), "cyd-%02x%02x%02x", mac[3], mac[4], mac[5]);
    lock_handle();
    ESP_LOGI(TAG, "display id %s", s_id);
    hub_web_start();
    return xTaskCreate(hub_task, "hub", 4096, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
