// http://<name>.local/hub: this display's id, how the hub link is doing,
// and the hub address, for when mDNS cannot find it.
#include "hub.h"
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "netcfg.h"

static const char PAGE[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
"<title>Hub</title><style>body{font:16px system-ui;margin:16px;max-width:32em}"
"input{font:inherit;width:100%;margin:.3em 0}code{font-size:1.1em}</style>"
"<h2>Hub</h2><p>This display is <code id=id></code>. Look for it in the hub's admin page.</p>"
"<p id=st></p><h3>Hub address</h3>"
"<p>Leave blank to find the hub automatically (mDNS).</p>"
"<form id=f><input id=u placeholder='ws://192.168.1.20:8390/ws'>"
"<button>Save</button></form><p id=msg></p><p><a href=/>WiFi setup</a></p>"
"<p style='font-size:12px;color:#888'>&copy; 2026 Chris Collins &middot; MIT License</p>"
"<script>const $=i=>document.getElementById(i);"
"async function load(){const j=await(await fetch('/api/hub')).json();$('id').textContent=j.id;"
"$('st').textContent=j.status;if(document.activeElement!==$('u'))$('u').value=j.url}"
"$('f').onsubmit=async e=>{e.preventDefault();const j=await(await fetch('/api/hub',"
"{method:'POST',body:JSON.stringify({url:$('u').value.trim()})})).json();"
"$('msg').textContent=j.ok?'Saved. Reconnecting...':j.err;load()};load();setInterval(load,3000)</script>";

static esp_err_t h_page(httpd_req_t *r) { return httpd_resp_send(r, PAGE, sizeof(PAGE) - 1); }

static esp_err_t reply(httpd_req_t *r, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return httpd_resp_send_500(r);
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, s);
    free(s);
    return e;
}

static esp_err_t h_get(httpd_req_t *r)
{
    char status[96], url[96];
    hub_status_text(status, sizeof(status));
    hub_get_url(url, sizeof(url));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", hub_device_id());
    cJSON_AddStringToObject(j, "status", status);
    cJSON_AddStringToObject(j, "url", url);
    cJSON_AddBoolToObject(j, "online", hub_status() == HUB_ONLINE);
    return reply(r, j);
}

// {"url":"ws://..."} saves, {"url":""} goes back to finding it.
static esp_err_t h_post(httpd_req_t *r)
{
    char body[160];
    const int len = r->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad body");
    for (int got = 0; got < len;) {
        const int k = httpd_req_recv(r, body + got, len - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[len] = 0;
    cJSON *in = cJSON_Parse(body);
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(in, "url"));
    const esp_err_t e = url ? hub_set_url(url) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(in);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", e == ESP_OK);
    if (e != ESP_OK)
        cJSON_AddStringToObject(j, "err", e == ESP_ERR_INVALID_ARG ? "the address must start with ws:// or wss://"
                                        : e == ESP_ERR_INVALID_SIZE ? "that address is too long" : "could not save it");
    return reply(r, j);
}

void hub_web_start(void)
{
    httpd_handle_t srv = netcfg_httpd();
    if (!srv) return;
    const httpd_uri_t u[] = {
        { .uri = "/hub",     .method = HTTP_GET,  .handler = h_page },
        { .uri = "/api/hub", .method = HTTP_GET,  .handler = h_get  },
        { .uri = "/api/hub", .method = HTTP_POST, .handler = h_post },
    };
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++)
        if (httpd_register_uri_handler(srv, &u[i]) != ESP_OK) ESP_LOGW("hub", "could not add %s", u[i].uri);
}
