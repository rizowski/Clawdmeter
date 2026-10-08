// prov.h stub for the desktop simulator: no NVS, no WiFi — the sim gets its
// data from scenario playback (ble_sim.cpp), so provisioning is always off.
#include "../../prov.h"
#include <stdio.h>

static NetConfig empty_cfg = {};

void prov_init(void) {}
const char* prov_device_id(void) { return "000000"; }
bool prov_is_configured(void) { return false; }
const NetConfig* prov_config(void) { return &empty_cfg; }
bool prov_handle_write(const char*, char* resp, size_t n) {
    snprintf(resp, n, "{\"err\":\"nowifi\"}");
    return false;
}
bool prov_apply_unchecked(const char*) { return false; }
bool prov_code_visible(void) { return false; }
const char* prov_code(void) { return ""; }
void prov_clear(void) {}
bool prov_take_net_up_pending(void) { return false; }
