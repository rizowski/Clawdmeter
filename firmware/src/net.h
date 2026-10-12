#pragma once
#include <stdint.h>
// WiFi + MQTT transport for usage data. Mirrors ble.h's polling shape so
// main.cpp treats both sources alike. Compiles to stubs without BOARD_HAS_WIFI
// (C6 boards, sim).

enum net_state_t {
    NET_OFF,          // not configured (or no WiFi on this board)
    NET_WIFI,         // joining the WiFi network
    NET_DISCOVERING,  // on WiFi, looking for / connecting to the broker
    NET_CONNECTED,    // MQTT session up
    NET_AUTH_FAIL,    // broker rejected our login — re-run clawdmeter-setup
};

void net_init(void);
// Re-reads prov_config(); returns the new config generation (see NetUpGate).
uint32_t net_apply_config(void);
net_state_t net_get_state(void);
// Generation of the config the live MQTT session was started from; 0 when down.
uint32_t net_connected_generation(void);
bool net_has_data(void);
const char* net_get_data(void);
