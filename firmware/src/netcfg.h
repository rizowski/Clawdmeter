#pragma once
// WiFi + MQTT settings pushed by clawdmeter-setup over the BLE PROV
// characteristic, plus the setup-code session that gates them. Pure logic, no
// Arduino deps — host-tested by test/test_netcfg.
#include <stddef.h>
#include <stdint.h>

struct NetConfig {
    char ssid[33];       // 802.11 SSID max 32 bytes
    char wpass[65];      // WPA2 passphrase max 63 (or 64-hex PSK); empty = open network
    char broker_id[17];  // 16 hex chars from broker-init.sh (matches the mDNS TXT id)
    char pass[33];       // MQTT password; username is the device id
};

bool netcfg_is_valid(const NetConfig* c);
// Fills *out from {"ssid","wpass","broker_id","pass"}. Rejects missing or
// over-long fields instead of truncating — a cut password fails silently later.
bool netcfg_parse(const char* json, NetConfig* out);

enum prov_result_t { PROV_OK, PROV_BAD_CODE, PROV_LOCKED, PROV_NOT_ACTIVE, PROV_BAD_JSON };

struct ProvSession {
    bool     active;
    char     code[7];
    uint32_t expires_ms;
    uint8_t  fails;          // wrong codes since the last success — survives a new begin
    bool     locked;
    uint32_t lock_until_ms;
};

static const uint32_t PROV_CODE_TTL_MS = 120000;
static const uint8_t  PROV_MAX_FAILS   = 5;
static const uint32_t PROV_LOCKOUT_MS  = 60000;  // after PROV_MAX_FAILS wrong codes

// Shows a fresh code. Returns false (and shows nothing) during a lockout, so a
// client can't refill its guess budget by re-requesting codes.
bool prov_session_begin(ProvSession* s, uint32_t rand32, uint32_t now_ms);
bool prov_session_visible(const ProvSession* s, uint32_t now_ms);
// Checks the message's "code" against the session; on a match parses the
// config into *out and ends the session. Bad JSON doesn't count as a wrong code.
prov_result_t prov_session_apply(ProvSession* s, const char* json, uint32_t now_ms, NetConfig* out);

// "net up" for clawdmeter-setup, gated on config generation: each applied
// config gets a new generation, and only an MQTT session started from that
// generation counts. Without this a re-provision while connected reported the
// old session as success for the new (possibly wrong) WiFi password.
struct NetUpGate {
    uint32_t want;
    bool     armed;
};
void net_up_arm(NetUpGate* g, uint32_t config_gen);
// connected_gen: generation of the live MQTT session, 0 when not connected.
bool net_up_take(NetUpGate* g, uint32_t connected_gen);

// millis()-based deadline that survives the 49.7-day wrap. Always arm it from
// the current time: a deadline left at 0 reads as "in the future" for the
// whole stretch where (int32_t)millis() is negative (24.8-49.7 days uptime).
struct NetTimer {
    uint32_t due_ms;
};
void net_timer_arm(NetTimer* t, uint32_t now_ms, uint32_t delay_ms);
bool net_timer_due(const NetTimer* t, uint32_t now_ms);

// Device id = last 3 bytes of the 6-byte base MAC, lowercase hex. Callers pass
// the 6-byte WiFi-STA MAC (esp_read_mac(..., ESP_MAC_WIFI_STA)), not the C6's
// 8-byte EUI-64, which has an ff:fe filler in the middle.
void netcfg_device_id(const uint8_t base_mac[6], char out[7]);

// An mDNS answer can arrive before its A record resolves (address 0.0.0.0).
// Connecting there fails until the next rediscover, so skip such answers.
bool netcfg_broker_usable(uint32_t ipv4, uint16_t port);

// "clawdmeter/<device_id>/<leaf>"
void netcfg_topic(char* out, size_t n, const char* device_id, const char* leaf);
