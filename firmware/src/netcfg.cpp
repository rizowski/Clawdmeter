#include "netcfg.h"
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

static bool copy_field(JsonDocument& doc, const char* key, char* dst, size_t cap, bool allow_empty) {
    const char* v = doc[key] | (const char*)nullptr;
    if (!v) return false;
    size_t len = strlen(v);
    if (len >= cap || (!allow_empty && len == 0)) return false;
    memcpy(dst, v, len + 1);
    return true;
}

static bool is_hex16(const char* s) {
    if (strlen(s) != 16) return false;
    for (const char* p = s; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    }
    return true;
}

bool netcfg_is_valid(const NetConfig* c) {
    return c->ssid[0] != '\0' && is_hex16(c->broker_id) && c->pass[0] != '\0';
}

bool netcfg_parse(const char* json, NetConfig* out) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    NetConfig c = {};
    if (!copy_field(doc, "ssid", c.ssid, sizeof c.ssid, false)) return false;
    if (!copy_field(doc, "wpass", c.wpass, sizeof c.wpass, true)) return false;
    if (!copy_field(doc, "broker_id", c.broker_id, sizeof c.broker_id, false)) return false;
    if (!copy_field(doc, "pass", c.pass, sizeof c.pass, false)) return false;
    if (!netcfg_is_valid(&c)) return false;
    *out = c;
    return true;
}

bool prov_session_begin(ProvSession* s, uint32_t rand32, uint32_t now_ms) {
    if (s->locked) {
        if ((int32_t)(now_ms - s->lock_until_ms) < 0) return false;
        s->locked = false;
        s->fails = 0;   // lockout served: a fresh budget
    }
    s->active = true;
    snprintf(s->code, sizeof s->code, "%06lu", (unsigned long)(rand32 % 1000000u));
    s->expires_ms = now_ms + PROV_CODE_TTL_MS;
    return true;
}

bool prov_session_visible(const ProvSession* s, uint32_t now_ms) {
    return s->active && (int32_t)(now_ms - s->expires_ms) < 0;
}

prov_result_t prov_session_apply(ProvSession* s, const char* json, uint32_t now_ms, NetConfig* out) {
    if (!prov_session_visible(s, now_ms)) {
        s->active = false;
        return PROV_NOT_ACTIVE;
    }
    JsonDocument doc;
    if (deserializeJson(doc, json)) return PROV_BAD_JSON;
    const char* code = doc["code"] | "";
    if (strcmp(code, s->code) != 0) {
        if (++s->fails >= PROV_MAX_FAILS) {
            s->active = false;
            s->locked = true;
            s->lock_until_ms = now_ms + PROV_LOCKOUT_MS;
            return PROV_LOCKED;
        }
        return PROV_BAD_CODE;
    }
    if (!netcfg_parse(json, out)) return PROV_BAD_JSON;
    s->active = false;
    s->fails = 0;
    return PROV_OK;
}

void net_up_arm(NetUpGate* g, uint32_t config_gen) {
    g->want = config_gen;
    g->armed = true;
}

bool net_up_take(NetUpGate* g, uint32_t connected_gen) {
    if (!g->armed || connected_gen == 0 || connected_gen != g->want) return false;
    g->armed = false;
    return true;
}

void netcfg_device_id(const uint8_t base_mac[6], char out[7]) {
    snprintf(out, 7, "%02x%02x%02x", base_mac[3], base_mac[4], base_mac[5]);
}

void netcfg_topic(char* out, size_t n, const char* device_id, const char* leaf) {
    snprintf(out, n, "clawdmeter/%s/%s", device_id, leaf);
}
