#pragma once
// Provisioning: the WiFi + MQTT config clawdmeter-setup pushes over the BLE
// PROV characteristic, persisted in NVS (namespace "clawdnet"). All calls are
// loop-task only — ble.cpp hands PROV writes over via ble_has_prov().
#include <stddef.h>
#include "netcfg.h"

void prov_init(void);
const char* prov_device_id(void);          // 6 lowercase hex — also the MQTT username
bool prov_is_configured(void);
const NetConfig* prov_config(void);

// Handles one PROV message ({"op":"begin"} / {"op":"config",...}); writes the
// JSON reply to notify into resp. Returns true when a new config was saved.
bool prov_handle_write(const char* json, char* resp, size_t resp_n);
// Serial dev path: save a config without a setup code. USB access already
// means physical control of the device (it can be reflashed), so this adds no
// new attack surface.
bool prov_apply_unchecked(const char* json);

bool prov_code_visible(void);
const char* prov_code(void);
void prov_clear(void);                      // part of the hold-to-pair reset
