#include "prov.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_random.h>
#include <esp_mac.h>

static NetConfig   cfg = {};
static bool        configured = false;
static ProvSession sess = {};
static char        dev_id[7];
static bool        net_up_pending = false;

static void load(void) {
    Preferences p;
    p.begin("clawdnet", true);
    p.getString("ssid", cfg.ssid, sizeof cfg.ssid);
    p.getString("wpass", cfg.wpass, sizeof cfg.wpass);
    p.getString("broker", cfg.broker_id, sizeof cfg.broker_id);
    p.getString("pass", cfg.pass, sizeof cfg.pass);
    p.end();
    configured = netcfg_is_valid(&cfg);
}

static void save(void) {
    Preferences p;
    p.begin("clawdnet", false);
    p.putString("ssid", cfg.ssid);
    p.putString("wpass", cfg.wpass);
    p.putString("broker", cfg.broker_id);
    p.putString("pass", cfg.pass);
    p.end();
}

static void adopt(const NetConfig& next) {
    cfg = next;
    configured = true;
    save();
    net_up_pending = true;
    Serial.printf("PROV: saved config (ssid=%s broker=%s)\n", cfg.ssid, cfg.broker_id);
}

void prov_init(void) {
    // Last 3 bytes of the factory base MAC — stable per chip.
    uint8_t mac[6];
    // WiFi-STA MAC, not esp_efuse_mac_get_default(): on the C6 the latter
    // returns the first 6 bytes of its EUI-64 (…ff:fe…), measured on hardware.
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    netcfg_device_id(mac, dev_id);
    load();
    Serial.printf("PROV: device id %s, %s\n", dev_id,
                  configured ? "WiFi configured" : "no WiFi config");
}

const char* prov_device_id(void) { return dev_id; }
bool prov_is_configured(void) { return configured; }
const NetConfig* prov_config(void) { return &cfg; }

bool prov_handle_write(const char* json, char* resp, size_t resp_n) {
#ifndef BOARD_HAS_WIFI
    (void)json;
    snprintf(resp, resp_n, "{\"err\":\"nowifi\"}");
    return false;
#else
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        snprintf(resp, resp_n, "{\"err\":\"json\"}");
        return false;
    }
    const char* op = doc["op"] | "";
    if (strcmp(op, "begin") == 0) {
        if (!prov_session_begin(&sess, esp_random(), millis())) {
            snprintf(resp, resp_n, "{\"err\":\"locked\"}");   // too many wrong codes; wait a minute
            return false;
        }
        Serial.println("PROV: setup code shown");
        snprintf(resp, resp_n, "{\"id\":\"%s\"}", dev_id);
        return false;
    }
    if (strcmp(op, "config") == 0) {
        NetConfig next = {};
        switch (prov_session_apply(&sess, json, millis(), &next)) {
        case PROV_OK:
            adopt(next);
            snprintf(resp, resp_n, "{\"ok\":true}");
            return true;
        case PROV_BAD_CODE:   snprintf(resp, resp_n, "{\"err\":\"code\"}");    return false;
        case PROV_LOCKED:     snprintf(resp, resp_n, "{\"err\":\"locked\"}");  return false;
        case PROV_NOT_ACTIVE: snprintf(resp, resp_n, "{\"err\":\"expired\"}"); return false;
        case PROV_BAD_JSON:   snprintf(resp, resp_n, "{\"err\":\"json\"}");    return false;
        }
    }
    snprintf(resp, resp_n, "{\"err\":\"op\"}");
    return false;
#endif
}

bool prov_apply_unchecked(const char* json) {
#ifndef BOARD_HAS_WIFI
    (void)json;
    return false;   // nothing would ever use it
#endif
    NetConfig next = {};
    if (!netcfg_parse(json, &next)) return false;
    adopt(next);
    return true;
}

bool prov_code_visible(void) { return prov_session_visible(&sess, millis()); }
const char* prov_code(void) { return sess.code; }

void prov_clear(void) {
    cfg = {};
    configured = false;
    sess.active = false;
    net_up_pending = false;
    Preferences p;
    p.begin("clawdnet", false);
    p.clear();
    p.end();
    Serial.println("PROV: WiFi config cleared");
}

bool prov_take_net_up_pending(void) {
    bool v = net_up_pending;
    net_up_pending = false;
    return v;
}
