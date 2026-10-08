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

void prov_session_begin(ProvSession* s, uint32_t rand32, uint32_t now_ms) {
    s->active = true;
    snprintf(s->code, sizeof s->code, "%06lu", (unsigned long)(rand32 % 1000000u));
    s->expires_ms = now_ms + PROV_CODE_TTL_MS;
    s->fails = 0;
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
            return PROV_LOCKED;
        }
        return PROV_BAD_CODE;
    }
    if (!netcfg_parse(json, out)) return PROV_BAD_JSON;
    s->active = false;
    return PROV_OK;
}

void netcfg_topic(char* out, size_t n, const char* device_id, const char* leaf) {
    snprintf(out, n, "clawdmeter/%s/%s", device_id, leaf);
}
